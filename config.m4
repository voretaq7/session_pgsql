dnl
dnl $Id: config.m4,v 1.1.1.1 2002/02/17 02:35:54 yohgaki Exp $
dnl

PHP_ARG_WITH(session-pgsql,for pgsql sesssion support,
[  --with-session-pgsql[=DIR] Include pgsql(PostgreSQL) support for session storage])

if test "$PHP_SESSION_PGSQL" != "no"; then

  dnl
  dnl check libmm
  dnl
  LIBNAME=mm
  LIBSYMBOL=mm_create

  for i in /usr/local /usr ; do
    if test -f "$i/include/mm.h"; then
      MM_DIR=$i
    fi
  done

  if test -z "$MM_DIR" ; then
    AC_MSG_ERROR([cannot find mm.h under /usr/local /usr])
  fi

  if test -z "MM_DIR/lib/libmm.so"; then
    AC_MSG_ERROR([cannot find libmm.so /usr/local /usr])
  fi
  
  PHP_CHECK_LIBRARY($LIBNAME, $LIBSYMBOL,
   [
     PHP_ADD_LIBRARY_WITH_PATH($LIBNAME, $MM_DIR/lib, MM_SHARED_LIBADD)
     AC_DEFINE(HAVE_MMLIB,1,[Whether you have libmm or not])
   ],[
     AC_MSG_ERROR([wrong libmm version or libmm not found])
   ],[
     -L$MM_DIR/lib
   ])
  
  PHP_SUBST(MM_SHARED_LIBADD)
  PHP_ADD_INCLUDE($MM_DIR/include)

  dnl
  dnl check libpq
  dnl
  LIBNAME=pq
  LIBSYMBOL=PQexec

  if test "$PHP_SESSION_PGSQL" = "yes"; then
    PGSQL_SEARCH_PATHS="/usr /usr/local /usr/local/pgsql"
  else
    PGSQL_SEARCH_PATHS=$PHP_SESSION_PGSQL
  fi
  
  for i in $PGSQL_SEARCH_PATHS; do
    for j in include include/pgsql include/postgres include/postgresql ""; do
      if test -r "$i/$j/libpq-fe.h"; then
        PGSQL_INC_BASE=$i
        PGSQL_INCLUDE=$i/$j
      fi
    done

    for j in lib lib/pgsql lib/postgres lib/postgresql ""; do
      if test -f "$i/$j/libpq.so"; then 
        PGSQL_LIBDIR=$i/$j
      fi
    done
  done

  if test -z "$PGSQL_INCLUDE"; then
    AC_MSG_ERROR([Cannot find libpq-fe.h. Please specify correct PostgreSQL installation path])
  fi

  if test -z "$PGSQL_LIBDIR"; then
    AC_MSG_ERROR([Cannot find libpq.so. Please specify correct PostgreSQL installation path])
  fi

  if test -z "$PGSQL_INCLUDE" -a -z "$PGSQL_LIBDIR" ; then
    AC_MSG_ERROR([Unable to find libpq anywhere under $withval])
  fi

  PHP_CHECK_LIBRARY($LIBNAME, $LIBSYMBOL,
   [
     PHP_ADD_LIBRARY_WITH_PATH($LIBNAME, $PGSQL_LIBDIR, PGSQL_SHARED_LIBADD)
     AC_DEFINE(HAVE_LIBPQ,1,[Whether you have libpq or not])
   ],[
     AC_MSG_ERROR([wrong libpq version or libpq not found])
   ],[
     -L$PGSQL_LIBDIR
   ])
  
  PHP_SUBST(PGSQL_SHARED_LIBADD)
  PHP_ADD_INCLUDE($PGSQL_INCLUDE)

  AC_DEFINE(HAVE_SESSION_PGSQL, 1, [Whether you have pgsql session save handler])
  PHP_EXTENSION(session_pgsql, $ext_shared)
fi
