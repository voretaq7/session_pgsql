/* 
   +----------------------------------------------------------------------+
   | PHP Version 4														  |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2002 The PHP Group								  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 2.02 of the PHP license,	  |
   | that is bundled with this package in the file LICENSE, and is		  |
   | available at through the world-wide-web at							  |
   | http://www.php.net/license/2_02.txt.								  |
   | If you did not receive a copy of the PHP license and are unable to	  |
   | obtain it through the world-wide-web, please send a note to		  |
   | license@php.net so we can mail you a copy immediately.				  |
   +----------------------------------------------------------------------+
   | Authors: Yasuo Ohgaki <yohgaki@php.net>							  |
   +----------------------------------------------------------------------+
 */

/* $Id: session_pgsql.c,v 1.1 2002/02/17 02:35:54 yohgaki Exp $ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/time.h>
#include <unistd.h>
#include "php.h"
#include "php_ini.h"

#ifdef HAVE_SESSION_PGSQL
#include "mm.h"
#include "session_pgsql.h"

ps_module ps_mod_pgsql = {
  	PS_MOD(pgsql)
};

#define PS_PGSQL_FILE "/tmp/session_pgsql"

typedef struct {
	MM *mm;
	time_t *last_gc;
	pid_t owner;
} ps_pgsql_instance_t;

static ps_pgsql_instance_t *ps_pgsql_instance;


#ifdef ZTS
int session_pgsql_globals_id;
#else
php_session_pgsql_globals session_pgsql_globals;
#endif

#define PS_PGSQL_DATA ps_pgsql *data = PS_GET_MOD_DATA()
#define QUERY_BUF_SIZE 250

#if 0
#define ELOG( x )   php_log_err( x TSRMLS_CC)
#else
#define ELOG( x )
#endif

function_entry session_pgsql_functions[] = {
	PHP_FE(session_pgsql_status,		NULL)
	PHP_FE(session_pgsql_info,			NULL)
	PHP_FE(session_pgsql_add_error,		NULL)
	PHP_FE(session_pgsql_get_error,		NULL)
	{ NULL, NULL, NULL }
};

static PHP_INI_MH(OnUpdate_session_pgsql_db)
{
	/* FIXME: support multiple servers */
	PS_PGSQL(db) = new_value;
  	PS_PGSQL(connstr)[0] = new_value; 
  	PS_PGSQL(servers) = 1; 

	return SUCCESS;
}

static PHP_INI_MH(OnUpdate_session_pgsql_failorver)
{
	/* FIXME: support failover */
	return SUCCESS;
}

static PHP_INI_MH(OnUpdate_session_pgsql_loadbalance)
{
	/* FIXME: support loadbalance */
	return SUCCESS;
}


/* {{{ PHP_INI
 */
PHP_INI_BEGIN()
STD_PHP_INI_ENTRY("session.pgsql_db",          "host=localhost",     PHP_INI_SYSTEM, OnUpdate_session_pgsql_db, db, php_session_pgsql_globals, session_pgsql_globals)
STD_PHP_INI_ENTRY("session.pgsql_failover",    "0",    PHP_INI_SYSTEM, OnUpdateBool, failover,     php_session_pgsql_globals, session_pgsql_globals)
STD_PHP_INI_ENTRY("session.pgsql_loadbalance", "0",    PHP_INI_SYSTEM, OnUpdateBool, loadbalance,  php_session_pgsql_globals, session_pgsql_globals)
STD_PHP_INI_ENTRY("session.pgsql_serializable","0",    PHP_INI_ALL, OnUpdateBool, serializable, php_session_pgsql_globals, session_pgsql_globals)
STD_PHP_INI_ENTRY("session.pgsql_create_table","0",    PHP_INI_ALL, OnUpdateBool, create_table, php_session_pgsql_globals, session_pgsql_globals)
STD_PHP_INI_ENTRY("session.pgsql_gc_interval", "3600", PHP_INI_ALL, OnUpdateInt, gc_interval, php_session_pgsql_globals, session_pgsql_globals)
PHP_INI_END()
/* }}} */

