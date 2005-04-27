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

typedef struct match_list_
{
    struct match_list_ *next;
    char *match;
    char *mirror_path;
} match_list_t;

extern match_list_t *match_list_head;
extern match_list_t *match_list_tail;

typedef struct md5_list_
{
    struct md5_list_ *next;
    INT64 file_size;
    char *md5;
    char *full_path;
} md5_list_t;

extern md5_list_t *md5_list_head;
extern md5_list_t *md5_list_tail;
extern FILE *logfile;
extern char *missing_filename;

int parse_jigdo_file(char *filename);
int parse_md5_file(char *filename);
md5_list_t *find_file_in_md5_list(unsigned char *base64_md5);
INT64 get_file_size(char *filename);

int decompress_data_block(char *in_buf, INT64 in_len, char *out_buf,
                          INT64 out_len, int compress_type);


