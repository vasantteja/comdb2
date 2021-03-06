/*
   Copyright 2015 Bloomberg Finance L.P.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#include <strings.h>
#include <assert.h>
#include <limits.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <inttypes.h>
#include <pthread.h>
#include <epochlib.h>
#include <poll.h>

#include "comdb2.h"
#include "sql.h"
#include <sqliteInt.h>
#include <vdbeInt.h>
#include <thread_malloc.h>
#include <flibc.h>

#include "sqloffload.h"
#include "fdb_comm.h"
#include "osqlcheckboard.h"
#include "osqlshadtbl.h"
#include "fdb_bend.h"
#include "fdb_bend_sql.h"
#include "osqlsession.h"
#include "logmsg.h"

extern int gbl_fdb_track;
extern int blockproc2sql_error(int rc, const char *func, int line);

static void init_sqlclntstate(struct sqlclntstate *clnt, char *cid, int isuuid);

int fdb_appsock_work(const char *cid, struct sqlclntstate *clnt, int version,
                     enum run_sql_flags flags, char *sql, int sqllen,
                     char *trim_key, int trim_keylen, SBUF2 *sb)
{
    int rc = 0;
    int node = -1;    /* TODO: add source node */
    int queryid = -1; /* TODO */
    char *tzname = NULL;

    clnt->sql = sql;
    clnt->fdb_state.remote_sql_sb = sb;
    clnt->fdb_state.version = version;
    clnt->fdb_state.flags = flags;
    clnt->osql.timings.query_received = osql_log_time();

#if 0
   if(osql_register_sqlthr(&clnt, OSQL_BLOCK_REQ))
   {
      fprintf(stderr, "%s: unable to register blocksql thread %llx\n", __func__, clnt.osql.rqid);
   }
#endif

    /*
       dispatch the sql
       NOTE: this waits for statement termination
    */
    rc = dispatch_sql_query(clnt);

#if 0
   if(osql_unregister_sqlthr(&clnt))
   {
      fprintf(stderr, "%s: unable to unregister blocksql thread %llx\n", 
            __func__, clnt.osql.rqid);
   }
#endif

    return rc;
}

/**
  * Open a cursor, join the transaction tid
  *
  */
int fdb_svc_cursor_open_sql(char *tid, char *cid, int code_release, int version,
                            int flags, int isuuid, struct sqlclntstate **pclnt)
{
    struct sqlclntstate *clnt = NULL;
    int rc;

    /* we need to create a private clnt state */
    clnt = (struct sqlclntstate *)calloc(1, sizeof(struct sqlclntstate));
    if (!clnt) {
        return -1;
    }

    init_sqlclntstate(clnt, cid, isuuid);
    clnt->fdb_state.code_release = fdb_ver_decoded(code_release);

    *pclnt = clnt;

    return 0;
}

/**
 * Send back a streamed row with return code (marks also eos)
 *
 */
int fdb_svc_sql_row(SBUF2 *sb, char *cid, char *row, int rowlen, int ret,
                    int isuuid)
{
    /* NOTE: we assume everything required is embedded in the sqlite row
       including genid and datacopy fields - as generated by select
       use datarow just as support */
    int rc;
    unsigned long long genid = 0;

    /* we know that genid is the last column ! */
    if (ret == IX_FND || ret == IX_FNDMORE) {
        genid = *(unsigned long long *)(row + rowlen - sizeof(genid));
        genid = flibc_htonll(genid);
    }

    rc = fdb_remcur_send_row(sb, NULL, cid, genid, row, rowlen, NULL, 0, ret,
                             isuuid);

    return rc;
}

/**
 * For requests where we want to avoid a dedicated genid lookup socket, this
 * masks every index as covered index
 *
 */