PHP_MINIT_FUNCTION(session_pgsql);
PHP_MSHUTDOWN_FUNCTION(session_pgsql);
PHP_RSHUTDOWN_FUNCTION(session_pgsql);
PHP_MINFO_FUNCTION(session_pgsql);

/* {{{ session_pgsql_module_entry
 */
zend_module_entry session_pgsql_module_entry = {
	STANDARD_MODULE_HEADER,
	"session pgsql",
	session_pgsql_functions,
	PHP_MINIT(session_pgsql), PHP_MSHUTDOWN(session_pgsql),
	NULL, PHP_RSHUTDOWN(session_pgsql),
	PHP_MINFO(session_pgsql),
	"0.1",
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_SESSION_PGSQL
ZEND_GET_MODULE(session_pgsql)
#endif

/* {{{ php_pg_pgsql_create_table
 */
static int php_ps_pgsql_create_table(int id TSRMLS_DC) 
{
	PGresult *pg_result;
	char *query =
	"CREATE TABLE php_session ( "
	"sess_id            text, "
	"sess_name          text, "
	"sess_data          text, "
	"sess_created       integer, "
	"sess_modified      integer, "
	"sess_expire        integer, "
	"sess_addr_created  cidr, "
	"sess_addr_modified cidr, "
	"sess_error         integer, "
	"sess_warning       integer, "
	"sess_notice        integer, "
	"sess_err_message   text, "
	"sess_counter       integer, "
	"sess_reserved      integer); ";
	int ret = SUCCESS;
	int num;

	pg_result = PQexec(PS_PGSQL(pgsql_link)[id],
					   "SELECT relname FROM pg_class WHERE relname = 'php_session';");
	if (!pg_result) {
		ret = FAILURE;
	}
	else if (!(num = PQntuples(pg_result))) {
		PQclear(pg_result);
		pg_result = PQexec(PS_PGSQL(pgsql_link)[id], query);
		if (PQresultStatus(pg_result) != PGRES_COMMAND_OK) {
			ret = FAILURE;
		}
		PQclear(pg_result);
		pg_result = PQexec(PS_PGSQL(pgsql_link)[id],
						   "CREATE INDEX php_session_idx ON php_session USING BTREE (sess_id);");
		PQclear(pg_result);
	}
	else {
		PS_PGSQL(table_exist)[id] = 1;
		PQclear(pg_result);
	}
	return ret;
}
/* }}} */

/* {{{ php_pg_pgsql_get_link
 */
static PGconn *php_ps_pgsql_get_link(const char *key TSRMLS_DC) 
{
    int id;

	if (PS_PGSQL(servers) < 1) 
		id = -1;
	else if (PS_PGSQL(servers) == 1 || !PS_PGSQL(loadbalance))
		id = 0;
	else {
		div_t tmp;
		int i, sum = 0;
		for(i=0; i<32; i++)	sum += (int)key[i]; 
		if (PS_PGSQL(failover))
			tmp = div(sum, PS_PGSQL(servers) - 1);
		else
			tmp = div(sum, PS_PGSQL(servers));
		id = tmp.rem;
	}

	if (id < 0 ) {
		return NULL;
	}
    if (PS_PGSQL(pgsql_link)[id] == NULL) {
		PS_PGSQL(pgsql_link)[id] = PQconnectdb(PS_PGSQL(connstr)[id]);
	}
	if (PQstatus(PS_PGSQL(pgsql_link)[id]) == CONNECTION_BAD) {
		PQreset(PS_PGSQL(pgsql_link)[id]);
	}
	if (PS_PGSQL(pgsql_link)[id] == NULL || PQstatus(PS_PGSQL(pgsql_link)[id]) == CONNECTION_BAD) {
		if (PS_PGSQL(pgsql_link)[id])
			PQfinish(PS_PGSQL(pgsql_link)[id]);
		php_error(E_WARNING, "Session pgsql cannot connect to PostgreSQL server (%s)",
				  PS_PGSQL(connstr)[id]);
		return NULL;
	}
	if (!PS_PGSQL(table_exist[id]) && PS_PGSQL(create_table)) 
		php_ps_pgsql_create_table(id TSRMLS_CC);
	PQsetnonblocking(PS_PGSQL(pgsql_link)[id], 1);
	return PS_PGSQL(pgsql_link)[id];
}
/* }}} */


/* {{{ php_pg_pgsql_close
 */
static int php_ps_pgsql_close(TSRMLS_D) 
{
	return SUCCESS;
}
/* }}} */

/* {{{ php_pg_pgsql_gc
 */
static int php_ps_pgsql_gc(TSRMLS_D)
{
	PGresult *pg_result;
	char query[QUERY_BUF_SIZE+1];
	char *query_tmpl = "DELETE FROM php_session WHERE sess_expire < %d;";
	int id;

	ELOG("GC Called");
	sprintf(query, query_tmpl, time(NULL));
	for (id = 0; id < PS_PGSQL(servers); id++) {
		if (PS_PGSQL(pgsql_link)[id] == NULL || PQstatus(PS_PGSQL(pgsql_link)[id]) == CONNECTION_BAD) {
			php_log_err("Sessin pgsql GC failed. Bad connection." TSRMLS_CC);
		}
		else {
			pg_result = PQexec(PS_PGSQL(pgsql_link)[id], query);
#if 0
			php_error(E_NOTICE, "Session pgsql GC deleted %s session", PQcmdTuples(pg_result) TSRMLS_CC);
#endif
			PQclear(pg_result);
		}
	}
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(session_pgsql)
{
#ifdef ZTS
	php_session_pgsql_globals *session_pgsql_globals;
	ts_allocate_id(&session_pgsql_globals_id, sizeof(php_session_pgsql_globals), NULL, NULL);
	session_pgsql_globals = ts_resource(session_pgsql_globals_id);
#endif
	ELOG("MINIT Called");

	REGISTER_INI_ENTRIES();
	ps_pgsql_instance = calloc(sizeof(ps_pgsql_instance_t), 1);
	if (!ps_pgsql_instance) {
		return FAILURE;
	}
	ps_pgsql_instance->owner = getpid();
	ps_pgsql_instance->mm = mm_create(0, PS_PGSQL_FILE);
	if (!ps_pgsql_instance->mm) {
		return FAILURE;
	}
	ps_pgsql_instance->last_gc = mm_calloc(ps_pgsql_instance->mm, 1, sizeof(time_t));
	if (!ps_pgsql_instance->last_gc) {
		mm_destroy(ps_pgsql_instance->mm);
		return FAILURE;
	}
	
	php_session_register_module(&ps_mod_pgsql);
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOEN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(session_pgsql)
{
	int i;
	
	/* link is closed at shutdown */
	ELOG("MSHUTDOWN Called");
	for (i = 0; i < PS_PGSQL(servers); i++) {
		PQfinish(PS_PGSQL(pgsql_link)[i]);
	}
	if (ps_pgsql_instance->owner == getpid() && ps_pgsql_instance->mm) {
		if (ps_pgsql_instance->last_gc) {
			mm_free(ps_pgsql_instance->mm, ps_pgsql_instance->last_gc);
		}
		mm_destroy(ps_pgsql_instance->mm);
		free(ps_pgsql_instance);
	}
	
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOEN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(session_pgsql)
{
	int ret;
	
	ELOG("RSHUTDOWN Called");
	if (*(ps_pgsql_instance->last_gc) < time(NULL) - PS_PGSQL(gc_interval)) {
		*(ps_pgsql_instance->last_gc) = time(NULL);
		ret = php_ps_pgsql_gc(TSRMLS_C);
	}
	
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(session_pgsql)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "pgsql session support", "enabled");
	php_info_print_table_end();

  	DISPLAY_INI_ENTRIES(); 
}
/* }}} */

/* {{{ ps_pgsql_valid_str
 */
static int ps_pgsql_valid_str(const char *key TSRMLS_DC)
{
	size_t len;
	const char *p;
	char c;
	int ret = 1;

	for (p = key; (c = *p); p++) {
		/* valid characters are a..z,A..Z,0..9,_ */
		if (!((c >= 'a' && c <= 'z') ||
			  (c >= 'A' && c <= 'Z') ||
			  (c >= '0' && c <= '9') ||
			  (c == '_'))) {
			ret = 0;
			break;
		}
	}
	len = p - key;
	if (len != 32)
		ret = 0;
	return ret;
}
/* }}} */

/* {{{ PS_OPEN_FUNC
 */
PS_OPEN_FUNC(pgsql)
{
	ELOG("OPEN CALLED");
	*mod_data = (void *)1; /* mod_data cannot be NULL to make session save handler module work */
	return SUCCESS;
}
/* }}} */

/* {{{ PS_CLOSE_FUNC
 */
PS_CLOSE_FUNC(pgsql)
{

	ELOG("CLOSE Called");
	*mod_data = (void *)0; /* mod_data should be set to NULL to avoid additional close call */
	return SUCCESS;
}
/* }}} */

/* {{{ PS_READ_FUNC
 */
PS_READ_FUNC(pgsql)
{
	PGconn *pg_link;
	PGresult *pg_result;
	char query[QUERY_BUF_SIZE+1];
	char *query_tpl = "SELECT sess_expire, sess_counter, sess_data FROM php_session WHERE sess_id = '%s';";
	int ret = FAILURE;
	ExecStatusType status;

	ELOG("READ Called");

	pg_link = php_ps_pgsql_get_link(key TSRMLS_CC);
	if (pg_link == NULL || PQstatus(pg_link) == CONNECTION_BAD) {
		ret = FAILURE;
	}
	else {
		/* clean up & check last write */
		while ((pg_result = PQgetResult(pg_link))) {
			status = PQresultStatus(pg_result);
			switch(status) {
				case PGRES_COMMAND_OK:
				case PGRES_TUPLES_OK:
					break;
				default:
					php_log_err("Session pgsql: There is an error during last session write.\n" TSRMLS_CC);
			}
		}

		/* start reading */
		if (PS_PGSQL(serializable)) {
			pg_result = PQexec(pg_link, "BEGIN; SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;");
		}
		else {
			pg_result = PQexec(pg_link, "BEGIN;");
		}
		if (PQresultStatus(pg_result) != PGRES_COMMAND_OK) {
			php_error(E_NOTICE,"Session pgsql READ failed. %s",
					  PQresultErrorMessage(pg_result));
			ret = FAILURE;
		}
		PQclear(pg_result);

		PS_PGSQL(sess_new) = 0;
		PS_PGSQL(sess_del) = 0;
		*vallen = 0;
		if (ps_pgsql_valid_str(key TSRMLS_CC)) {
			snprintf(query, QUERY_BUF_SIZE, query_tpl, key);
			pg_result = PQexec(pg_link, query);
			status = PQresultStatus(pg_result);
			if (PQresultStatus(pg_result) == PGRES_TUPLES_OK) {
				if (PQntuples(pg_result) == 0) {
					PS_PGSQL(sess_new) = 1;
				}
				else {
					char *expire;
					size_t exp;
					
					expire = PQgetvalue(pg_result, 0, 0);
					exp = (size_t)atoi(expire);
					if ((exp < time(NULL))) {
						PS_PGSQL(sess_new) = 1;
						PS_PGSQL(sess_del) = 1;
					}
					else {
						PS_PGSQL(sess_cnt) = (int)atoi(PQgetvalue(pg_result, 0, 1));
						/* PQgetvalue reuturns "" for NULL */
						*val = PQgetvalue(pg_result, 0, 2);
						if (*val) {
							*vallen = strlen(*val);
							*val = safe_estrndup(*val, *vallen);
						}
					}
				}
				ret = SUCCESS;
			}
			else {
				php_error(E_WARNING,"Session pgsql READ failed: %s (%s)",
						  PQresultErrorMessage(pg_result), query);
				ret = FAILURE;
			}
			PQclear(pg_result);	
		}
		if (*vallen == 0) {
			*val = safe_estrndup("",0);
		}
	}
/*  	return ret; */
	return SUCCESS;
}
/* }}} */

/* {{{ PS_WRITE_FUNC
 */
PS_WRITE_FUNC(pgsql)
{
	PGconn *pg_link;
	size_t query_len;
	time_t now, exp;
	char *data;
	char *query;
	char *query_delete =
	   "DELETE FROM php_session WHERE sess_id = '%s';";
	char *query_insert =
	   "INSERT INTO php_session (sess_id, sess_created, sess_modified, sess_expire, sess_counter, sess_data) "
	   "VALUES ('%s', %d, %d, %d, 1, '%s');";
	char *query_update =
	   "UPDATE php_session SET sess_data = '%s', sess_modified = %d , sess_expire = %d "
	   "WHERE sess_id = '%s';";
	int len, ret = FAILURE;

	ELOG("WRITE Called");
	pg_link = php_ps_pgsql_get_link(key TSRMLS_CC);
	if (pg_link == NULL || PQstatus(pg_link) == CONNECTION_BAD) {
		php_error(E_NOTICE,"Session pgsql WRITE failed. Bad connection");
		return FAILURE;
	}
	if (PS_PGSQL(sess_del)) {
		query_len = strlen(query_delete)+strlen(key);
		query = emalloc(query_len+1);
		sprintf(query, query_delete, key);
		PQsendQuery(pg_link, query);
		efree(query);
	}

	now = time(NULL);
	exp = now + PS(gc_maxlifetime);
	data = (void *)php_addslashes(val, strlen(val), &len, 0 TSRMLS_CC);
	if (PS_PGSQL(sess_new)) {
		/* INSERT */
		query_len = strlen(query_insert)+strlen(data)+strlen(key)+120;
		query = emalloc(query_len+1);
		sprintf(query, query_insert, key, now, now, exp, data);
		PQsendQuery(pg_link, query);
	}
	else {
		/* UPDATE */
		query_len = strlen(query_update)+strlen(key)+strlen(data)+120;
		query = emalloc(query_len+1);
		sprintf(query, query_update, data, now, exp, key);
		PQsendQuery(pg_link, query);
	}
	efree(query);
	efree(data);
/*  	PQsendQuery(pg_link, "END;"); */
	PQexec(pg_link, "END;");

	return SUCCESS;	
}
/* }}} */

/* {{{ PS_DESTROY_FUNC
 */
PS_DESTROY_FUNC(pgsql)
{
	PGconn *pg_link;
	PGresult *pg_result;
	size_t query_len;
	char *query;
	char *query_update = "DELETE FROM php_session WHERE sess_id = '%s';";
	int ret = FAILURE;

	ELOG("DESTROY Called");
	pg_link = php_ps_pgsql_get_link(key TSRMLS_CC);
	if (pg_link == NULL || PQstatus(pg_link) == CONNECTION_BAD) {
		php_error(E_NOTICE,"Session pgsql DESTROY failed. Bad connection");
		return FAILURE;
	}

	if (ps_pgsql_valid_str(key TSRMLS_CC)) {
		query_len = strlen(query_update)+strlen(key);
		query = (char *)emalloc(query_len+1);
		snprintf(query, query_len, key);
		pg_result = PQexec(pg_link, query);
		if (PQresultStatus(pg_result) == PGRES_TUPLES_OK) {
			ret = SUCCESS;
		}
		PQclear(pg_result);
		efree(query);
	}
	
	return ret;
}
/* }}} */

/* {{{ PS_GC_FUNC
 */
PS_GC_FUNC(pgsql)
{
	/* this module does not use probablity for gc, but gc_interval */
	*nrdels = 0;
	return SUCCESS;
}
/* }}} */

/* {{{ proto int session_pgsql_status(void)
   Returns current pgsql save handler status */
PHP_FUNCTION(session_pgsql_status)
{

}
/* }}} */

/* {{{ proto int session_pgsql_status(void)
   Returns current session info */
PHP_FUNCTION(session_pgsql_info)
{

}
/* }}} */

/* {{{ proto bool session_pgsql_add_error(int error_level [[, string error_message], string session_id])
   Increments error counts and sets last error message */
PHP_FUNCTION(session_pgsql_add_error)
{

}
/* }}} */

/* {{{ proto array session_pgsql_get_error([string session_id])
   Returns number of errors and last error message */
PHP_FUNCTION(session_pgsql_get_error)
{

}
/* }}} */

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: sw=4 ts=4 fdm=marker
 * vim<600: sw=4 ts=4
 */
