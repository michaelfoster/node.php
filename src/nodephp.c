#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

// php deps
#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"

// nodephp deps
#include "uv.h"
#include "http_parser.h"

#include "nodephp.h"

#ifdef ZTS
#include "TSRM.h"
# define TSRMLS_SET(o)    (o)->TSRMLS_C = TSRMLS_C
# define TSRMLS_GET(o)    TSRMLS_C = (o)->TSRMLS_C
# define TSRMLS_D_GET(o)  TSRMLS_D = (o)->TSRMLS_C

#else /* ZTS not defined */
# define TSRMLS_SET(o)    /* empty */
# define TSRMLS_GET(o)    /* empty */
# define TSRMLS_D_GET(o)  /* empty */

#endif /* ZTS not defined */

/* Shamelessly nicked from mongo-php-driver */
#if ZEND_MODULE_API_NO >= 20100525
#define init_properties(obj, class_type)     \
  object_properties_init((obj), class_type)
#else
#define init_properties(obj, class_type)     \
  do {                                       \
  zval *tmp;                                 \
  zend_hash_copy((obj)->properties,          \
    &class_type->default_properties,         \
    (copy_ctor_func_t) zval_add_ref,         \
    (void *) &tmp,                           \
    sizeof(zval*));                          \
  }                                          \
  while (0)
#endif

typedef struct _http_wrap_t {
  zend_object obj;
  uv_tcp_t handle;
  http_parser parser;
  struct _http_wrap_t *parent;
  char *header;
  zval *request;
  zval *headers;
  zval *close_cb;
  zval *connection_cb;
#ifdef ZTS
  TSRMLS_D;
#endif
} http_wrap_t;

typedef struct {
  uv_connect_t request;
  zval *callback;
#ifdef ZTS
  TSRMLS_D;
#endif
} connect_wrap_t;

typedef struct {
  zend_object obj;
  uv_write_t request;
  uv_tcp_t *socket;
  char *response;
  zval *callback;
  zval *string;
#ifdef ZTS
  TSRMLS_D;
#endif
} http_response_t;

// private protocols
uv_buf_t _http_on_alloc(uv_handle_t *client, size_t suggested_size);
void _http_on_read(uv_stream_t *client, ssize_t nread, uv_buf_t buf);
void _http_on_close(uv_handle_t *client);
int _http_on_message_begin(http_parser *parser);
int _http_on_url(http_parser *parser, const char *at, size_t length);
int _http_on_header_field(http_parser *parser, const char *at, size_t length);
int _http_on_header_value(http_parser *parser, const char *at, size_t length);
int _http_on_headers_complete(http_parser *parser);
int _http_on_body(http_parser *parser, const char *at, size_t length);
int _http_on_message_complete(http_parser *parser);
void _after_http_write(uv_write_t *request, int status);

struct http_parser_settings _http_parser_settings = {
  .on_message_begin    = _http_on_message_begin,
  .on_url              = _http_on_url,
  .on_header_field     = _http_on_header_field,
  .on_header_value     = _http_on_header_value,
  .on_headers_complete = _http_on_headers_complete,
  .on_body             = _http_on_body,
  .on_message_complete = _http_on_message_complete
};

// class entries
zend_class_entry *http_server_ce;
zend_class_entry *http_server_response_ce;

static void http_wrap_free(void *object TSRMLS_DC) {
  http_wrap_t *wrap = (http_wrap_t*) object;
  zend_object_std_dtor(&wrap->obj TSRMLS_CC);
  efree(wrap);
}

static void http_response_free(void *object TSRMLS_DC) {
  http_response_t *response = (http_response_t*) object;
  zend_object_std_dtor(&response->obj TSRMLS_CC);
  efree(response);
}

static zend_object_value http_new(zend_class_entry *class_type TSRMLS_DC) {
  zend_object_value instance;
  http_wrap_t *wrap;

  wrap = (http_wrap_t*) emalloc(sizeof *wrap);

  uv_tcp_init(uv_default_loop(), &wrap->handle);

  zend_object_std_init(&wrap->obj, class_type TSRMLS_CC);
  init_properties(&wrap->obj, class_type);

  TSRMLS_SET(wrap);

  wrap->handle.data = (void*) wrap;
  wrap->connection_cb = NULL;

  instance.handle = zend_objects_store_put((void*) wrap,
                                           (zend_objects_store_dtor_t) zend_objects_destroy_object,
                                           http_wrap_free,
                                           NULL
                                           TSRMLS_CC);
  instance.handlers = zend_get_std_object_handlers();

  return instance;
}

static zend_object_value http_response_new(zend_class_entry *class_type TSRMLS_DC) {
  zend_object_value instance;
  http_response_t *wrap;

  wrap = (http_response_t*) emalloc(sizeof *wrap);

  zend_object_std_init(&wrap->obj, class_type TSRMLS_CC);
  init_properties(&wrap->obj, class_type);

  TSRMLS_SET(wrap);

  instance.handle = zend_objects_store_put((void*) wrap,
                                           (zend_objects_store_dtor_t) zend_objects_destroy_object,
                                           http_response_free,
                                           NULL
                                           TSRMLS_CC);
  instance.handlers = zend_get_std_object_handlers();

  return instance;
}