int fdb_svc_alter_schema(struct sqlclntstate *clnt, sqlite3_stmt *stmt,
                         UnpackedRecord *upr)
{
    int rootpage;
    int ixnum;
    Mem *pMem;
    struct dbtable *db;
    char *sql;
    strbuf *new_sql;
    char *bracket;
    char *where;
    struct schema *ixschema;
    struct schema *tblschema;
    int i, j;
    int first = 1;
    int len;

    if (unlikely(upr->nField != 6 && upr->nField != 7)) {
        logmsg(LOGMSG_ERROR, "%s: bug! nField=%d\n", __func__, upr->nField);
        return -1;
    }

    /* get rootpage */
    pMem = &upr->aMem[3];
    if (unlikely(pMem->flags != MEM_Int)) {
        logmsg(LOGMSG_ERROR, "%s: wrong type flags=%x\n", __func__, pMem->flags);
        return -1;
    }
    rootpage = pMem->u.i;

    struct sql_thread *thd = pthread_getspecific(query_info_key);
    db = get_sqlite_db(thd, rootpage, &ixnum);

    if (unlikely(!db)) {
        logmsg(LOGMSG_ERROR, "%s: wrong rootpage %d\n", __func__, rootpage);
        return -1;
    }

    /* check if this is an index */
    if (ixnum == -1) {
        return 0;
    }

    /* if this is a datacopy index, do not do anything */
    if (db->ix_datacopy[ixnum]) {
        return 0;
    }

    /* retrieve schemas for table and index */
    tblschema = db->schema;
    ixschema = db->ixschema[ixnum];

    /* already datacopy indexes are ok */
    if (ixschema->flags & SCHEMA_DATACOPY) {
        return 0;
    }

    /* we got a non datacopy index, export it as covered index */

    /* get the sql create */
    pMem = &upr->aMem[4];
    if (unlikely((pMem->flags & MEM_Str) == 0)) {
        logmsg(LOGMSG_ERROR, "%s: wrong type sql string %x\n", __func__,
                pMem->flags);
        return -1;
    }
    sql = pMem->z;

    /*
    fprintf(stderr, "ROOTPAGE=%d GOING FROM \"%s\"\n", rootpage, sql);
    */

    /* generate new string */
    new_sql = strbuf_new();

    if ((where = strstr(sql, ") where (")) == NULL) {
        bracket = strstr(sql, ");");
        if (unlikely(bracket == NULL)) {
            abort();
        }

        /* remove bracket */
        bracket[0] = '\0';

        strbuf_append(new_sql, sql);
    } else {
        /* move the where clause to the end */
        where[0] = '\0';

        strbuf_append(new_sql, sql);

        where[0] = ')';
    }

    /* Add all fields from ONDISK to index */
    for (int i = 0; i < tblschema->nmembers; ++i) {
        int skip = 0;
        struct field *ondisk_field = &tblschema->member[i];

        /* skip fields already in index */
        for (j = 0; j < ixschema->nmembers; ++j) {
            if (strcasecmp(ondisk_field->name, ixschema->member[j].name) == 0) {
                skip = 1;
                break;
            }
        }
        if (skip)
            continue;

        strbuf_append(new_sql, ", \"");
        strbuf_append(new_sql, ondisk_field->name);
        strbuf_append(new_sql, "\"");
        /* stop optimizer by adding dummy collation */
        if (first == 1) {
            strbuf_append(new_sql, " collate DATACOPY");
            first = 0;
        }
    }

    if (where) {
        strbuf_append(new_sql, ") ");
        where = strstr(sql, "where (");
        if (unlikely(where == NULL)) {
            abort();
        }
        strbuf_append(new_sql, where);
    } else
        strbuf_append(new_sql, ");");

    len = strlen(strbuf_buf(new_sql));
    if (pMem->zMalloc == pMem->z) {
        pMem->zMalloc = pMem->z =
            sqlite3DbRealloc(((Vdbe *)stmt)->db, pMem->z, len);
        pMem->szMalloc = pMem->n = len;
    } else {
        pMem->z = sqlite3DbRealloc(((Vdbe *)stmt)->db, pMem->z, len);
        pMem->n = len;
    }
    if (!pMem->z) {
        logmsg(LOGMSG_ERROR, "%s: failed to malloc\n", __func__);
    } else {
        memcpy(pMem->z, strbuf_buf(new_sql), len);
        /*
        pMem->z[pMem->n-2] ='\0';
        pMem->z[pMem->n-1] ='\0';
        */
    }

    /*
    fprintf(stderr, "ROOTPAGE=%d TO \"%.*s\"\n", rootpage, pMem->n, pMem->z);
    */

    strbuf_free(new_sql);

    return 0;
}

