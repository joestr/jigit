#include <db.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "jigdb.h"


/* Primary key is md5; secondary indices available for _all_ other
 * fields */
typedef struct
{
    DB  *md5_db;     /* md5 key, should be unique */
    DB  *age_db;     /* age key, expect DUPs */
    DBC *age_cursor; /* cursor for searching through age field */
    DB  *name_db;    /* name key, should be unique */
} db_state_t;

static int get_age(DB *sdbp,         /* secondary db handle */
                   const DBT *pkey,  /* primary db record's key */
                   const DBT *pdata, /* primary db record's data */
                   DBT *skey)        /* secondary db record's key */
{
    db_entry_t *entry = pdata->data;
    
    memset(skey, 0, sizeof(DBT));
    skey->data = &entry->age;
    skey->size = sizeof(entry->age);
    return 0;
}

static int get_name(DB *sdbp,         /* secondary db handle */
                    const DBT *pkey,  /* primary db record's key */
                    const DBT *pdata, /* primary db record's data */
                    DBT *skey)        /* secondary db record's key */
{
    db_entry_t *entry = pdata->data;
    
    memset(skey, 0, sizeof(DBT));
    skey->data = &entry->filename;
    skey->size = strlen(entry->filename) + 1;
    return 0;
}

JIGDB *db_open()
{
    db_state_t *dbp = NULL;
    u_int32_t flags;          /* database open flags */
    int ret = 0;              /* function return value */

    /* Allocate state structure */
    dbp = calloc(1, sizeof(*dbp));
    if (dbp)
    {
        /* Open and associate all the databases */
        ret = db_create(&dbp->md5_db, NULL, 0);
        if (ret)
            return NULL;
        ret = db_create(&dbp->age_db, NULL, 0);
        if (ret)
            return NULL;
        ret = dbp->age_db->set_flags(dbp->age_db, DB_DUPSORT);
        if (ret)
            return NULL;
        ret = db_create(&dbp->name_db, NULL, 0);
        if (ret)
            return NULL;

        /* Database open flags */
        flags = DB_CREATE;      /* If the database does not exist,
                                 * create it.*/

        /* open the databases */
        ret = dbp->md5_db->open(dbp->md5_db,     /* DB structure pointer */
                                NULL,            /* Transaction pointer */
                                "jigit_md5.db",  /* On-disk file that holds the database. */
                                NULL,            /* Optional logical database name */
                                DB_BTREE,        /* Database access method */
                                flags,           /* Open flags */
                                0);              /* File mode (using defaults) */
        if (ret)
            return NULL;

        ret = dbp->age_db->open(dbp->age_db,     /* DB structure pointer */
                                NULL,            /* Transaction pointer */
                                "jigit_age.db",  /* On-disk file that holds the database. */
                                NULL,            /* Optional logical database name */
                                DB_BTREE,        /* Database access method */
                                flags,           /* Open flags */
                                0);              /* File mode (using defaults) */
        if (ret)
            return NULL;

        ret = dbp->name_db->open(dbp->name_db,    /* DB structure pointer */
                                 NULL,            /* Transaction pointer */
                                 "jigit_name.db", /* On-disk file that holds the database. */
                                 NULL,            /* Optional logical database name */
                                 DB_BTREE,        /* Database access method */
                                 flags,           /* Open flags */
                                 0);              /* File mode (using defaults) */
        if (ret)
            return NULL;

        /* Now associate the secondaries to the primary */
        dbp->md5_db->associate(dbp->md5_db,    /* Primary database */
                               NULL,           /* TXN id */
                               dbp->age_db,    /* Secondary database */
                               get_age,        /* Callback used for key creation. Not
                                                * defined in this example. See the next
                                                * section. */
                               0);              /* Flags */
        
        dbp->md5_db->associate(dbp->md5_db,    /* Primary database */
                               NULL,           /* TXN id */
                               dbp->name_db,   /* Secondary database */
                               get_name,       /* Callback used for key creation. Not
                                                * defined in this example. See the next
                                                * section. */
                               0);              /* Flags */

        /* And open a cursor in the age database */
        dbp->age_db->cursor(dbp->age_db, NULL, &dbp->age_cursor, 0);
    }
    
    return dbp;
}

int db_close(JIGDB *dbp)
{
    db_state_t *state = dbp;
    /* When we're done with the database, close it. */
    if (state != NULL)
    {
        if (state->name_db)
            state->name_db->close(state->name_db, 0);
        if (state->age_cursor)
            state->age_cursor->c_close(state->age_cursor);
        if (state->age_db)
            state->age_db->close(state->age_db, 0);
        if (state->md5_db)
            state->md5_db->close(state->md5_db, 0);
    }
    free(state);
    return 0;
}