void call_callback(zval* callback, int argc, zval *argv[] TSRMLS_DC) {
  zend_fcall_info fci = empty_fcall_info;
  zend_fcall_info_cache fci_cache = empty_fcall_info_cache;
  char *is_callable_error = NULL;
  zval* result;

  if (zend_fcall_info_init(callback, 0, &fci, &fci_cache, NULL, &is_callable_error TSRMLS_CC) == SUCCESS) {
    fci.retval_ptr_ptr = &result;
    zend_fcall_info_argn(&fci, argc, &argv[0], &argv[1]);
    zend_call_function(&fci, &fci_cache TSRMLS_CC);
  }
}

// libuv callbacks

void _http_on_close(uv_handle_t *client) {
  // nothing exciting here to do, php takes care of
  // cleaning up the objects
}

uv_buf_t _http_on_alloc(uv_handle_t *client, size_t suggested_size) {
  return uv_buf_init(emalloc(suggested_size), suggested_size);
}

void _http_on_read(uv_stream_t *client, ssize_t nread, uv_buf_t buf) {
  if (nread > 0) {
    http_wrap_t *http_wrap = client->data;

    ssize_t s = http_parser_execute( &http_wrap->parser
                                   , &_http_parser_settings
                                   , buf.base
                                   , nread
                                   );

    if (s < nread) {
      uv_close((uv_handle_t*) client, _http_on_close);
      // TODO: notify the user of a parse error?
    } 
  } else {
    uv_err_t error = uv_last_error(uv_default_loop());
    if (error.code == UV_EOF) {
      uv_close((uv_handle_t*)client, _http_on_close);
    } else {
      // TODO: notify the user of IO error?
    }
  }
  efree(buf.base);
}

// http parser callbacks
int _http_on_message_begin(http_parser *parser) {
  http_wrap_t *http_wrap = parser->data;

  ALLOC_INIT_ZVAL(http_wrap->request);
  ALLOC_INIT_ZVAL(http_wrap->headers);
  array_init(http_wrap->request);
  array_init(http_wrap->headers);

  return 0;
}

int _http_on_url(http_parser *parser, const char *at, size_t length) {
  http_wrap_t *http_wrap = parser->data;
  zval *data = http_wrap->request;
  char *tmp = estrndup(at, length);

  add_assoc_string(data, "url", tmp, 1);

  efree(tmp);
  return 0;
}

int _http_on_header_field(http_parser *parser, const char *at, size_t length) {
  http_wrap_t *http_wrap = parser->data;

  http_wrap->header = estrndup(at, length);

  return 0;
}

int _http_on_header_value(http_parser *parser, const char *at, size_t length) {
  http_wrap_t *http_wrap = parser->data;
  char *tmp = estrndup(at, length);

  add_assoc_string(http_wrap->headers, http_wrap->header, tmp, 1);

  efree(http_wrap->header);
  efree(tmp);
  return 0;
}

int _http_on_headers_complete(http_parser *parser) {
  http_wrap_t *http_wrap = parser->data;
  zval *data = http_wrap->request;

  add_assoc_zval(data, "headers", http_wrap->headers);

  return 0;
}

int _http_on_body(http_parser *parser, const char *at, size_t length) {
  http_wrap_t *http_wrap = parser->data;
  zval *data = http_wrap->request;
  char *tmp = estrndup(at, length);

  add_assoc_string(data, "body", tmp, 1);

  free(tmp);
  return 0;
}

int _http_on_message_complete(http_parser *parser) {
  http_wrap_t *http_wrap = parser->data;
  zval *cb = http_wrap->parent->connection_cb;
  zval *data = http_wrap->request;
  zval *r_zval;
  http_response_t *response;

  // call the request callback
  if (cb) {
    MAKE_STD_ZVAL(r_zval);
    Z_TYPE_P(r_zval) = IS_OBJECT;
    zend_object_value object = http_response_new(http_server_response_ce TSRMLS_CC);
    Z_OBJVAL_P(r_zval) = object;
    response = (http_response_t*) zend_object_store_get_object(r_zval TSRMLS_CC);
    response->socket = &http_wrap->handle;

    zval *args[3];
    args[0] = data;
    args[1] = r_zval;

    call_callback(cb, 2, args TSRMLS_CC);
  }

  return 0;
}

void _after_http_write(uv_write_t *request, int status) {
  // nothing to do here yet
}

// hold over code
void http_connection_cb(uv_stream_t* server_handle, int status) {
  http_wrap_t* self = (http_wrap_t*) server_handle->data;
  http_wrap_t* client_wrap;
  zval* client_zval;
  int r;
  TSRMLS_D_GET(self);

  if (status != 0) {
    /* TODO: do something sensible */
    return;
  }

  /* Create container for new rerquest object */
  MAKE_STD_ZVAL(client_zval);
  Z_TYPE_P(client_zval) = IS_OBJECT;
  Z_OBJVAL_P(client_zval) = http_new(http_server_ce TSRMLS_CC);
  client_wrap = (http_wrap_t*) zend_object_store_get_object(client_zval TSRMLS_CC);

  /* Accept connection */
  r = uv_accept(server_handle, (uv_stream_t*) &client_wrap->handle);
  if (r != 0) {
    /* This should not happen */
    return;
  }

  uv_read_start( (uv_stream_t*) &client_wrap->handle
               , _http_on_alloc
               , _http_on_read
               );

  http_parser_init(&client_wrap->parser, HTTP_REQUEST);

  client_wrap->handle.data = client_wrap;
  client_wrap->parser.data = client_wrap;
  client_wrap->parent = self;
};