static void init_sqlclntstate(struct sqlclntstate *clnt, char *tid, int isuuid)
{
    reset_clnt(clnt, NULL, 1);

    pthread_mutex_init(&clnt->wait_mutex, NULL);
    pthread_cond_init(&clnt->wait_cond, NULL);
    pthread_mutex_init(&clnt->write_lock, NULL);
    pthread_mutex_init(&clnt->dtran_mtx, NULL);

    clnt->dbtran.mode = TRANLEVEL_SOSQL;
    strcpy(clnt->tzname, "America/New_York");
    clnt->osql.host = NULL;

    if (isuuid) {
        clnt->osql.rqid = OSQL_RQID_USE_UUID;
        comdb2uuidcpy(clnt->osql.uuid, tid);
    } else {
        clnt->osql.rqid = *(unsigned long long *)tid;
    }

    clnt->osql.timings.query_received = osql_log_time();
}

/**
 * Start a transaction
 *
 */
int fdb_svc_trans_begin(char *tid, enum transaction_level lvl, int flags,
                        int seq, struct sql_thread *thd, int isuuid,
                        struct sqlclntstate **pclnt)
{
    struct sqlclntstate *clnt = NULL;
    int rc = 0;

    assert(seq == 0);

    *pclnt = clnt =
        (struct sqlclntstate *)calloc(1, sizeof(struct sqlclntstate));
    if (!clnt) {
        return -1;
    }

    init_sqlclntstate(clnt, tid, isuuid);

    clnt->sql = "begin";

    rc = fdb_svc_trans_init(clnt, tid, lvl, seq, isuuid);
    if (rc) {
        free(clnt);
        *pclnt = NULL;
        return rc;
    }

    /* we keep the rqid in osql_sock_start */
    /* register transaction */
    if (isuuid) {
        clnt->osql.rqid = OSQL_RQID_USE_UUID;
        comdb2uuidcpy(clnt->osql.uuid, tid);
    } else
        memcpy(&clnt->osql.rqid, tid, sizeof(clnt->osql.rqid));
    if (osql_register_sqlthr(clnt, OSQL_SOCK_REQ /* not needed actually*/)) {
        logmsg(LOGMSG_ERROR, "%s: unable to register blocksql thread %llx\n",
                __func__, clnt->osql.rqid);
    }

    rc = initialize_shadow_trans(clnt, thd);

    return rc;
}

/**
 * Commit a transaction
 *
 */
int fdb_svc_trans_commit(char *tid, enum transaction_level lvl,
                         struct sqlclntstate *clnt, int seq)
{
    int rc = 0, irc = 0;
    int bdberr = 0;

    /* we have to wait for any potential cursor to go away */
    pthread_mutex_lock(&clnt->dtran_mtx);

    /* we need to wait for not yet arrived cursors, before we wait for them
       to finish!!!
     */
    fdb_sequence_request(clnt, clnt->dbtran.dtran->fdb_trans.top, seq);

    while (clnt->dbtran.dtran->fdb_trans.top->cursors.top != NULL) {
        pthread_mutex_unlock(&clnt->dtran_mtx);
        poll(NULL, 0, 10);
    }
    pthread_mutex_unlock(&clnt->dtran_mtx);

    if (clnt->dbtran.mode == TRANLEVEL_RECOM ||
        clnt->dbtran.mode == TRANLEVEL_SNAPISOL ||
        clnt->dbtran.mode == TRANLEVEL_SERIAL ||
        clnt->dbtran.mode == TRANLEVEL_SOSQL) {
        osql_shadtbl_begin_query(thedb->bdb_env, clnt);
    }

