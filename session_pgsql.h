/* 
   +----------------------------------------------------------------------+
   | PHP Version 4                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2002 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 2.02 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available at through the world-wide-web at                           |
   | http://www.php.net/license/2_02.txt.                                 |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: yohgaki@php.net                                             |
   +----------------------------------------------------------------------+
 */

#ifndef MOD_PGSQL_H
#define MOD_PGSQL_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_SESSION_PGSQL

#include <libpq-fe.h>
#include "ext/session/php_session.h"

extern ps_module ps_mod_pgsql;
#define ps_pgsql_ptr &ps_mod_pgsql

extern zend_module_entry session_pgsql_module_entry;
#define phpext_session_pgsql_ptr &session_pgsql_module_entry

PS_FUNCS(pgsql);

#define MAX_PGSQL_SERVERS 16

typedef struct _php_session_pgsql_globals {
	char *db;
	int failover;
	int loadbalance;
	int serializable;
	int create_table;
	PGconn *pgsql_link[MAX_PGSQL_SERVERS];
	char *connstr[MAX_PGSQL_SERVERS];
	int table_exist[MAX_PGSQL_SERVERS];
	int servers;
	int sess_new;
	int sess_del;
	int sess_cnt;
	/* FIXME: gc related vars must be shared mem :) */
	int gc_interval;
	time_t last_gc;
	int maxlifetime;
	int failovered;
} php_session_pgsql_globals; 

/* php function registration */
PHP_FUNCTION(session_pgsql_status);
PHP_FUNCTION(session_pgsql_info);
PHP_FUNCTION(session_pgsql_add_error);
PHP_FUNCTION(session_pgsql_get_error);

#ifdef ZTS
extern int session_pgsql_globals_id;
#define PS_PGSQL(v) TSRMG(session_pgsql_globals_id, php_session_pgsql_globals *, v)
#else
extern php_session_pgsql_globals session_pgsql_globals;
#define PS_PGSQL(v) (session_pgsql_globals.v)
#endif

#else

#define ps_pgsql_ptr NULL
#define phpext_session_pgsql_ptr NULL

#endif

#endif
