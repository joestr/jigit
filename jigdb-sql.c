#include <sqlite3.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include "jigdb.h"

typedef struct
{
    sqlite3 *db;
} db_state_t;

struct results
{
    struct results *next;
    struct results *prev;
    db_file_entry_t entry;
};

struct results *res_head = NULL;
struct results *res_current = NULL;
struct results *res_tail = NULL;

char sql_command[2 * PATH_MAX];

JIGDB *db_open(char *db_name)
{
    db_state_t *dbp = NULL;
    int error = 0;            /* function return value */
    char *open_error;

    /* Allocate state structure */
    dbp = calloc(1, sizeof(*dbp));
    if (dbp)
    {
        error = sqlite3_open(db_name, &dbp->db);
        if (error)
        {
            fprintf(stderr, "Unable to open sqlite file %s: error %d\n", db_name, error);
            errno = error;
            return NULL;
        }
        
        /* We have a database pointer open. Do we need to init the
         * "files" table? Try to grab the first row of the table and
         * see if we get an error. There has to be a better way than
         * this! */
        error = sqlite3_exec(dbp->db, "SELECT COUNT(*) FROM files;", NULL, NULL, NULL);
        if (SQLITE_ERROR == error)
        {
            /* We can't access the table. Delete it and create new */
            error = sqlite3_exec(dbp->db, "DROP TABLE files;", NULL, NULL, NULL);
            sprintf(sql_command, "CREATE TABLE files ("
                    "md5 VARCHAR(32),"
                    "filetype INTEGER,"
                    "mtime INTEGER,"
                    "age INTEGER,"
                    "size INTEGER,"
                    "filename VARCHAR(%d),"
                    "extra VARCHAR(%d));", PATH_MAX, PATH_MAX);
            error = sqlite3_exec(dbp->db, sql_command, NULL, NULL, &open_error);
            if (error)
            {
                fprintf(stderr, "db_open: got error %d (%s) from create\n", error, open_error);
                if (open_error)
                    sqlite3_free(open_error);
                errno = error;
                return NULL;
            }
            /* Create indices */
            sprintf(sql_command, "CREATE INDEX files_md5 ON files (md5);");
            error = sqlite3_exec(dbp->db, sql_command, NULL, NULL, &open_error);
            if (error)
            {
                fprintf(stderr, "db_open: got error %d (%s) from create index\n", error, open_error);
                if (open_error)
                    sqlite3_free(open_error);
                sqlite3_close(dbp->db);
                errno = error;
                return NULL;
            }                                 
            sprintf(sql_command, "CREATE INDEX files_name ON files (filename);");
            error = sqlite3_exec(dbp->db, sql_command, NULL, NULL, &open_error);
            if (error)
            {
                fprintf(stderr, "db_open: got error %d (%s) from create index\n", error, open_error);
                if (open_error)
                    sqlite3_free(open_error);
                sqlite3_close(dbp->db);
                errno = error;
                return NULL;
            }
            sprintf(sql_command, "CREATE INDEX files_age ON files (age);");
            error = sqlite3_exec(dbp->db, sql_command, NULL, NULL, &open_error);
            if (error)
            {
                fprintf(stderr, "db_open: got error %d (%s) from create index\n", error, open_error);
                if (open_error)
                    sqlite3_free(open_error);
                sqlite3_close(dbp->db);
                errno = error;
                return NULL;
            }
        }
    }
    
    return dbp;
}

int db_close(JIGDB *dbp)
{
    db_state_t *state = dbp;
    /* When we're done with the database, close it. */
    if (state->db)
        sqlite3_close(state->db);
    free(state);
    return 0;
}

int db_store_file(JIGDB *dbp, db_file_entry_t *entry)
{
    int error = 0;
    db_state_t *state = dbp;
    char *open_error;
    
    sprintf(sql_command, "INSERT INTO files VALUES('%s', %d , %ld , %ld , %lld , '%s', '%s');",
            entry->md5, entry->type, entry->mtime, entry->age,
            entry->file_size, entry->filename, entry->extra);
    error = sqlite3_exec(state->db, sql_command, NULL, NULL, &open_error);
    if (error)
    {
        fprintf(stderr, "db_store_file: Failed to write entry, error %d (%s)\n", error, open_error);
        if (open_error)
            sqlite3_free(open_error);
        return error;
    }
    return error;
}

void free_results(void)
{
    struct results *entry = res_head;
    struct results *current = res_head;
    
    while(entry)
    {
        entry = entry->next;
        free(current);
        current = entry;
    }
    res_head = NULL;
    res_current = NULL;
    res_tail = NULL;
}