    switch (clnt->dbtran.mode) {
    case TRANLEVEL_RECOM: {
        /* here we handle the communication with bp */
        rc = recom_commit(clnt, NULL, clnt->tzname, 1);
        /* if a transaction exists
           (it doesn't for empty begin/commit */
        if (clnt->dbtran.shadow_tran) {
            if (rc == SQLITE_OK) {
                irc = trans_commit_shadow(clnt->dbtran.shadow_tran, &bdberr);
            } else {
                if (rc == SQLITE_ABORT) {
                    /* convert this to user code */
                    rc = blockproc2sql_error(clnt->osql.xerr.errval, __func__, __LINE__);
                }
                irc = trans_abort_shadow((void **)&clnt->dbtran.shadow_tran,
                                         &bdberr);
            }
            if (irc) {
                logmsg(LOGMSG_ERROR, "%s: failed %s rc=%d bdberr=%d\n", __func__,
                        (rc == SQLITE_OK) ? "commit" : "abort", irc, bdberr);
            }
        }
    }

    break;

    case TRANLEVEL_SOSQL:

        rc = osql_sock_commit(clnt, tran2req(clnt->dbtran.mode));
        /* convert this to user code */
        if (rc == SQLITE_ABORT) {
            rc = blockproc2sql_error(clnt->osql.xerr.errval, __func__, __LINE__);
        }

        break;

    default:
        abort();
    }

    if (gbl_fdb_track) {
        if (clnt->osql.rqid == OSQL_RQID_USE_UUID) {
            uuidstr_t us;
            logmsg(LOGMSG_USER, "%lu commiting tid=%s\n", pthread_self(),
                   comdb2uuidstr(clnt->osql.uuid, us));
        } else
            logmsg(LOGMSG_USER, "%lu commiting tid=%llx\n", pthread_self(),
                   clnt->osql.rqid);
    }

    if (clnt->dbtran.mode == TRANLEVEL_RECOM ||
        clnt->dbtran.mode == TRANLEVEL_SNAPISOL ||
        clnt->dbtran.mode == TRANLEVEL_SERIAL ||
        clnt->dbtran.mode == TRANLEVEL_SOSQL) {
        osql_shadtbl_done_query(thedb->bdb_env, clnt);
    }

    //#if 0
    if (osql_unregister_sqlthr(clnt)) {
        logmsg(LOGMSG_ERROR, "%s: unable to unregister blocksql thread %llx\n",
                __func__, clnt->osql.rqid);
    }
    //#endif

    clnt->writeTransaction = 0;
    clnt->dbtran.shadow_tran = NULL;

    return rc;
}

/**
 * Rollback a transaction
 *
 */
int fdb_svc_trans_rollback(char *tid, enum transaction_level lvl,
                           struct sqlclntstate *clnt, int seq)
{
    int rc;
    int bdberr = 0;

    /* we have to wait for any potential cursor to go away */
    pthread_mutex_lock(&clnt->dtran_mtx);

    /* we need to wait for not yet arrived cursors, before we wait for them
       to finish!!!
     */
    fdb_sequence_request(clnt, clnt->dbtran.dtran->fdb_trans.top, seq);

    while (clnt->dbtran.dtran->fdb_trans.top->cursors.top != NULL) {
        pthread_mutex_unlock(&clnt->dtran_mtx);
        poll(NULL, 0, 10);
    }
    pthread_mutex_unlock(&clnt->dtran_mtx);

    switch (clnt->dbtran.mode) {
    case TRANLEVEL_RECOM: {
        rc = recom_abort(clnt);
        if (rc)
            logmsg(LOGMSG_ERROR, "%s: recom abort failed %d??\n", __func__, rc);
    } break;

    case TRANLEVEL_SOSQL: {

        rc = osql_sock_abort(clnt, OSQL_SOCK_REQ);

    } break;

    default:
        abort();
    }

    if (gbl_fdb_track) {
        if (clnt->osql.rqid == OSQL_RQID_USE_UUID) {
            uuidstr_t us;
            logmsg(LOGMSG_USER, "%lu commiting tid=%s\n", pthread_self(),
                   comdb2uuidstr(clnt->osql.uuid, us));
        } else
            logmsg(LOGMSG_USER, "%lu commiting tid=%llx\n", pthread_self(),
                   clnt->osql.rqid);
    }
    /* destroying curstran */
    if (clnt->dbtran.cursor_tran) {
        rc = bdb_put_cursortran(thedb->bdb_env, clnt->dbtran.cursor_tran,
                                &bdberr);
        if (rc || bdberr) {
            logmsg(LOGMSG_ERROR, 
                    "%s: failed releasing the curstran rc=%d bdberr=%d\n",
                    __func__, rc, bdberr);
        }
        clnt->dbtran.cursor_tran = NULL;
    } else {
        logmsg(LOGMSG_ERROR, "%s: missing trans %llx\n", __func__, clnt->osql.rqid);
    }

