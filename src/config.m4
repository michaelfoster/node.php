dnl $Id$
dnl config.m4 for extension nodephp

PHP_ARG_ENABLE(redis, whether to enable nodephp support,
dnl Make sure that the comment is aligned:
[  --enable-nodephp           Enable nodephp support])

if test "$PHP_NODEPHP" != "no"; then
  PHP_ADD_INCLUDE(../deps/libuv/include)
  PHP_ADD_INCLUDE(../deps/http-parser)
  NODEPHP_SHARED_LIBADD='../deps/libuv/uv.a ../deps/http-parser/http_parser.o'
  PHP_SUBST(NODEPHP_SHARED_LIBADD)
  PHP_NEW_EXTENSION(nodephp, nodephp.c node_http.c node_events.c node_function.c, $ext_shared)
fi
