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

/* $Id: session_pgsql.c,v 1.3 2002/02/21 06:39:48 yohgaki Exp $ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/time.h>
#include <unistd.h>
#include <assert.h>
#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/standard/php_string.h"
#include "ext/standard/php_var.h"
#include "ext/standard/php_smart_str.h"

#ifdef HAVE_SESSION_PGSQL
#include "mm.h"
#include "session_pgsql.h"

#ifndef DEBUG
#undef NDEBUG
#endif

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

static PHP_INI_MH(OnUpdate_session_pgsql_failover)
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
STD_PHP_INI_ENTRY("session.pgsql_db",          "host=localhost",     PHP_INI_SYSTEM|PHP_INI_PERDIR, OnUpdate_session_pgsql_db, db, php_session_pgsql_globals, session_pgsql_globals)
STD_PHP_INI_ENTRY("session.pgsql_use_app_vars",    "1",    PHP_INI_SYSTEM, OnUpdateBool, use_app_vars,     php_session_pgsql_globals, session_pgsql_globals)
STD_PHP_INI_ENTRY("session.pgsql_failover",    "0",    PHP_INI_SYSTEM, OnUpdate_session_pgsql_failover, failover,     php_session_pgsql_globals, session_pgsql_globals)
STD_PHP_INI_ENTRY("session.pgsql_loadbalance", "0",    PHP_INI_SYSTEM, OnUpdate_session_pgsql_loadbalance, loadbalance,  php_session_pgsql_globals, session_pgsql_globals)
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
	"ps_id            text, "
	"ps_name          text, "
	"ps_data          text, "
	"ps_created       integer, "
	"ps_modified      integer, "
	"ps_expire        integer, "
	"ps_addr_created  text, "
	"ps_addr_modified text, "
	"ps_error         integer, "
	"ps_warning       integer, "
	"ps_notice        integer, "
	"ps_err_message   text, "
	"ps_access       integer, "
	"ps_reserved      integer); ";
	char *query_app_vars =
	"CREATE TABLE php_app_vars ( "
	"app_modified       integer, "
	"app_name           text, "
	"app_vars           text);";
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
						   "CREATE INDEX php_session_idx ON php_session USING BTREE (ps_id);");
		PQclear(pg_result);
		pg_result = PQexec(PS_PGSQL(pgsql_link)[id], query_app_vars);
		if (PQresultStatus(pg_result) != PGRES_COMMAND_OK) {
			ret = FAILURE;
		}
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
/* static int php_ps_pgsql_close(TSRMLS_D)  */
/* { */
/* 	return SUCCESS; */
/* } */
/* }}} */

/* {{{ php_pg_pgsql_gc
 */