    if (osql_unregister_sqlthr(clnt)) {
        logmsg(LOGMSG_ERROR, "%s: unable to unregister blocksql thread %llx\n",
                __func__, clnt->osql.rqid);
    }

    return rc;
}

/**
 * Join a transaction, if any
 *
 */
int fdb_svc_trans_join(char *tid, struct sqlclntstate **clnt)
{
    int rc = 0;

    rc = osql_chkboard_get_clnt(*(unsigned long long *)tid, clnt);

    if (rc)
        return rc;

    return 0;
}

int fdb_svc_trans_join_uuid(char *tid, struct sqlclntstate **clnt)
{
    int rc = 0;

    rc = osql_chkboard_get_clnt_uuid(tid, clnt);

    if (rc)
        return rc;

    return 0;
}

static struct sql_thread *
_fdb_svc_cursor_start(BtCursor *pCur, struct sqlclntstate *clnt, char *tblname,
                      int rootpage, unsigned long long genid,
                      int need_bdbcursor, int *standalone)
{
    struct sql_thread *thd;
    int rc = 0;
    int bdberr = 0;

    thd = pthread_getspecific(query_info_key);
    if (!thd) {
        logmsg(LOGMSG_ERROR, "%s: bug alert, not sql thread!\n", __func__);
        return NULL;
    }

    if (gbl_fdb_track)
        logmsg(LOGMSG_ERROR, "XYXYXY: thread %lu getting a curtran\n",
               pthread_self());

    /* we need a curtran for this one */
    if (!clnt->dbtran.cursor_tran) {
        if (gbl_fdb_track)
            logmsg(LOGMSG_ERROR, "XYXYXY: thread %lu getting a curtran\n",
                   pthread_self());

        clnt->dbtran.cursor_tran =
            bdb_get_cursortran(thedb->bdb_env, 0, &bdberr);
        if (!clnt->dbtran.cursor_tran) {
            logmsg(LOGMSG_ERROR, "%s: failed to open a curtran bdberr=%d\n",
                    __func__, bdberr);
            return NULL;
        }
        *standalone = 1;
    } else {
        if (gbl_fdb_track)
            logmsg(LOGMSG_ERROR,
                   "XYXYXY: thread %lu part of transaction %llu\n",
                   pthread_self(), clnt->osql.rqid);

        *standalone = 0;
    }

    get_copy_rootpages(thd);

    /* close any shadow cursors */
    if (clnt->dbtran.mode == TRANLEVEL_RECOM ||
        clnt->dbtran.mode == TRANLEVEL_SNAPISOL ||
        clnt->dbtran.mode == TRANLEVEL_SERIAL ||
        clnt->dbtran.mode == TRANLEVEL_SOSQL) {
        osql_shadtbl_begin_query(thedb->bdb_env, clnt);
    }

    thd->sqlclntstate = clnt;
    bzero(pCur, sizeof(*pCur));
    pCur->genid = genid;

    /* retrieve the table involved */
    if (tblname) {
        pCur->db = get_dbtable_by_name(tblname);
        pCur->ixnum = -1;
    } else {
        pCur->db = get_sqlite_db(thd, rootpage, &pCur->ixnum);
    }
    pCur->numblobs = get_schema_blob_count(pCur->db->tablename, ".ONDISK");

    if (need_bdbcursor) {
        pCur->bdbcur = bdb_cursor_open(
            pCur->db->handle, clnt->dbtran.cursor_tran,
            clnt->dbtran.shadow_tran, pCur->ixnum,
            (clnt->dbtran.shadow_tran && clnt->dbtran.mode != TRANLEVEL_SOSQL)
                ? BDB_OPEN_BOTH
                : BDB_OPEN_REAL,
            NULL /* TODO: I don't think I need this here, please double check */,
            clnt->pageordertablescan, 0, NULL, NULL, NULL, NULL, NULL,
            clnt->bdb_osql_trak, &bdberr);
        if (pCur->bdbcur == NULL) {
            logmsg(LOGMSG_ERROR, "%s: bdb_cursor_open rc %d\n", __func__, bdberr);

            rc = bdb_put_cursortran(thedb->bdb_env, clnt->dbtran.cursor_tran,
                                    &bdberr);
            if (rc || bdberr) {
                logmsg(LOGMSG_ERROR, 
                        "%s: failed releasing the curstran rc=%d bdberr=%d\n",
                        __func__, rc, bdberr);
            }
            clnt->dbtran.cursor_tran = NULL;

            return NULL;
        }
    }

    return thd;
}