int db_store(JIGDB *dbp, db_entry_t *entry)
{
    int error = 0;
    DBT key, data;
    db_state_t *state = dbp;
    size_t keylen = strlen(entry->md5) + 1;
    
    memset(&key, 0, sizeof(DBT));
    memset(&data, 0, sizeof(DBT));

    key.data = &entry->md5[0];
    key.size = keylen;
    key.ulen = keylen;
    key.flags = DB_DBT_USERMEM;
    
    data.data = entry;
    data.size = sizeof(*entry);
    data.ulen = sizeof(*entry);
    data.flags = DB_DBT_USERMEM;
    
    error = state->md5_db->put(state->md5_db, NULL, &key, &data, 0);    
    return error;
}

/* Look up the most recent record that is older than the specified
 * age */
int db_lookup_by_age(JIGDB *dbp, time_t age, db_entry_t **out)
{
    int error = 0;
    DBT key, data;
    db_state_t *state = dbp;
    size_t keylen = sizeof(age);
    
    memset(&key, 0, sizeof(DBT));
    memset(&data, 0, sizeof(DBT));

    key.data = &age;
    key.size = keylen;
    key.ulen = keylen;
    
    error = state->age_cursor->c_get(state->age_cursor, &key, &data, DB_SET_RANGE);
    if (!error)
        *out = data.data;

    return error;
}

/* Look up the next oldest record */
int db_lookup_older(JIGDB *dbp, db_entry_t **out)
{
    int error = 0;
    DBT key, data;
    db_state_t *state = dbp;
    
    memset(&key, 0, sizeof(DBT));
    memset(&data, 0, sizeof(DBT));

    /* Look for the next record */
    error = state->age_cursor->c_get(state->age_cursor, &key, &data, DB_NEXT);
    if (error)
        return error;
    
    /* else */
    *out = data.data;
    return 0;
}

int db_lookup_by_md5(JIGDB *dbp, char *md5, db_entry_t **out)
{
    int error = 0;
    DBT key, data;
    db_state_t *state = dbp;
    size_t keylen = strlen(md5) + 1;
    
    memset(&key, 0, sizeof(DBT));
    memset(&data, 0, sizeof(DBT));

    key.data = md5;
    key.size = keylen;
    key.ulen = keylen;
    key.flags = DB_DBT_USERMEM;
    
    error = state->md5_db->get(state->md5_db, NULL, &key, &data, 0);
    if (error)
        return error;
    
    /* else */
    *out = data.data;
    return 0;
}

int db_lookup_by_name(JIGDB *dbp, char *filename, db_entry_t **out)
{
    int error = 0;
    DBT key, data;
    db_state_t *state = dbp;
    size_t keylen = strlen(filename) + 1;
    
    memset(&key, 0, sizeof(DBT));
    memset(&data, 0, sizeof(DBT));

    key.data = filename;
    key.size = keylen;
    key.ulen = keylen;
    
    error = state->name_db->get(state->name_db, NULL, &key, &data, 0);
    if (error)
        return error;
    
    /* else */
    *out = data.data;
    return 0;
}

int db_delete(JIGDB *dbp, char *md5)
{
    int error = 0;
    DBT key;
    db_state_t *state = dbp;
    size_t keylen = strlen(md5) + 1;
    
    memset(&key, 0, sizeof(DBT));

    key.data = md5;
    key.size = keylen;
    key.ulen = keylen;
    key.flags = DB_DBT_USERMEM;
    
    error = state->md5_db->del(state->md5_db, NULL, &key, 0);    
    return error;
}

int db_dump(JIGDB *dbp)
{
    int error = 0;
    DBC *cursor;
    DBT key, data;
    int num_records = 0;
    db_entry_t *entry = NULL;
    db_state_t *state = dbp;
    
    memset(&key, 0, sizeof(DBT));
    memset(&data, 0, sizeof(DBT));
    
    state->md5_db->cursor(state->md5_db, NULL, &cursor, 0);

    printf("Rec  T  Age       Mtime     Size       MD5                     Filename\n");
    while (!error)
    {
        error = cursor->c_get(cursor, &key, &data, DB_NEXT);
        switch (error)
        {
            case DB_NOTFOUND:
                break;
            case 0:
                num_records++;
                entry = (db_entry_t *)data.data;
                printf("%3.3d  %c  %8.8X  %8.8X  %9lld  %s  %s\n",
                       num_records,
                       (entry->type == FT_LOCAL ? 'L' : (entry->type == FT_ISO ? 'C' : 'R')),
                       (unsigned int)entry->age,
                       (unsigned int)entry->mtime,
                       entry->file_size,
                       entry->md5,
                       entry->filename);
                break;
            default:
                printf("Error %d from c_get!\n", error);
                break;
        }
    }

	/* Cursors must be closed */
    if (cursor != NULL)
        cursor->c_close(cursor);

    return 0;
}

