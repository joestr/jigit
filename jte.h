#include <time.h>
#include <sys/time.h>

typedef long long INT64;
typedef unsigned long long UINT64;
typedef unsigned long      UINT32;

#ifndef LLONG_MAX
#   define LLONG_MAX (INT64)INT_MAX * INT_MAX
#endif

#define BUF_SIZE 65536 * 8 
#define MISSING -1

#ifndef MIN
#define MIN(x,y)        ( ((x) < (y)) ? (x) : (y))
#endif

typedef struct _jd_time
{
    struct timeval tv;
    struct timezone tz;
} jd_time_t;

int   jd_timer_init (jd_time_t *timer);
INT64 jd_timer_count (jd_time_t *timer);

#define JD void

/* Limited FS-like interface to an ISO image */
int jd_init(int cache_size);
JD *jd_open(JIGDB *dbp, char *template_file);
int jd_read(JD *state, INT64 start_offset, INT64 length, unsigned char *buffer, INT64 *bytes_read);
int jd_md5(JD *state, unsigned char md5[16]);
int jd_size(JD *state, INT64 *size);
int jd_last_filename(JD *state, char **name);
int jd_close(JD *state);

/* Must be implemented by the caller */
int jd_log(int verbose_level, char *fmt, ...);

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

/* Interface functions */
void display_progress(int verbose_level, INT64 image_size, INT64 current_offset, char *text);
extern void file_missing(char *missing, char *filename);

/* decompress.c */
int decompress_data_block(char *in_buf, INT64 in_len, char *out_buf,
                          INT64 out_len, int compress_type);

/* parse_jigdo.c */
int parse_jigdo_file(char *filename, md5_list_t **md5_list_head, match_list_t *match_list_head, char *missing_filename);
int parse_md5_file(char *filename, md5_list_t **md5_list_head);
md5_list_t *find_file_in_md5_list(unsigned char *base64_md5, md5_list_t **md5_list_head);
INT64 get_file_size(char *filename);
time_t get_file_mtime(char *filename);

/* parse_template.c */
int add_new_template_file(JIGDB *dbp, char *filename);
int parse_template_file(char *filename, int sizeonly, int no_md5, char *missing,
                        FILE *outfile, char *output_name, JIGDB *dbp,
                        md5_list_t **md5_list_head, FILE *missing_file,
                        UINT64 start_offset, UINT64 end_offset);