static int results_callback(void *pArg, int argc, char **argv, char **columnNames)
{
    struct results *entry = calloc(1, sizeof (*entry));
    
    if (res_tail)
        res_tail->next = entry;
    if (!res_head)
        res_head = entry;

    entry->prev = res_tail;
    res_tail = entry;
    if (argv[0])
        strncpy(entry->entry.md5, argv[0], sizeof(entry->entry.md5));
    if (argv[1])
        entry->entry.type = strtol(argv[1], NULL, 10);
    if (argv[2])
        entry->entry.mtime = strtoul(argv[2], NULL, 10);
    if (argv[3])
        entry->entry.age = strtoul(argv[3], NULL, 10);
    if (argv[4])
        entry->entry.file_size = strtoull(argv[4], NULL, 10);
    if (argv[5])
        strncpy(entry->entry.filename, argv[5], sizeof(entry->entry.filename));
    if (argv[6])
        strncpy(entry->entry.extra, argv[6], sizeof(entry->entry.extra));

    return 0;
}

/* Look up the most recent record that is older than the specified
 * age */
int db_lookup_file_by_age(JIGDB *dbp, time_t age, db_file_entry_t **out)
{
    int error = 0;
    db_state_t *state = dbp;
    char *open_error;

    free_results();

    sprintf(sql_command, "SELECT * FROM files WHERE age > %ld", age);
    error = sqlite3_exec(state->db, sql_command, results_callback, NULL, &open_error);
    if (error)
    {
        fprintf(stderr, "db_lookup_file_by_age: Failed to lookup, error %d (%s)\n", error, open_error);
        return error;
    }

    res_current = res_head;
    if (res_current)
    {
        *out = &res_current->entry;
        res_current = res_current->next;
    }
    else
        error = ENOENT;

    return error;
}

/* Look up the next oldest record */
int db_lookup_file_older(JIGDB *dbp, db_file_entry_t **out)
{
    int error = 0;

    if (!res_head)
        return EINVAL;
    
    if (res_current)
    {
        *out = &res_current->entry;
        res_current = res_current->next;
    }
    else
        error = ENOENT;

    return error;
}

int db_lookup_file_by_md5(JIGDB *dbp, char *md5, db_file_entry_t **out)
{
    int error = 0;
    db_state_t *state = dbp;
    char *open_error;

    free_results();

    sprintf(sql_command, "SELECT * FROM files WHERE md5 == '%s' ORDER BY filetype ASC;", md5);
    error = sqlite3_exec(state->db, sql_command, results_callback, NULL, &open_error);
    if (error)
    {
        fprintf(stderr, "db_lookup_file_by_md5: Failed to lookup, error %d (%s)\n", error, open_error);
        return error;
    }

    res_current = res_head;
    if (res_current)
    {
        *out = &res_current->entry;
        res_current = res_current->next;
    }
    else
        error = ENOENT;

    return error;
}

int db_lookup_file_by_name(JIGDB *dbp, char *filename, db_file_entry_t **out)
{
    int error = 0;
    db_state_t *state = dbp;
    char *open_error;

    free_results();

    sprintf(sql_command, "SELECT * FROM files WHERE filename == '%s';", filename);
    error = sqlite3_exec(state->db, sql_command, results_callback, NULL, &open_error);
    if (error)
    {
        fprintf(stderr, "db_lookup_file_by_name: Failed to lookup, error %d (%s)\n", error, open_error);
        return error;
    }

    res_current = res_head;
    if (res_current)
    {
        *out = &res_current->entry;
        res_current = res_current->next;
    }
    else
        error = ENOENT;

    return error;
}

int db_delete_file(JIGDB *dbp, char *md5, enum filetype type, char *filename)
{
    int error = 0;
    db_state_t *state = dbp;
    char *open_error;

    sprintf(sql_command, "DELETE FROM files WHERE md5 == '%s' AND type == '%d' AND filename == '%s';", md5, type, filename);
    error = sqlite3_exec(state->db, sql_command, NULL, NULL, &open_error);
    if (error)
        fprintf(stderr, "db_delete_file: Failed to delete, error %d (%s)\n", error, open_error);

    return error;
}

int db_dump(JIGDB *dbp)
{
    int error = 0;
/*    int num_records = 0;
    db_file_entry_t *entry = NULL;
    db_state_t *state = dbp; */

    return error;
}
