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

enum result_type
{
    RES_TEMPLATE,
    RES_FILES,
    RES_BLOCKS
};

struct results
{
    struct results *next;
    struct results *prev;
    enum result_type type;
    union
    {
        db_template_entry_t template;
        db_file_entry_t file;
        db_compressed_entry_t block;
    } data;
};

struct results *res_head = NULL;
struct results *res_current = NULL;
struct results *res_tail = NULL;

char sql_command[2 * PATH_MAX];

static int db_create_template_table(db_state_t *dbp)
{
    int error = 0;
    char *open_error;
    
    /* We can't access the table. Delete it and create new */
    error = sqlite3_exec(dbp->db, "DROP TABLE template;", NULL, NULL, NULL);
    sprintf(sql_command, "CREATE TABLE template ("
            "image_offset INTEGER,"
            "size INTEGER,"
            "uncomp_offset INTEGER,"
            "type INTEGER,"
            "templatename VARCHAR(%d),"
            "md5 VARCHAR(32));", PATH_MAX);
    error = sqlite3_exec(dbp->db, sql_command, NULL, NULL, &open_error);
    if (error)
    {
        fprintf(stderr, "db_create_template_table: got error %d (%s) from create\n", error, open_error);
        if (open_error)
            sqlite3_free(open_error);
        return error;
    }
    /* Create indices */
    sprintf(sql_command, "CREATE INDEX template_offset ON template (uncomp_offset);");
    error = sqlite3_exec(dbp->db, sql_command, NULL, NULL, &open_error);
    if (error)
    {
        fprintf(stderr, "db_create_template_table: got error %d (%s) from create index\n", error, open_error);
        if (open_error)
            sqlite3_free(open_error);
        return error;
    }                                 
    sprintf(sql_command, "CREATE INDEX template_template ON template (templatename);");
    error = sqlite3_exec(dbp->db, sql_command, NULL, NULL, &open_error);
    if (error)
    {
        fprintf(stderr, "db_create_template_table: got error %d (%s) from create index\n", error, open_error);
        if (open_error)
            sqlite3_free(open_error);
        return error;
    }

    return 0;
}

static int db_create_files_table(db_state_t *dbp)
{
    int error = 0;
    char *open_error;
    
    /* We can't access the table. Delete it and create new */
    error = sqlite3_exec(dbp->db, "DROP TABLE files;", NULL, NULL, NULL);
    sprintf(sql_command, "CREATE TABLE files ("
            "size INTEGER,"
            "mtime INTEGER,"
            "age INTEGER,"
            "filetype INTEGER,"
            "md5 VARCHAR(32),"
            "filename VARCHAR(%d),"
            "extra VARCHAR(%d));", PATH_MAX, PATH_MAX);
    error = sqlite3_exec(dbp->db, sql_command, NULL, NULL, &open_error);
    if (error)
    {
        fprintf(stderr, "db_create_files_table: got error %d (%s) from create\n", error, open_error);
        if (open_error)
            sqlite3_free(open_error);
        return error;
    }
    /* Create indices */
    sprintf(sql_command, "CREATE INDEX files_md5 ON files (md5);");
    error = sqlite3_exec(dbp->db, sql_command, NULL, NULL, &open_error);
    if (error)
    {
        fprintf(stderr, "db_create_files_table: got error %d (%s) from create index\n", error, open_error);
        if (open_error)
            sqlite3_free(open_error);
        return error;
    }                                 
    sprintf(sql_command, "CREATE INDEX files_name ON files (filename);");
    error = sqlite3_exec(dbp->db, sql_command, NULL, NULL, &open_error);
    if (error)
    {
        fprintf(stderr, "db_create_files_table: got error %d (%s) from create index\n", error, open_error);
        if (open_error)
            sqlite3_free(open_error);
        return error;
    }
    sprintf(sql_command, "CREATE INDEX files_age ON files (age);");
    error = sqlite3_exec(dbp->db, sql_command, NULL, NULL, &open_error);
    if (error)
    {
        fprintf(stderr, "db_create_files_table: got error %d (%s) from create index\n", error, open_error);
        if (open_error)
            sqlite3_free(open_error);
        return error;
    }

    return 0;
}