static int php_ps_pgsql_gc(TSRMLS_D)
{
	PGresult *pg_result;
	char query[QUERY_BUF_SIZE+1];
	char *query_tmpl = "DELETE FROM php_session WHERE ps_expire < %d;";
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

	zend_register_auto_global("_APP", sizeof("_APP")-1 TSRMLS_CC);
	
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
int ps_pgsql_app_read(PGconn *pg_link TSRMLS_DC) 
{
	PGresult *pg_result;
	char query[QUERY_BUF_SIZE+1];
	int ret = SUCCESS;
	
	if (PS_PGSQL(use_app_vars)) {
		sprintf(query, "SELECT app_vars FROM php_app_vars WHERE app_name = '%s';", PS(session_name));
		pg_result = PQexec(pg_link, query);
		ALLOC_ZVAL(PS_PGSQL(app_vars));				
		if (PQresultStatus(pg_result) == PGRES_TUPLES_OK) {
			if (PQntuples(pg_result) == 0) {
				array_init(PS_PGSQL(app_vars));
				INIT_PZVAL(PS_PGSQL(app_vars));
				ZEND_SET_GLOBAL_VAR_WITH_LENGTH("_APP", sizeof("_APP"), PS_PGSQL(app_vars), 1, 0);
				PS_PGSQL(app_new) = 1;
			}
			else {
				php_unserialize_data_t var_hash;
				char *data;
				
				data = PQgetvalue(pg_result, 0, 0);
				
				PHP_VAR_UNSERIALIZE_INIT(var_hash);
				php_var_unserialize(&PS_PGSQL(app_vars), (const char **)&data, data + strlen(data), &var_hash TSRMLS_CC); 
				PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
				ZEND_SET_GLOBAL_VAR_WITH_LENGTH("_APP", sizeof("_APP"), PS_PGSQL(app_vars), 1, 0);
				INIT_PZVAL(PS_PGSQL(app_vars));
			}
 		}
		else {
			php_error(E_WARNING,"Session pgsql READ(applicatoin vars) failed: %s (%s)",
					  PQresultErrorMessage(pg_result), query);
			ret = FAILURE;
		}
		PQclear(pg_result);
 	}
	return ret;
}
/* }}} */

/* {{{ ps_pgsql_app_write
 */
static int ps_pgsql_app_write(PGconn *pg_link TSRMLS_DC)
{
	PGresult *pg_result;
	char *query_insert = "INSERT INTO php_app_vars (app_modified, app_name, app_vars) VALUES (%d, '%s', '%s');";
	char *query_update = "UPDATE php_app_vars SET app_modified = %d, app_vars = '%s'";
	char *query = NULL;
	unsigned char *escaped_data = NULL;
	size_t escaped_data_len = 0;
	php_serialize_data_t var_hash;
	smart_str buf = {0};
	

	assert(PS(session_name) != NULL);

	PHP_VAR_SERIALIZE_INIT(var_hash);
	php_var_serialize(&buf, &(PS_PGSQL(app_vars)), &var_hash TSRMLS_CC);
	PHP_VAR_SERIALIZE_DESTROY(var_hash);

	assert(buf.c && buf.len);
	escaped_data = php_addslashes(buf.c, buf.len, &escaped_data_len, 1 TSRMLS_CC);
	if (PS_PGSQL(app_new)) {
		/* INSERT */
		query = emalloc(strlen(query_insert)+strlen(PS(session_name))+escaped_data_len+16);
		sprintf(query, query_insert,time(NULL),PS(session_name),escaped_data);
		pg_result = PQexec(pg_link, query);
		if (PQresultStatus(pg_result) != PGRES_COMMAND_OK) {
			php_error(E_WARNING, "Session pgsql $_APP write(insert) failed. (%s)", PQerrorMessage(pg_link) TSRMLS_CC);
		}
	}
	else {
		/* UPDATE */
		query = emalloc(strlen(query_insert)+escaped_data_len+16);
		sprintf(query, query_update,time(NULL),escaped_data);
		pg_result = PQexec(pg_link, query);
		if (PQresultStatus(pg_result) != PGRES_COMMAND_OK) {
			php_error(E_WARNING, "Session pgsql $_APP write(update) failed. (%s) ", PQerrorMessage(pg_link) TSRMLS_CC);
		}
	}
	PQclear(pg_result);
	efree(query);
	efree(escaped_data);
	
	return SUCCESS;
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


#define PS_PGSQL_CHECK_APP_NEW() 1

/* {{{ PS_READ_FUNC
 */
PS_READ_FUNC(pgsql)
{
	PGconn *pg_link;
	PGresult *pg_result;
	ExecStatusType pg_status;
	char query[QUERY_BUF_SIZE+1];
	char *query_tpl = "SELECT ps_expire, ps_access, ps_data FROM php_session WHERE ps_id = '%s';";
	int ret = FAILURE;

	ELOG("READ Called");
	assert(PS(session_name));

	if (!(pg_link = php_ps_pgsql_get_link(key TSRMLS_CC))) {
		return FAILURE;
	}
	
	if (pg_link == NULL) {
		return FAILURE;
	}
	if (PQstatus(pg_link) == CONNECTION_BAD) {
		PQfinish(pg_link);
		return FAILURE;
	}
	
	/* clean up & check last write */
	while ((pg_result = PQgetResult(pg_link))) {
		pg_status = PQresultStatus(pg_result);
		switch(pg_status) {
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
		pg_status = PQresultStatus(pg_result);
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
					PS_PGSQL(sess_cnt) = (int)atoi(PQgetvalue(pg_result, 0, 1)) + 1;
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
			php_error(E_WARNING,"Session pgsql READ(session vars) failed: %s (%s)",
					  PQresultErrorMessage(pg_result), query);
			ret = FAILURE;
		}
		PQclear(pg_result);	
	}
	else {
		php_error(E_NOTICE,"Session ID is not valid");
	}
	
	
	if (*vallen == 0) {
		*val = safe_estrndup("",0);
	}

	/* Init app vars */
	PS_PGSQL(app_new) = 0;
	if (PS_PGSQL_CHECK_APP_NEW()) {
		ps_pgsql_app_read(pg_link TSRMLS_CC);
	}

  	return ret;
/* 	return SUCCESS; */
}
/* }}} */

#define PS_PGSQL_CHECK_APP_MODIFIED() 1

/* {{{ PS_WRITE_FUNC
 */
PS_WRITE_FUNC(pgsql)
{
	PGconn *pg_link;
	PGresult *pg_result;
	size_t query_len;
	time_t now, exp;
	char *data;
	char *query;
	char *query_delete =
	   "DELETE FROM php_session WHERE ps_id = '%s';";
	char *query_insert =
	   "INSERT INTO php_session (ps_id, ps_created, ps_modified, ps_expire, ps_access, ps_addr_created, ps_addr_modified, ps_data) "
	   "VALUES ('%s', %d, %d, %d, 1, '%s', '%s', '%s');";
	char *query_update =
	   "UPDATE php_session SET ps_modified = %d , ps_expire = %d, ps_access = %d, ps_addr_modified = '%s', ps_data = '%s' "
	   "WHERE ps_id = '%s';";
 	int len; 
	zval *addr = NULL;

	ELOG("WRITE Called");

	if (!(pg_link = php_ps_pgsql_get_link(key TSRMLS_CC))) {
		return FAILURE;
	}
	
	if (PS_PGSQL(sess_del)) {
		query_len = strlen(query_delete)+strlen(key);
		query = emalloc(query_len+1);
		sprintf(query, query_delete, key);
		pg_result = PQexec(pg_link, query);
		efree(query);
		PQclear(pg_result);		
	}

	now = time(NULL);
	exp = now + PS(gc_maxlifetime);
	data = (void *)php_addslashes((char *)val, strlen(val), &len, 0 TSRMLS_CC);
	MAKE_STD_ZVAL(addr);
/* 	if (zend_hash_find(Z_ARRVAL_P(PG(http_globals)[TRACK_VARS_SERVER]), "REMOTE_ADDR", sizeof("REMOTE_ADDR"),(void **)&addr) == FAILURE) { */
/* 		Z_STRVAL_P(addr) = ""; */
/* 		Z_STRLEN_P(addr) = 0; */
/* 	} */
	Z_STRVAL_P(addr) = "Not implemented";
	Z_STRLEN_P(addr) = sizeof("Not implemented");
		
	if (PS_PGSQL(sess_new)) {
		/* INSERT */
		query_len = strlen(query_insert)+strlen(data)+strlen(key)+120;
		query = emalloc(query_len+1);
 		sprintf(query, query_insert, key, now, now, exp, Z_STRVAL_P(addr), Z_STRVAL_P(addr), data);
		pg_result = PQexec(pg_link, query);
		if (PQresultStatus(pg_result) != PGRES_COMMAND_OK) {
			php_error(E_WARNING, "PostgreSQL session. %s", PQresultErrorMessage(pg_result));
		}
		PQclear(pg_result);		
	}
	else {
		/* UPDATE */
		query_len = strlen(query_update)+strlen(key)+strlen(data)+120;
		query = emalloc(query_len+1);
 		sprintf(query, query_update, now, exp, PS_PGSQL(sess_cnt), Z_STRVAL_P(addr), data, key);
		pg_result = PQexec(pg_link, query);
		if (PQresultStatus(pg_result) != PGRES_COMMAND_OK) {
			php_error(E_WARNING, "PostgreSQL session. %s", PQresultErrorMessage(pg_result));
		}
		PQclear(pg_result);		
	}
	efree(query);
	efree(data);
	FREE_ZVAL(addr);

	/* Save Application Variables */
	if (PS_PGSQL_CHECK_APP_MODIFIED()) {
		ps_pgsql_app_write(pg_link TSRMLS_CC);
	}
	
	pg_result = PQexec(pg_link, "END;");
	if (PQresultStatus(pg_result) != PGRES_COMMAND_OK) {
		php_error(E_WARNING, "PostgreSQL session. %s", PQresultErrorMessage(pg_result));
	}
	PQclear(pg_result);

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
	char *query_update = "DELETE FROM php_session WHERE ps_id = '%s';";
	int ret = FAILURE;

	ELOG("DESTROY Called");
	
	if (!(pg_link = php_ps_pgsql_get_link(key TSRMLS_CC))) {
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
	PGconn *pg_link;
	
	if (ZEND_NUM_ARGS() != 0) {
		WRONG_PARAM_COUNT;
		RETURN_FALSE;
	}
	
	if (!PS(id)) {
		php_error(E_NOTICE, "%s() cannot find session id.",
				  get_active_function_name(TSRMLS_C));
		RETURN_FALSE;
	}

	if (!(pg_link = php_ps_pgsql_get_link(PS(id) TSRMLS_CC))) {
		RETURN_STRING("Session pgsql is NOT working", 1);
	}
	RETURN_STRING("Session pgsql is working", 1);
}
/* }}} */

/* {{{ proto int session_pgsql_status(void)
   Returns current session info */
PHP_FUNCTION(session_pgsql_info)
{
	PGconn *pg_link;
	PGresult *pg_result;
	int argc = ZEND_NUM_ARGS();
	char *id = NULL;
	size_t id_len = 0;
	char query[QUERY_BUF_SIZE+1];
	char *query_select = "SELECT ps_created, ps_addr_created, ps_modified, ps_addr_modified, ps_expire, ps_access, ps_error, ps_warning, ps_notice, ps_err_message  FROM php_session WHERE ps_id = '%s';";

	if (zend_parse_parameters(argc TSRMLS_CC, "|l",
							  &id, &id_len) == FAILURE) {
		return;
	}
	if (id_len > 32) {
		php_error(E_NOTICE, "%s() accepts session id string.",
				  get_active_function_name(TSRMLS_C));
		RETURN_FALSE;
	}
	if (!id)
		id = PS(id);
	
	if (!(pg_link = php_ps_pgsql_get_link(PS(id) TSRMLS_CC))) {
		php_error(E_WARNING, "%s() cannot find valid connection.",
				  get_active_function_name(TSRMLS_C));
		RETURN_FALSE;
	}

	sprintf(query, query_select, id);
	pg_result = PQexec(pg_link, query);
	if (PQresultStatus(pg_result) != PGRES_TUPLES_OK) {
		PQclear(pg_result);
		php_error(E_NOTICE, "%s() query failed. %s",
				  get_active_function_name(TSRMLS_C), PQresultErrorMessage(pg_result));
		RETURN_FALSE;
	}
	if (PQntuples(pg_result) == 0) {
		PQclear(pg_result);
		php_error(E_NOTICE, "%s() cannot find session record. %s",
				  get_active_function_name(TSRMLS_C), PQresultErrorMessage(pg_result));
		RETURN_FALSE;
	}
	array_init(return_value);
	add_assoc_long(return_value, "Date Created", atoi(PQgetvalue(pg_result, 0, 0)));
	add_assoc_string(return_value, "Created By", PQgetvalue(pg_result, 0, 1), 1);
	add_assoc_long(return_value, "Date Modified", atoi(PQgetvalue(pg_result, 0, 2)));
	add_assoc_string(return_value, "Modified By", PQgetvalue(pg_result, 0, 3), 1);
	add_assoc_long(return_value, "Expire", atoi(PQgetvalue(pg_result, 0, 4)));
	add_assoc_long(return_value, "Access", atoi(PQgetvalue(pg_result, 0, 5)));
	add_assoc_long(return_value, "Error", atoi(PQgetvalue(pg_result, 0, 6)));
	add_assoc_long(return_value, "Warning", atoi(PQgetvalue(pg_result, 0, 7)));
	add_assoc_long(return_value, "Notice", atoi(PQgetvalue(pg_result, 0, 8)));
	add_assoc_string(return_value, "Error Message", PQgetvalue(pg_result, 0, 9), 1);	
}
/* }}} */

/* {{{ proto bool session_pgsql_add_error(int error_level [, string error_message])
   Increments error counts and sets last error message */
PHP_FUNCTION(session_pgsql_add_error)
{
	PGconn *pg_link;
	PGresult *pg_result;
	int argc = ZEND_NUM_ARGS();
	int err_lvl;
	char *err_msg = NULL;
	size_t err_msg_len = 0;
	char query[QUERY_BUF_SIZE+1];
	char *query_select = "SELECT ps_error, ps_warning, ps_notice FROM  php_session WHERE ps_id = '%s';";
	char *query_update =
	"UPDATE php_session SET "
	"ps_err_message = '%s', "
	"ps_error = %d, "
	"ps_warning = %d, "
	"ps_notice = %d "
	"WHERE ps_id = '%s';";
	char *query_update2 =
	"UPDATE php_session SET "
	"ps_error = %d, "
	"ps_warning = %d, "
	"ps_notice = %d "
	"WHERE ps_id = '%s';";	
	int cnt_error, cnt_warning, cnt_notice;
	
	if (zend_parse_parameters(argc TSRMLS_CC, "l|s",
							  &err_lvl, &err_msg, &err_msg_len) == FAILURE) {
		return;
	}

	if (!PS(id)) {
		php_error(E_NOTICE, "%s() cannot find session id",
				  get_active_function_name(TSRMLS_C));
		RETURN_FALSE;
	}

	if (!(pg_link = php_ps_pgsql_get_link(PS(id) TSRMLS_CC))) {
		php_error(E_WARNING, "%s() cannot find valid connection",
				  get_active_function_name(TSRMLS_C));
		RETURN_FALSE;
	}
	
	sprintf(query, query_select, PS(id));
	pg_result = PQexec(pg_link, query);
	if (PQresultStatus(pg_result) != PGRES_TUPLES_OK) {
		PQclear(pg_result);
		php_error(E_NOTICE, "%s() query failed (%s)",
				  get_active_function_name(TSRMLS_C), PQresultErrorMessage(pg_result));
		RETURN_FALSE;
	}
	if (PQgetvalue(pg_result, 0, 0))
		cnt_error = atoi(PQgetvalue(pg_result, 0, 0));
	else
		cnt_error = 0;
	if (PQgetvalue(pg_result, 0, 1))
		cnt_warning = atoi(PQgetvalue(pg_result, 0, 1));
	else
		cnt_warning = 0;
	if (PQgetvalue(pg_result, 0, 2))
		cnt_notice = atoi(PQgetvalue(pg_result, 0, 2));
	else
		cnt_notice = 0;
	
	switch (err_lvl) {
		case E_USER_ERROR:
			cnt_error++;
			break;
		case E_USER_WARNING:
			cnt_warning++;
			break;
		case E_USER_NOTICE:
			cnt_notice++;
			break;
		default:
			php_error(E_WARNING, "%s() expects E_USER_ERROR, E_USER_WARNING or E_USER_NOTICE for 1st arg",
					  get_active_function_name(TSRMLS_C));
			RETURN_FALSE;
	}
		
	if (argc = 1) {
		if (err_msg_len > 400) {
			php_error(E_NOTICE, "%s() error message is too long and trancated (Max 400 bytes)",
					  get_active_function_name(TSRMLS_C));
			err_msg_len = 400;
			err_msg[400] = '\0';
		}
		err_msg = php_addslashes(err_msg, err_msg_len, &err_msg_len, 0 TSRMLS_CC);
		sprintf(query, query_update, err_msg, cnt_error, cnt_warning, cnt_notice, PS(id));
		efree(err_msg);
	}
	else {
		sprintf(query, query_update2, cnt_error, cnt_warning, cnt_notice, PS(id));
	}
	
	pg_result = PQexec(pg_link, query);
	if (PQresultStatus(pg_result) != PGRES_COMMAND_OK) {
		PQclear(pg_result);
		php_error(E_NOTICE, "%s() query failed. (%s)",
				  get_active_function_name(TSRMLS_C), PQresultErrorMessage(pg_result));
		RETURN_FALSE;
	}
	PQclear(pg_result);

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto array session_pgsql_get_error([string session_id])
   Returns number of errors and last error message */
PHP_FUNCTION(session_pgsql_get_error)
{
	PGconn *pg_link;
	PGresult *pg_result;
	int argc = ZEND_NUM_ARGS();
	char *id = NULL;
	size_t id_len = 0;
	char query[QUERY_BUF_SIZE+1];
	char *query_select = "SELECT ps_error, ps_warning, ps_notice, ps_err_message  FROM php_session WHERE ps_id = '%s';";

	if (zend_parse_parameters(argc TSRMLS_CC, "|l",
							  &id, &id_len) == FAILURE) {
		return;
	}
	if (id_len > 32) {
		php_error(E_NOTICE, "%s() accepts session id string.",
				  get_active_function_name(TSRMLS_C));
		RETURN_FALSE;
	}
	if (!id)
		id = PS(id);
	
	if (!(pg_link = php_ps_pgsql_get_link(PS(id) TSRMLS_CC))) {
		php_error(E_WARNING, "%s() cannot find valid connection.",
				  get_active_function_name(TSRMLS_C));
		RETURN_FALSE;
	}

	sprintf(query, query_select, id);
	pg_result = PQexec(pg_link, query);
	if (PQresultStatus(pg_result) != PGRES_TUPLES_OK) {
		PQclear(pg_result);
		php_error(E_NOTICE, "%s() query failed. %s",
				  get_active_function_name(TSRMLS_C), PQresultErrorMessage(pg_result));
		RETURN_FALSE;
	}
	if (PQntuples(pg_result) == 0) {
		PQclear(pg_result);
		php_error(E_NOTICE, "%s() cannot find session record. %s",
				  get_active_function_name(TSRMLS_C), PQresultErrorMessage(pg_result));
		RETURN_FALSE;
	}
	array_init(return_value);
	add_assoc_long(return_value, "Error", atoi(PQgetvalue(pg_result, 0, 0)));
	add_assoc_long(return_value, "Warning", atoi(PQgetvalue(pg_result, 0, 1)));
	add_assoc_long(return_value, "Notice", atoi(PQgetvalue(pg_result, 0, 2)));
	add_assoc_string(return_value, "Error Message", PQgetvalue(pg_result, 0, 3), 1);
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
