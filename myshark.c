#define HAVE_STDARG_H 1
#define WS_MSVC_NORETURN
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <wireshark/epan/epan.h>
#include <wireshark/epan/print.h>
#include <wireshark/epan/timestamp.h>
#include <wireshark/epan/prefs.h>
#include <wireshark/epan/column.h>
#include <wireshark/epan/epan-int.h>
#include <wireshark/wsutil/privileges.h>
#include <wireshark/epan/epan_dissect.h>
#include <wireshark/epan/proto.h>
#include <wireshark/epan/ftypes/ftypes.h>
#include <wireshark/epan/asm_utils.h>

#define BUFSIZE 2048 * 100

extern tvbuff_t *frame_tvbuff_new(const frame_data *fd, const guint8 *buf);
static void timestamp_set(capture_file cfile);
static const nstime_t *tshark_get_frame_ts(void *data, guint32 frame_num);
static void clean();

typedef enum {
	PRINT_XML,
	PRINT_TEXT,
}print_type_t;

//global variable
capture_file cfile;

e_prefs *get_prefs()
{
	e_prefs     *prefs_p;
	char        *gpf_path, *pf_path;
	int          gpf_read_errno, gpf_open_errno;
	int          pf_open_errno, pf_read_errno;

	prefs_p = read_prefs(&gpf_open_errno, &gpf_read_errno, &gpf_path,
			&pf_open_errno, &pf_read_errno, &pf_path);
	return prefs_p;
}

int init(char *filename)
{
	int          err = 0;
	gchar       *err_info = NULL;
	e_prefs     *prefs_p;

	init_process_policies();

	epan_init(register_all_protocols, register_all_protocol_handoffs, NULL, NULL);

	cap_file_init(&cfile);
	cfile.filename = filename;

	cfile.wth = wtap_open_offline(cfile.filename, WTAP_TYPE_AUTO, &err, &err_info, TRUE);
	if (cfile.wth == NULL)
		goto fail;

	cfile.count = 0;
	cfile.epan = epan_new();
	cfile.epan->data = &cfile;
	cfile.epan->get_frame_ts = tshark_get_frame_ts;

	timestamp_set(cfile);
	cfile.frames = new_frame_data_sequence();

	prefs_p = get_prefs();

	build_column_format_array(&cfile.cinfo, prefs_p->num_cols, TRUE);

	return 0;

fail:
	clean();
	return err;
}

gboolean read_packet(epan_dissect_t **edt_r)
{
	epan_dissect_t    *edt;
	int                err;
	gchar             *err_info = NULL;
	static guint32     cum_bytes = 0;
	static gint64      data_offset = 0;

	struct wtap_pkthdr *whdr = wtap_phdr(cfile.wth);
	guchar             *buf = wtap_buf_ptr(cfile.wth);

	if (wtap_read(cfile.wth, &err, &err_info, &data_offset)) {

		cfile.count++;

		frame_data fdlocal;
		frame_data_init(&fdlocal, cfile.count, whdr, data_offset, cum_bytes);

		edt = epan_dissect_new(cfile.epan, TRUE, TRUE);

		frame_data_set_before_dissect(&fdlocal, &cfile.elapsed_time, &cfile.ref, cfile.prev_dis);
		cfile.ref = &fdlocal;

		epan_dissect_run(edt, cfile.cd_t, &(cfile.phdr), frame_tvbuff_new(&fdlocal, buf), &fdlocal, &cfile.cinfo);

		frame_data_set_after_dissect(&fdlocal, &cum_bytes);
		cfile.prev_cap = cfile.prev_dis = frame_data_sequence_add(cfile.frames, &fdlocal);

		//free space
		frame_data_destroy(&fdlocal);

		*edt_r = edt;
		return TRUE;
	}
	return FALSE;
}

void clean()
{
	if (cfile.frames != NULL) {
		free_frame_data_sequence(cfile.frames);
		cfile.frames = NULL;
	}

	if (cfile.wth != NULL) {
		wtap_close(cfile.wth);
		cfile.wth = NULL;
	}

	if (cfile.epan != NULL)
		epan_free(cfile.epan);

	epan_cleanup();
}