static int db_create_comp_blocks_table(db_state_t *dbp)
{
    int error = 0;
    char *open_error;
    
    /* We can't access the table. Delete it and create new */
    error = sqlite3_exec(dbp->db, "DROP TABLE blocks;", NULL, NULL, NULL);
    sprintf(sql_command, "CREATE TABLE blocks ("
            "comp_offset INTEGER,"
            "uncomp_offset INTEGER,"
            "uncomp_size INTEGER,"
            "templatename VARCHAR(%d));", PATH_MAX);
    error = sqlite3_exec(dbp->db, sql_command, NULL, NULL, &open_error);
    if (error)
    {
        fprintf(stderr, "db_create_comp_blocks_table: got error %d (%s) from create\n", error, open_error);
        if (open_error)
            sqlite3_free(open_error);
        return error;
    }
    /* Create indices */
    sprintf(sql_command, "CREATE INDEX blocks_offset ON blocks (uncomp_offset);");
    error = sqlite3_exec(dbp->db, sql_command, NULL, NULL, &open_error);
    if (error)
    {
        fprintf(stderr, "db_create_comp_blocks_table: got error %d (%s) from create index\n", error, open_error);
        if (open_error)
            sqlite3_free(open_error);
        return error;
    }                                 
    sprintf(sql_command, "CREATE INDEX blocks_template ON blocks (templatename);");
    error = sqlite3_exec(dbp->db, sql_command, NULL, NULL, &open_error);
    if (error)
    {
        fprintf(stderr, "db_create_comp_blocks_table: got error %d (%s) from create index\n", error, open_error);
        if (open_error)
            sqlite3_free(open_error);
        return error;
    }

    return 0;
}