static int _fdb_svc_cursor_end(BtCursor *pCur, struct sqlclntstate *clnt,
                               int standalone)
{
    int bdberr = 0;
    int rc = 0;

    if (pCur->bdbcur) {
        rc = pCur->bdbcur->close(pCur->bdbcur, &bdberr);
        if (rc || bdberr) {
            logmsg(LOGMSG_ERROR, "%s: cursor close fail rc=%d bdberr=%d\n", __func__,
                    rc, bdberr);
        }
        pCur->bdbcur = NULL;
    }

    /* close any shadow cursors */
    if (clnt->dbtran.mode == TRANLEVEL_RECOM ||
        clnt->dbtran.mode == TRANLEVEL_SNAPISOL ||
        clnt->dbtran.mode == TRANLEVEL_SERIAL ||
        clnt->dbtran.mode == TRANLEVEL_SOSQL) {
        osql_shadtbl_done_query(thedb->bdb_env, clnt);
    }

    /* destroying curstran */
    if (standalone) {
        if (clnt->dbtran.cursor_tran) {
            if (gbl_fdb_track)
                logmsg(LOGMSG_ERROR, "XYXYXY: thread %lu releasing curtran\n",
                       pthread_self());

            rc = bdb_put_cursortran(thedb->bdb_env, clnt->dbtran.cursor_tran,
                                    &bdberr);
            if (rc || bdberr) {
                logmsg(LOGMSG_ERROR, 
                        "%s: failed releasing the curstran rc=%d bdberr=%d\n",
                        __func__, rc, bdberr);
            }
            clnt->dbtran.cursor_tran = NULL;
        } else {
            uuidstr_t us;
            comdb2uuidstr(clnt->osql.uuid, us);
            logmsg(LOGMSG_ERROR, "%s: missing trans %llx %s\n", __func__,
                    clnt->osql.rqid, us);
        }
    } else {
        if (gbl_fdb_track)
            logmsg(LOGMSG_USER,
                   "XYXYXY: thread %lu in transaction, keeping curtran\n",
                   pthread_self());
    }

    if (pCur->ondisk_buf) {
        free(pCur->ondisk_buf);
    }

    pCur->ondisk_buf = NULL;

    return rc;
}

static int _fdb_svc_indexes_to_ondisk(unsigned char **pIndexes, struct dbtable *db,
                                      struct convert_failure *fail_reason,
                                      BtCursor *pCur)
{
    int i = 0;
    int rc = 0;
    unsigned char *ix = NULL;

    extern int gbl_expressions_indexes;
    if (!gbl_expressions_indexes || !db->ix_expr)
        return 0;

    for (i = 0; i < db->nix; i++) {
        if (!pIndexes[i])
            continue;
        ix = malloc(getkeysize(db, i));
        if (ix == NULL) {
            logmsg(LOGMSG_ERROR, "%s:%d failed to malloc %d\n", __func__, __LINE__,
                    getkeysize(db, i));
            return -1;
        }
        rc = sqlite_to_ondisk(db->ixschema[i],
                              (unsigned char *)pIndexes[i] + sizeof(int),
                              *((int *)pIndexes[i]), ix, "America/New_York",
                              NULL, 0, fail_reason, pCur);
        if (rc != getkeysize(db, i)) {
            char errs[128];
            convert_failure_reason_str(fail_reason, db->tablename,
                                       "SQLite format", ".ONDISK_ix", errs,
                                       sizeof(errs));
            return -1;
        }
        free(pIndexes[i]);
        pIndexes[i] = ix;
    }
    return 0;
}