PHP_METHOD(node_http, listen) {
  http_wrap_t* self;
  zval* arg1, *arg2, *arg3;
  zval* port, *host, *callback;
  struct sockaddr_in addr;
  int r;

  self = (http_wrap_t*) zend_object_store_get_object(getThis() TSRMLS_CC);

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z|z!z!", &arg1, &arg2, &arg3) == FAILURE) {
    return;
  }

  if (Z_TYPE_P(arg1) == IS_LONG) {
    port = arg1;
  } else {
    RETURN_NULL();
  }

  if (ZEND_NUM_ARGS() == 3) {
    host = arg2;
    callback = arg3;
  } else if (ZEND_NUM_ARGS() == 2) {
    if (Z_TYPE_P(arg2) == IS_STRING) {
      host = arg2;
      callback = NULL;
    } else {
      host = NULL;
      callback = arg2;
    }
  } else {
    callback = NULL;
    host = NULL;
  }

  if (host != NULL) {
    addr = uv_ip4_addr(Z_STRVAL_P(host), Z_LVAL_P(port));
  } else {
    addr = uv_ip4_addr("0.0.0.0", Z_LVAL_P(port));
  }

  r = uv_tcp_bind(&self->handle, addr);
  if (r != 0) {
    RETURN_NULL();
  }

  r = uv_listen((uv_stream_t*) &self->handle, 512, http_connection_cb);
  if (r != 0) {
    RETURN_NULL();
  }

  if (callback) {
    self->connection_cb = callback;
    Z_ADDREF_P(callback);
  }

  RETURN_NULL();
}

static zend_function_entry http_server_methods[] = {
  PHP_ME(node_http, listen, NULL, ZEND_ACC_PUBLIC)
  { NULL }
};

PHP_METHOD(node_http_response, end) {
  http_response_t *self;
  zval *arg1;
  char *http_res = "HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/plain\r\n"
                   "Content-Length: %d\r\n"
                   "\r\n%s";

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &arg1) == FAILURE) {
    return;
  }

  if (Z_TYPE_P(arg1) != IS_STRING) {
    return;
  }

  self = (http_response_t*) zend_object_store_get_object(getThis() TSRMLS_CC);

  self->response = emalloc(strlen(http_res) + 20 + Z_STRLEN_P(arg1));

  sprintf(self->response, http_res, Z_STRLEN_P(arg1), Z_STRVAL_P(arg1));

  uv_buf_t buf;
  buf.base = self->response;
  buf.len = strlen(self->response);

  uv_write( &self->request
          , (uv_stream_t*)self->socket
          , &buf
          , 1
          , _after_http_write
          );

  RETURN_NULL();
}

static zend_function_entry http_server_response_methods[] = {
  PHP_ME(node_http_response, end, NULL, ZEND_ACC_PUBLIC)
  { NULL }
};

PHP_MINIT_FUNCTION(nodephp) {
  zend_class_entry http_ce;
  zend_class_entry request_ce;

  // register the http object with php
  INIT_CLASS_ENTRY(http_ce, "node_http", http_server_methods);
  http_ce.create_object = http_new;
  http_server_ce = zend_register_internal_class(&http_ce TSRMLS_CC);

  // register the response object with php
  INIT_CLASS_ENTRY( request_ce
                  , "node_http_response"
                  , http_server_response_methods
                  );
  request_ce.create_object = http_response_new;
  http_server_response_ce = zend_register_internal_class(&request_ce TSRMLS_CC);


  return SUCCESS;
}


PHP_MSHUTDOWN_FUNCTION(nodephp) {
  return SUCCESS;
}


PHP_MINFO_FUNCTION(nodephp) {
  php_info_print_table_start();
  php_info_print_table_header(2, "nodephp", "enabled");
  php_info_print_table_end();
}

PHP_FUNCTION(nodephp_run)
{
  uv_run(uv_default_loop());
  RETURN_NULL();
}

static function_entry nodephp_functions[] = {
  PHP_FE(nodephp_run, NULL)
  {NULL, NULL, NULL}
};

zend_module_entry nodephp_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
  STANDARD_MODULE_HEADER,
#endif
  NODEPHP_EXTNAME,
  nodephp_functions,
  PHP_MINIT(nodephp),
  PHP_MSHUTDOWN(nodephp),
  NULL,
  NULL,
  PHP_MINFO(nodephp),
#if ZEND_MODULE_API_NO >= 20010901
  NODEPHP_VERSION,
#endif
    STANDARD_MODULE_PROPERTIES
};

ZEND_GET_MODULE(nodephp)