JIGDB *db_open(char *db_name)
{
    db_state_t *dbp = NULL;
    int error = 0;            /* function return value */

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
            /* No table found, so create new */
            /* First, the template table */
            error = db_create_template_table(dbp);
            if (error)
            {
                sqlite3_close(dbp->db);
                errno = error;
                return NULL;
            }

            /* 2. The files table */
            error = db_create_files_table(dbp);
            if (error)
            {
                sqlite3_close(dbp->db);
                errno = error;
                return NULL;
            }

            /* 3. The compressed blocks table */
            error = db_create_comp_blocks_table(dbp);
            if (error)
            {
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

/* Delete ALL the template and compressed block entries for a
 * specified template file */
int db_delete_template_cache(JIGDB *dbp, char *template_name)
{
    int error = 0;
    db_state_t *state = dbp;
    char *open_error;

    sprintf(sql_command, "DELETE FROM template WHERE templatename == '%s';", template_name);
    error = sqlite3_exec(state->db, sql_command, NULL, NULL, &open_error);
    if (error)
        fprintf(stderr, "db_delete_template_cache: Failed to delete template entries, error %d (%s)\n", error, open_error);
    else
    {
        sprintf(sql_command, "DELETE FROM blocks WHERE templatename == '%s';", template_name);
        error = sqlite3_exec(state->db, sql_command, NULL, NULL, &open_error);
        if (error)
            fprintf(stderr, "db_delete_template_cache: Failed to delete block entries, error %d (%s)\n", error, open_error);
    }
    return error;
}

/* Does nothing at the moment... */
int db_dump(JIGDB *dbp)
{
    int error = 0;
/*    int num_records = 0;
    db_file_entry_t *entry = NULL;
    db_state_t *state = dbp; */

    return error;
}

static void free_results(void)
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
    enum result_type *type = pArg;
    
    if (res_tail)
        res_tail->next = entry;
    if (!res_head)
        res_head = entry;

    entry->prev = res_tail;
    res_tail = entry;

    switch (*type)
    {
        case RES_TEMPLATE:
            if (argv[0])
                entry->data.template.image_offset = strtoull(argv[0], NULL, 10);
            if (argv[1])
                entry->data.template.size = strtoull(argv[1], NULL, 10);
            if (argv[2])
                entry->data.template.uncomp_offset = strtoull(argv[2], NULL, 10);
            if (argv[3])
                entry->data.template.type = strtoul(argv[3], NULL, 10);
            if (argv[4])
                strncpy(entry->data.template.template_name, argv[4], sizeof(entry->data.template.template_name));
            if (argv[5])
                strncpy(entry->data.template.md5, argv[5], sizeof(entry->data.template.md5));
            break;
        case RES_FILES:
            if (argv[0])
                entry->data.file.file_size = strtoull(argv[0], NULL, 10);
            if (argv[1])
                entry->data.file.mtime = strtoul(argv[1], NULL, 10);
            if (argv[2])
                entry->data.file.age = strtoul(argv[2], NULL, 10);
            if (argv[3])
                entry->data.file.type = strtol(argv[3], NULL, 10);
            if (argv[4])
                strncpy(entry->data.file.md5, argv[4], sizeof(entry->data.file.md5));
            if (argv[5])
                strncpy(entry->data.file.filename, argv[5], sizeof(entry->data.file.filename));
            if (argv[6])
                strncpy(entry->data.file.extra, argv[6], sizeof(entry->data.file.extra));
            break;
        case RES_BLOCKS:
            if (argv[0])
                entry->data.block.comp_offset = strtoull(argv[0], NULL, 10);
            if (argv[1])
                entry->data.block.uncomp_offset = strtoull(argv[1], NULL, 10);
            if (argv[2])
                entry->data.block.uncomp_size = strtoull(argv[2], NULL, 10);
            if (argv[3])
                strncpy(entry->data.block.template_name, argv[3], sizeof(entry->data.block.template_name));            
            break;
    }
    
    return 0;
}

int db_store_template(JIGDB *dbp, db_template_entry_t *entry)
{
    int error = 0;
    db_state_t *state = dbp;
    char *open_error;

    sprintf(sql_command, "INSERT INTO template VALUES(%lld,%lld,%lld,%d,'%s','%s');",
            entry->image_offset, entry->size, entry->uncomp_offset, entry->type,
            entry->template_name, entry->md5);
    error = sqlite3_exec(state->db, sql_command, NULL, NULL, &open_error);
    if (error)
    {
        fprintf(stderr, "db_store_template: Failed to write entry, error %d (%s)\n", error, open_error);
        if (open_error)
            sqlite3_free(open_error);
        return error;
    }
    return error;
}
    
int db_lookup_template_by_offset(JIGDB *dbp, char *template_name,
                                 unsigned long long image_offset, db_template_entry_t **out)
{
    int error = 0;
    db_state_t *state = dbp;
    char *open_error;
    int result_type = RES_TEMPLATE;

    free_results();

    sprintf(sql_command,
            "SELECT * FROM template WHERE template_name == '%s' "
            "AND image_offset <= %lld "
            "AND (image_offset + size) > %lld;", template_name, image_offset, image_offset);
    error = sqlite3_exec(state->db, sql_command, results_callback, &result_type, &open_error);
    if (error)
    {
        fprintf(stderr, "db_lookup_template_by_offset: Failed to lookup, error %d (%s)\n", error, open_error);
        return error;
    }

    res_current = res_head;
    if (res_current)
    {
        *out = &res_current->data.template;
        res_current = res_current->next;
    }
    else
        error = ENOENT;

    return error;
}

int db_store_file(JIGDB *dbp, db_file_entry_t *entry)
{
    int error = 0;
    db_state_t *state = dbp;
    char *open_error;
    
    sprintf(sql_command, "INSERT INTO files VALUES(%lld,%ld,%ld,%d,'%s','%s','%s');",
            entry->file_size,
            entry->mtime,
            entry->age,
            entry->type,
            entry->md5,
            entry->filename,
            entry->extra);
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

/* Look up the most recent record that is older than the specified
 * age */
int db_lookup_file_by_age(JIGDB *dbp, time_t age, db_file_entry_t **out)
{
    int error = 0;
    db_state_t *state = dbp;
    char *open_error;
    int result_type = RES_FILES;

    free_results();

    sprintf(sql_command, "SELECT * FROM files WHERE age > %ld", age);
    error = sqlite3_exec(state->db, sql_command, results_callback, &result_type, &open_error);
    if (error)
    {
        fprintf(stderr, "db_lookup_file_by_age: Failed to lookup, error %d (%s)\n", error, open_error);
        return error;
    }

    res_current = res_head;
    if (res_current)
    {
        *out = &res_current->data.file;
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
        *out = &res_current->data.file;
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
    int result_type = RES_FILES;

    free_results();

    sprintf(sql_command, "SELECT * FROM files WHERE md5 == '%s' ORDER BY filetype ASC;", md5);
    error = sqlite3_exec(state->db, sql_command, results_callback, &result_type, &open_error);
    if (error)
    {
        fprintf(stderr, "db_lookup_file_by_md5: Failed to lookup, error %d (%s)\n", error, open_error);
        return error;
    }

    res_current = res_head;
    if (res_current)
    {
        *out = &res_current->data.file;
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
    int result_type = RES_FILES;

    free_results();

    sprintf(sql_command, "SELECT * FROM files WHERE filename == '%s';", filename);
    error = sqlite3_exec(state->db, sql_command, results_callback, &result_type, &open_error);
    if (error)
    {
        fprintf(stderr, "db_lookup_file_by_name: Failed to lookup, error %d (%s)\n", error, open_error);
        return error;
    }

    res_current = res_head;
    if (res_current)
    {
        *out = &res_current->data.file;
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

    sprintf(sql_command, "DELETE FROM files WHERE md5 == '%s' AND filetype == '%d' AND filename == '%s';", md5, type, filename);
    error = sqlite3_exec(state->db, sql_command, NULL, NULL, &open_error);
    if (error)
        fprintf(stderr, "db_delete_file: Failed to delete, error %d (%s)\n", error, open_error);

    return error;
}

int db_store_block(JIGDB *dbp, db_compressed_entry_t *entry)
{
    int error = 0;
    db_state_t *state = dbp;
    char *open_error;

    sprintf(sql_command, "INSERT INTO blocks VALUES(%ld,%ld,%ld,'%s');",
            entry->comp_offset, entry->uncomp_offset, entry->uncomp_size, entry->template_name);
    error = sqlite3_exec(state->db, sql_command, NULL, NULL, &open_error);
    if (error)
    {
        fprintf(stderr, "db_store_block: Failed to write entry, error %d (%s)\n", error, open_error);
        if (open_error)
            sqlite3_free(open_error);
        return error;
    }
    return error;
}

int db_lookup_block_by_offset(JIGDB *dbp, char *template_name,
                              unsigned long uncomp_offset, db_compressed_entry_t **out)
{
    int error = 0;
    db_state_t *state = dbp;
    char *open_error;
    int result_type = RES_BLOCKS;

    free_results();

    sprintf(sql_command,
            "SELECT * FROM blocks WHERE template_name == '%s' "
            "AND uncomp_offset <= %ld "
            "AND (uncomp_offset + size) > %ld;", template_name, uncomp_offset, uncomp_offset);
    error = sqlite3_exec(state->db, sql_command, results_callback, &result_type, &open_error);
    if (error)
    {
        fprintf(stderr, "db_lookup_template_by_offset: Failed to lookup, error %d (%s)\n", error, open_error);
        return error;
    }

    res_current = res_head;
    if (res_current)
    {
        *out = &res_current->data.block;
        res_current = res_current->next;
    }
    else
        error = ENOENT;

    return error;
}