/**
 * Insert a sqlite row in the local transaction
 *
 */
int fdb_svc_cursor_insert(struct sqlclntstate *clnt, char *tblname,
                          int rootpage, int version, unsigned long long genid,
                          char *data, int datalen, int seq)
{
    BtCursor bCur;
    struct sql_thread *thd;
    struct dbtable *db;
    char *row;
    int rowlen;
    blob_buffer_t rowblobs[MAXBLOBS];
    int rc = 0;
    int rc2 = 0;
    int standalone = 0;

    thd = _fdb_svc_cursor_start(&bCur, clnt, tblname, rootpage, genid, 0,
                                &standalone);
    if (!thd) {
        return -1;
    }

    db = bCur.db;
    rowlen = getdatsize(db);      /* allocate the buffer */
    row = (char *)malloc(rowlen); /* cleaned at the end of __func__ */
    bzero(&rowblobs, sizeof(rowblobs));

    if (!row) {
        logmsg(LOGMSG_ERROR, "%s malloc %d\n", __func__, rowlen);
        return -1;
    }

    rc = _fdb_svc_indexes_to_ondisk(clnt->idxInsert, db, &clnt->fail_reason,
                                    &bCur);
    if (rc) {
        logmsg(LOGMSG_ERROR, "%s:%d failed to convert sqlite indexes\n", __func__,
                __LINE__);
        free(row);
        return -1;
    }

    /* convert sqlite row to comdb2 row for bplog */
    rc = sqlite_to_ondisk(db->schema, data, datalen, row, "America/New_York",
                          rowblobs, MAXBLOBS, &clnt->fail_reason, &bCur);
    if (rc < 0) {
        char errs[128];
        convert_failure_reason_str(&clnt->fail_reason, db->tablename,
                                   "SQLite format", ".ONDISK", errs,
                                   sizeof(errs));

        rc = -1;
        free(row);
        goto done;
    }

    /* osql_insrec might need to generate indexes for readcommitted and
       it will retrieve the disk row from ondisk_buf! */
    bCur.ondisk_buf = row;

    pthread_mutex_lock(&clnt->dtran_mtx);

    fdb_sequence_request(clnt, clnt->dbtran.dtran->fdb_trans.top, seq);

    rc = osql_insrec(&bCur, thd, row, rowlen, rowblobs, MAXBLOBS);

    pthread_mutex_unlock(&clnt->dtran_mtx);

    clnt->effects.num_inserted++;

done:

    rc2 = _fdb_svc_cursor_end(&bCur, clnt, standalone);
    if (!rc) {
        rc = rc2;
    }

    return rc;
}

/**
 * Delete a sqlite row in the local transaction
 *
 */
int fdb_svc_cursor_delete(struct sqlclntstate *clnt, char *tblname,
                          int rootpage, int version, unsigned long long genid,
                          int seq)
{
    BtCursor bCur;
    struct sql_thread *thd;
    int rc = 0;
    int rc2 = 0;
    int standalone = 0;

    thd = _fdb_svc_cursor_start(&bCur, clnt, tblname, rootpage, genid, 1,
                                &standalone);
    if (!thd) {
        return -1;
    }

    rc = _fdb_svc_indexes_to_ondisk(clnt->idxDelete, bCur.db,
                                    &clnt->fail_reason, &bCur);
    if (rc) {
        logmsg(LOGMSG_ERROR, "%s:%d failed to convert sqlite indexes\n", __func__,
                __LINE__);
        return -1;
    }

    pthread_mutex_lock(&clnt->dtran_mtx);

    fdb_sequence_request(clnt, clnt->dbtran.dtran->fdb_trans.top, seq);

    rc = osql_delrec(&bCur, thd);

    pthread_mutex_unlock(&clnt->dtran_mtx);

    clnt->effects.num_deleted++;

done:

    rc2 = _fdb_svc_cursor_end(&bCur, clnt, standalone);
    if (!rc) {
        rc = rc2;
    }

    return rc;
}