static void
timestamp_set(capture_file cfile)
{
	switch(wtap_file_tsprecision(cfile.wth)) {
		case(WTAP_FILE_TSPREC_SEC):
			timestamp_set_precision(TS_PREC_AUTO_SEC);
			break;
		case(WTAP_FILE_TSPREC_DSEC):
			timestamp_set_precision(TS_PREC_AUTO_DSEC);
			break;
		case(WTAP_FILE_TSPREC_CSEC):
			timestamp_set_precision(TS_PREC_AUTO_CSEC);
			break;
		case(WTAP_FILE_TSPREC_MSEC):
			timestamp_set_precision(TS_PREC_AUTO_MSEC);
			break;
		case(WTAP_FILE_TSPREC_USEC):
			timestamp_set_precision(TS_PREC_AUTO_USEC);
			break;
		case(WTAP_FILE_TSPREC_NSEC):
			timestamp_set_precision(TS_PREC_AUTO_NSEC);
			break;
		default:
			g_assert_not_reached();
	}
}

static const nstime_t *
tshark_get_frame_ts(void *data, guint32 frame_num)
{
  capture_file *cf = (capture_file *) data;

  if (cf->ref && cf->ref->num == frame_num)
    return &(cf->ref->abs_ts);

  if (cf->prev_dis && cf->prev_dis->num == frame_num)
    return &(cf->prev_dis->abs_ts);

  if (cf->prev_cap && cf->prev_cap->num == frame_num)
    return &(cf->prev_cap->abs_ts);

  if (cf->frames) {
     frame_data *fd = frame_data_sequence_find(cf->frames, frame_num);

     return (fd) ? &fd->abs_ts : NULL;
  }

  return NULL;
}

void
cap_file_init(capture_file *cf)
{
	/* Initialize the capture file struct */
	memset(cf, 0, sizeof(capture_file));
	cf->snap            = WTAP_MAX_PACKET_SIZE;
}

void print_usage(char *argv[])
{
	printf("Usage: %s -f <input_file>\n", argv[0]);
}

static void
print_field(proto_node *node, int *level, char **buf)
{
	if (node->finfo == NULL)
		return;

	//reset level when node is proto
        if (node->finfo->hfinfo->type == FT_PROTOCOL)
		*level = 0;

	for (int i = 0; i < *level; i++) {
		snprintf(*buf + strlen(*buf), BUFSIZE, "%s", ". ");
	}

	const char *name = node->finfo->hfinfo->abbrev;

	fvalue_t fv = node->finfo->value;
	char *value = fvalue_to_string_repr(&fv, FTREPR_DISPLAY, NULL);

	if (value == NULL) {
		snprintf(*buf + strlen(*buf), BUFSIZE, "[%s]\n", name);
	} else {
		snprintf(*buf + strlen(*buf), BUFSIZE, "[%s] %s\n", name, value);
	}
}

static void
proto_node_print(proto_tree *tree, int *level, char **buf)
{
	proto_node *node = tree;
	proto_node *current;

	if (!node)
		return;

	node = node->first_child;
	while (node != NULL) {
		current = node;
		node    = current->next;

		print_field(current, level, buf);

		(*level)++;
		proto_node_print(current, level, buf);
		(*level)--;
	}
}

void print_node(epan_dissect_t *edt)
/*void print_node(proto_node *node)*/
{
	char *buf = calloc(sizeof(char), BUFSIZE);
	int level = 0;
	proto_tree *node = edt->tree;

	print_field(node, &level, &buf);

	level++;
	proto_node_print(node, &level, &buf);

	printf("%s\n", buf);

	free(buf);
}

void print_each_packet_self_format()
{
	epan_dissect_t *edt;

	while (read_packet(&edt)) {

		print_node(edt);

		epan_dissect_free(edt);
		edt = NULL;
	}
}

int main(int argc, char* argv[])
{

	int          err;
	char        *filename = NULL;
	int          opt;

	while((opt = getopt(argc, argv, "f:")) != -1) {
		switch(opt) {
		case 'f':
			filename = calloc(sizeof(char), strlen(optarg) + 1);
			strncpy(filename, optarg, strlen(optarg));
			break;
		default:
			print_usage(argv);
			return 1;
		}
	}

	if (filename == NULL) {
		print_usage(argv);
		return 1;
	}

	err = init(filename);
	if (err) {
		return 1;
	}

	print_each_packet_self_format();

	clean();
	return 0;
}
