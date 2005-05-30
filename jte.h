typedef long long INT64;
typedef unsigned long long UINT64;
typedef unsigned long      UINT32;

#ifndef LLONG_MAX
#   define LLONG_MAX (INT64)INT_MAX * INT_MAX
#endif

#define BUF_SIZE 65536
#define MISSING -1

#ifndef MIN
#define MIN(x,y)        ( ((x) < (y)) ? (x) : (y))
#endif

#define JD void

/* Limited FS-like interface to an ISO image */
JD *jd_open(JIGDB *dbp, char *template_file);
int jd_read(JD *state, INT64 start_offset, INT64 length, unsigned char *buffer, INT64 *bytes_read);
int jd_md5(JD *state, unsigned char md5[16]);
int jd_size(JD *state, INT64 *size);
int jd_last_filename(JD *state, char **name);
int jd_close(JD *state);

typedef struct match_list_
{
    struct match_list_ *next;
    char *match;
    char *mirror_path;
} match_list_t;

typedef struct md5_list_
{
    struct md5_list_ *next;
    INT64 file_size;
    char *md5;
    char *full_path;
} md5_list_t;

extern match_list_t *G_match_list_head;
extern match_list_t *G_match_list_tail;
extern md5_list_t *G_md5_list_head;
extern md5_list_t *G_md5_list_tail;
extern FILE *G_logfile;
extern FILE *G_missing_file;
extern char *G_missing_filename;
extern int G_verbose;
extern int G_quick;
extern UINT64 G_out_size;
extern long long G_start_offset;
extern long long G_end_offset;

/* Interface functions */
extern void display_progress(FILE *file, char *text);
extern void file_missing(char *missing, char *filename);

/* decompress.c */
int decompress_data_block(char *in_buf, INT64 in_len, char *out_buf,
                          INT64 out_len, int compress_type);

/* parse_jigdo.c */
int parse_jigdo_file(char *filename);
int parse_md5_file(char *filename);
md5_list_t *find_file_in_md5_list(unsigned char *base64_md5);
INT64 get_file_size(char *filename);
time_t get_file_mtime(char *filename);

/* parse_template.c */
int add_new_template_file(JIGDB *dbp, char *filename);
int parse_template_file(char *filename, int sizeonly, char *missing,
                        FILE *outfile, char *output_name, JIGDB *dbp);