/**
 * Update a sqlite row in the local transaction
 *
 */
int fdb_svc_cursor_update(struct sqlclntstate *clnt, char *tblname,
                          int rootpage, int version,
                          unsigned long long oldgenid, unsigned long long genid,
                          char *data, int datalen, int seq)
{
    BtCursor bCur;
    struct sql_thread *thd;
    struct dbtable *db;
    char *row;
    int rowlen;
    blob_buffer_t rowblobs[MAXBLOBS];
    int rc = 0;
    int rc2 = 0;
    int standalone = 0;

    thd = _fdb_svc_cursor_start(&bCur, clnt, tblname, rootpage, oldgenid, 1,
                                &standalone);
    if (!thd) {
        return -1;
    }

    db = bCur.db;
    rowlen = getdatsize(db);      /* allocate the buffer */
    row = (char *)malloc(rowlen); /* cleaned at the end of __func__ */
    bzero(&rowblobs, sizeof(rowblobs));

    if (!row) {
        logmsg(LOGMSG_ERROR, "%s malloc %d\n", __func__, rowlen);
        return -1;
    }

    rc = _fdb_svc_indexes_to_ondisk(clnt->idxDelete, db, &clnt->fail_reason,
                                    &bCur);
    if (rc) {
        logmsg(LOGMSG_ERROR, "%s:%d failed to convert sqlite indexes\n", __func__,
                __LINE__);
        free(row);
        return -1;
    }
    rc = _fdb_svc_indexes_to_ondisk(clnt->idxInsert, db, &clnt->fail_reason,
                                    &bCur);
    if (rc) {
        logmsg(LOGMSG_ERROR, "%s:%d failed to convert sqlite indexes\n", __func__,
                __LINE__);
        free(row);
        return -1;
    }

    /* convert sqlite row to comdb2 row for bplog */
    rc = sqlite_to_ondisk(db->schema, data, datalen, row, "America/New_York",
                          rowblobs, MAXBLOBS, &clnt->fail_reason, &bCur);
    if (rc < 0) {
        char errs[128];
        convert_failure_reason_str(&clnt->fail_reason, db->tablename,
                                   "SQLite format", ".ONDISK", errs,
                                   sizeof(errs));

        rc = -1;
        free(row);
        goto done;
    }

    /* osql_insrec might need to generate indexes for readcommitted and
       it will retrieve the disk row from ondisk_buf! */
    bCur.ondisk_buf = row;

    pthread_mutex_lock(&clnt->dtran_mtx);

    fdb_sequence_request(clnt, clnt->dbtran.dtran->fdb_trans.top, seq);

    rc = osql_updrec(&bCur, thd, row, rowlen, NULL /*TODO : review updCols*/,
                     rowblobs, MAXBLOBS);

    pthread_mutex_unlock(&clnt->dtran_mtx);

    clnt->effects.num_updated++;

done:

    rc2 = _fdb_svc_cursor_end(&bCur, clnt, standalone);
    if (!rc) {
        rc = rc2;
    }

    return rc;
}

/**
 * Return the sqlclntstate storing the shared transaction, if any
 *
 */
struct sqlclntstate *fdb_svc_trans_get(char *tid, int isuuid)
{
    struct sqlclntstate *clnt;
    int rc = 0;

    /* this returns a dtran_mtx locked structure */
    do {
        if (isuuid)
            rc = osql_chkboard_get_clnt_uuid(tid, &clnt);
        else
            rc = osql_chkboard_get_clnt(*(unsigned long long *)tid, &clnt);
        if (rc && rc == -1) {
            /* this is a missing transaction, we need to wait for it !*/
            poll(NULL, 0, 10);
            continue;
        }
        break;
    } while (1);

    if (rc) {
        logmsg(LOGMSG_ERROR, "%s: osql_chkboard_get_clnt returned rc=%d\n", __func__,
                rc);
        return NULL;
    }

    return clnt;
}

