#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// deps
#include "uv.h"
#include "http_parser.h"

#include "nodephp.h"
#include "node_http.h"
#include "node_function.h"

//// private protocols

// auxilary node_http_response methods
uv_buf_t _http_response_send_headers(http_response_t *self, int send);
size_t _http_response_get_header_length(http_response_t *self);
void _http_response_default_headers(http_response_t *self);
void _http_response_set_default_header(http_response_t *self, 
                                       char *key, size_t key_len, 
                                       char *val, size_t val_len);
uv_buf_t _http_response_send_headers_if_not_sent(http_response_t *self, 
                                                 int send);
void _http_response_write(http_response_t *self, char *data, size_t len);
void _http_response_end(http_response_t *self, char *data, size_t len);

// libuv callbacks
void _on_http_connection(uv_stream_t* server_handle, int status);
uv_buf_t _http_on_alloc(uv_handle_t *client, size_t suggested_size);
void _http_on_read(uv_stream_t *client, ssize_t nread, uv_buf_t buf);
void _http_on_close(uv_handle_t *client);
void _after_http_response_end(uv_write_t *request, int status);

// http parser callbacks
int _http_on_message_begin(http_parser *parser);
int _http_on_url(http_parser *parser, const char *at, size_t length);
int _http_on_header_field(http_parser *parser, const char *at, size_t length);
int _http_on_header_value(http_parser *parser, const char *at, size_t length);
int _http_on_headers_complete(http_parser *parser);
int _http_on_body(http_parser *parser, const char *at, size_t length);
int _http_on_message_complete(http_parser *parser);

static char HTTP_OK[] = "HTTP/1.1 %s OK\r\n";
static size_t HTTP_OK_LEN = 17;

// set the callbacks for the http parser
 http_parser_settings _http_parser_settings = {
  .on_message_begin    = _http_on_message_begin,
  .on_url              = _http_on_url,
  .on_header_field     = _http_on_header_field,
  .on_header_value     = _http_on_header_value,
  .on_headers_complete = _http_on_headers_complete,
  .on_body             = _http_on_body,
  .on_message_complete = _http_on_message_complete
};

// register methods for the node_http class
zend_function_entry http_server_methods[] = { 
  PHP_ME(node_http, listen, NULL, ZEND_ACC_PUBLIC)
  NODEPHP_END_FUNCTIONS
};

// register methods for the node_http_resonse class
zend_function_entry http_server_response_methods[] = {
  PHP_ME(node_http_response, writeContinue, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(node_http_response, writeHead,     NULL, ZEND_ACC_PUBLIC)
  PHP_ME(node_http_response, setStatus,     NULL, ZEND_ACC_PUBLIC)
  PHP_ME(node_http_response, setHeader,     NULL, ZEND_ACC_PUBLIC)
  PHP_ME(node_http_response, getHeader,     NULL, ZEND_ACC_PUBLIC)
  PHP_ME(node_http_response, removeHeader,  NULL, ZEND_ACC_PUBLIC)
  PHP_ME(node_http_response, addTrailers,   NULL, ZEND_ACC_PUBLIC)
  PHP_ME(node_http_response, write,         NULL, ZEND_ACC_PUBLIC)
  PHP_ME(node_http_response, end,           NULL, ZEND_ACC_PUBLIC)
  NODEPHP_END_FUNCTIONS
};

// ctors and dtors

zend_object_value http_new(zend_class_entry *class_type TSRMLS_DC) {
  zend_object_value instance;
  http_wrap_t *wrap = emalloc(sizeof(http_wrap_t));

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

void http_wrap_free(void *object TSRMLS_DC) {
  http_wrap_t *http = (http_wrap_t*) object;
  zend_objects_free_object_storage(&http->obj TSRMLS_CC);
}

zend_object_value http_response_new(zend_class_entry *class_type TSRMLS_DC) {
  zend_object_value instance;
  http_response_t *response = emalloc(sizeof(http_response_t));

  ALLOC_INIT_ZVAL(response->headers);
  array_init(response->headers);
  response->headers_sent = 0;
  response->status = NULL;

  zend_object_std_init(&response->obj, class_type TSRMLS_CC);
  init_properties(&response->obj, class_type);

  instance.handle = zend_objects_store_put((void*) response,
                                           (zend_objects_store_dtor_t) zend_objects_destroy_object,
                                           http_response_free,
                                           NULL
                                           TSRMLS_CC);
  instance.handlers = zend_get_std_object_handlers();

  response->handle = instance.handle;

  return instance;
}

void http_response_free(void *object TSRMLS_DC) {
  http_response_t *response = (http_response_t*) object;
  zval_ptr_dtor(&response->headers);
  zend_objects_free_object_storage(&response->obj TSRMLS_CC);
}

// libuv callbacks

void _on_http_connection(uv_stream_t* server_handle, int status) {
  http_wrap_t *self = (http_wrap_t*) server_handle->data;

  if (status != 0) {
    // TODO: emit error
    return;
  }

  http_request_t *client = emalloc(sizeof(http_request_t));
  uv_tcp_init(uv_default_loop(), &client->handle);

  int r = uv_accept(server_handle, (uv_stream_t*) &client->handle);
  if (r != 0) {
    // TODO: emit error
    return;
  }

  uv_read_start( (uv_stream_t*) &client->handle
               , _http_on_alloc
               , _http_on_read
               );

  http_parser_init(&client->parser, HTTP_REQUEST);

  client->handle.data = client;
  client->parser.data = client;
  client->parent = self;
};

void _http_on_close(uv_handle_t *client) {
  http_request_t *request = client->data;
  
  zval_ptr_dtor(&request->request);
  zval_ptr_dtor(&request->headers);

  efree(request);
}

uv_buf_t _http_on_alloc(uv_handle_t *client, size_t suggested_size) {
  return uv_buf_init(emalloc(suggested_size), suggested_size);
}

void _http_on_read(uv_stream_t *client, ssize_t nread, uv_buf_t buf) {
  if (nread > 0) {
    http_request_t *request = client->data;

    ssize_t s = http_parser_execute( &request->parser
                                   , &_http_parser_settings
                                   , buf.base
                                   , nread
                                   );

    if (s < nread) {
      uv_close((uv_handle_t*) client, _http_on_close);
    } 
  } else {
    uv_err_t error = uv_last_error(uv_default_loop());
    if (error.code == UV_EOF) {
      uv_close((uv_handle_t*)client, _http_on_close);
    } else {
      // TODO: emit error
    }
  }
  efree(buf.base);
}

// http parser callbacks
int _http_on_message_begin(http_parser *parser) {
  http_request_t *request = parser->data;

  ALLOC_INIT_ZVAL(request->request);
  ALLOC_INIT_ZVAL(request->headers);
  array_init(request->request);
  array_init(request->headers);
  Z_ADDREF_P(request->request);
  Z_ADDREF_P(request->headers);

  return 0;
}

int _http_on_url(http_parser *parser, const char *at, size_t length) {
  http_request_t *request = parser->data;
  zval *data = request->request;
  char *method = (char *) http_method_str(parser->method);

  add_assoc_stringl(data, "url", (char*)at, length, 1);
  add_assoc_stringl(data, "method", method, length, 1);

  return 0;
}

int _http_on_header_field(http_parser *parser, const char *at, size_t length) {
  http_request_t *request = parser->data;

  request->header = estrndup(at, length);

  return 0;
}

int _http_on_header_value(http_parser *parser, const char *at, size_t length) {
  http_request_t *request = parser->data;

  add_assoc_stringl(request->headers, request->header, (char*)at, length, 1);
  efree(request->header);

  return 0;
}

int _http_on_headers_complete(http_parser *parser) {
  http_request_t *request = parser->data;
  zval *data = request->request;

  add_assoc_zval(data, "headers", request->headers);
  Z_DELREF_P(request->headers);

  return 0;
}

int _http_on_body(http_parser *parser, const char *at, size_t length) {
  http_request_t *request = parser->data;
  zval *data = request->request;

  add_assoc_stringl(data, "body", (char*)at, length, 1);

  return 0;
}

int _http_on_message_complete(http_parser *parser) {
  http_request_t *request = parser->data;
  zval *cb = request->parent->connection_cb;
  zval *data = request->request;

  // call the request callback
  if (cb) {
    zval *r_zval, *result;
    MAKE_STD_ZVAL(r_zval);
    Z_TYPE_P(r_zval) = IS_OBJECT;
    zend_object_value object = http_response_new(http_server_response_ce TSRMLS_CC);
    Z_OBJVAL_P(r_zval) = object;
    http_response_t *response = (http_response_t*) zend_object_store_get_object_by_handle(
      object.handle TSRMLS_CC
    );
    response->socket = &request->handle;

    result = node_function_call_zval(cb, 2, &data, &r_zval);

    zval_ptr_dtor(&result);
    zval_ptr_dtor(&r_zval);

    zend_objects_store_del_ref_by_handle_ex(object.handle, object.handlers);
  }

  return 0;
}

void _after_http_response_end(uv_write_t *request, int status) {
  http_response_t *self = (http_response_t*) request->data;
  uv_stream_t *handle = request->handle;
  efree(self->response);
  zend_objects_store_del_ref_by_handle(self->handle);
  uv_close((uv_handle_t*) handle, _http_on_close);
}

// node_http_response auxilary methods

uv_buf_t _http_response_send_headers(http_response_t *self, int send) {
  //  size_t offset = HTTP_OK_LEN;
  HashTable *ht = Z_ARRVAL_P(self->headers);
  zval **data;
  char *key;
  //  uint key_len;
  ulong index;
  uv_buf_t buf;

  _http_response_default_headers(self);

  buf.len = _http_response_get_header_length(self) + HTTP_OK_LEN;
  buf.base = emalloc(buf.len);
  sprintf(buf.base, HTTP_OK, Z_STRVAL_P(self->status));
  for(zend_hash_internal_pointer_reset(ht); 
      zend_hash_get_current_data(ht, (void**) &data) == SUCCESS; 
      zend_hash_move_forward(ht)) { 
    zend_hash_get_current_key(ht, &key, &index, 0);
    php_printf("%s: %s\n", key, Z_STRVAL_PP(data));
    strcat(buf.base, key);
    strcat(buf.base, ": ");
    strcat(buf.base, Z_STRVAL_PP(data));
    strcat(buf.base, "\r\n");
  }

  strcat(buf.base, "\r\n");

  if (!send) { return buf; }

  // TODO: write headers to socket when send == 1
  buf.len = 0;
  return buf;
}

size_t _http_response_get_header_length(http_response_t *self) {
  size_t acc = 2;
  HashTable *ht = Z_ARRVAL_P(self->headers);
  zval *data;
  char *key;
  ulong index;

  php_printf("==scanning\n");
  for(zend_hash_internal_pointer_reset(ht); 
      zend_hash_get_current_data(ht, (void**) &data) == SUCCESS; 
      zend_hash_move_forward(ht)) { 
    zend_hash_get_current_key(ht, &key, &index, 0);
    php_printf("%s: %s(%d)\n", key, Z_STRVAL_P(data), Z_STRLEN_P(data));
    acc += Z_STRLEN_P(data) + strlen(key) + 4;
  }
  php_printf("alloc size: %zu\n", acc);
  return acc;
}

void _http_response_default_headers(http_response_t *self) {
  if (self->status == NULL) {
    MAKE_STD_ZVAL(self->status);
    ZVAL_STRING(self->status, "200", 1);
  }

  _http_response_set_default_header(self, "Content-Type", 12, "text/html", 9); 

  int result = zend_hash_exists( Z_ARRVAL_P(self->headers)
                               , "Content-Length"
                               , 14
                               );
  if (result) { return; }
  // TODO: fix this hash lookup
  return;

  zval *encodings;
  result = zend_hash_find( Z_ARRVAL_P(self->headers)
                         , "Transfer-Encoding"
                         , 17
                         , (void**) &encodings
                         );

  if (result == FAILURE) {
    add_assoc_stringl_ex( self->headers
                        , "Transfer-Encoding"
                        , 17
                        , "chunked"
                        , 8
                        , 1
                        );
    self->is_chunked = 1;
  } else {
    char *substr = strstr(Z_STRVAL_P(encodings), "chunked");
    if (substr == NULL) {
      substr = emalloc(Z_STRLEN_P(encodings) + 11);
      sprintf(substr, "%s, \"chunked\"", Z_STRVAL_P(encodings));
      efree(Z_STRVAL_P(encodings));
      Z_STRVAL_P(encodings) = substr;
      self->is_chunked = 1;
    }
  }
}

void _http_response_set_default_header(http_response_t *self, 
                                       char *key, size_t key_len, 
                                       char *val, size_t val_len) {
  zval *ret;
  int result = zend_hash_find( Z_ARRVAL_P(self->headers)
                             , key
                             , key_len + 1
                             , (void**)&ret
                             );
  php_printf("== setting\n");
  if (result == FAILURE) {
    php_printf("default not found for: %s\n", key);
    zval *new;
    MAKE_STD_ZVAL(new);
    ZVAL_STRING(new, val, 1);
    zval_copy_ctor(new);
    zend_hash_update( Z_ARRVAL_P(self->headers)
                    , key
                    , key_len + 1
                    , new
                    , sizeof(zval*)
                    , NULL
                    );
    php_printf("setting %s: %s(%d)\n", key, Z_STRVAL_P(new), Z_STRLEN_P(new));
    add_assoc_stringl(self->headers, key, val, val_len, 1);
  } else {
    php_printf("default found for %s: %s\n", key, Z_STRVAL_P(ret));
  }
}

uv_buf_t _http_response_send_headers_if_not_sent(http_response_t *self, 
                                                 int send) {
  uv_buf_t buf;
  if (!self->headers_sent) {
    return _http_response_send_headers(self, send);
  }
  buf.len = 0;
  return buf;
}

void _http_response_write(http_response_t *self, char *data, size_t len) {
  
}

void _http_response_end(http_response_t *self, char *data, size_t len) {
  
}

// class methods

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

  r = uv_listen((uv_stream_t*) &self->handle, 512, _on_http_connection);
  if (r != 0) {
    RETURN_NULL();
  }

  if (callback) {
    self->connection_cb = callback;
    Z_ADDREF_P(callback);
  }

  RETURN_NULL();
}

PHP_METHOD(node_http_response, writeContinue) {
  // TODO: implement
  // NOTE: takes no args
  RETURN_NULL();
}

PHP_METHOD(node_http_response, writeHead) {
  zend_object *self = zend_object_store_get_object(getThis() TSRMLS_CC);
  http_response_t *response = (http_response_t*) self;

  if (response->headers_sent) {
    RETURN_BOOL(0);
  }

  // TODO: implement the actual writing of the headers

  RETURN_BOOL(1);
}

PHP_METHOD(node_http_response, setStatus) {
  zend_object *self = zend_object_store_get_object(getThis() TSRMLS_CC);
  http_response_t *response = (http_response_t*) self;
  zval *status;
  int result = zend_parse_parameters( ZEND_NUM_ARGS() TSRMLS_CC
                                    , "z"
                                    , &status
                                    );

  if (response->headers_sent || result == FAILURE) {
    RETURN_BOOL(0);
  }

  if (response->status != NULL) { zval_ptr_dtor(&response->status); }
  zval_copy_ctor(status);
  response->status = status;

  RETURN_BOOL(1);
}

PHP_METHOD(node_http_response, setHeader) {
  zend_object *self = zend_object_store_get_object(getThis() TSRMLS_CC);
  http_response_t *response = (http_response_t*) self;
  zval *key, *value, *new;
  int result = zend_parse_parameters( ZEND_NUM_ARGS() TSRMLS_CC
                                    , "zz"
                                    , &key
                                    , &value
                                    );

  if (response->headers_sent || result == FAILURE) {
    RETURN_BOOL(0);
  }

  if (Z_TYPE_P(key) != IS_STRING || Z_TYPE_P(value) != IS_STRING) {
    RETURN_BOOL(0);
  }

  MAKE_STD_ZVAL(new);
  ZVAL_ZVAL(new, value, 1, 0);
  zend_hash_update( Z_ARRVAL_P(response->headers)
                  , Z_STRVAL_P(key)
                  , Z_STRLEN_P(key) + 1
                  , new
                  , sizeof(zval*)
                  , NULL
                  );

  RETURN_BOOL(1);
}

PHP_METHOD(node_http_response, getHeader) {
  zend_object *self = zend_object_store_get_object(getThis() TSRMLS_CC);
  http_response_t *response = (http_response_t*) self;
  zval *header, *value;
  int result = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &header);

  if (result == FAILURE || Z_TYPE_P(header) != IS_STRING) {
    RETURN_BOOL(0);
  }

  result = zend_hash_find( Z_ARRVAL_P(response->headers)
                         , Z_STRVAL_P(header)
                         , Z_STRLEN_P(header) + 1
                         , (void**)&value
                         );

  if (result == SUCCESS) {
    RETURN_STRING(Z_STRVAL_P(value), 1);
  } else {
    RETURN_BOOL(0);
  }
}

PHP_METHOD(node_http_response, removeHeader) {
  zend_object *self = zend_object_store_get_object(getThis() TSRMLS_CC);
  http_response_t *response = (http_response_t*) self;
  zval *header;
  int result = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &header);

  if (response->headers_sent) {
    RETURN_BOOL(0);
  }

  if (result == FAILURE || Z_TYPE_P(header) != IS_STRING) {
    RETURN_BOOL(0);
  }

  result = zend_hash_del( Z_ARRVAL_P(response->headers)
                        , Z_STRVAL_P(header)
                        , Z_STRLEN_P(header)
                        );

  RETURN_BOOL(result == SUCCESS);
}

PHP_METHOD(node_http_response, addTrailers) {
  // TODO: implement
  // NOTE: takes headers as an array
  RETURN_NULL();
}

PHP_METHOD(node_http_response, write) {
  // TODO: implement
  // NOTE: takes body as a string
  RETURN_NULL();
}

PHP_METHOD(node_http_response, end) {
  zval *body;
  zend_object *self;
  http_response_t *response;
  uv_buf_t bufs[2];
  int buf_len = 0;
  int result = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &body);

  if (result == FAILURE || Z_TYPE_P(body) != IS_STRING) {
    RETURN_BOOL(0);
  }

  self = zend_object_store_get_object(getThis() TSRMLS_CC);
  response = (http_response_t*) self;
  zend_objects_store_add_ref_by_handle(response->handle);

  zval *length;
  MAKE_STD_ZVAL(length)
  ZVAL_LONG(length, Z_STRLEN_P(body));
  convert_to_string(length);

  _http_response_set_default_header( response
                                   , "Content-Length"
                                   , 14
                                   , Z_STRVAL_P(length)
                                   , Z_STRLEN_P(length)
                                   );

  zval_ptr_dtor(&length);

  bufs[0] = _http_response_send_headers_if_not_sent(response, 0);
  if (bufs[0].len) { buf_len++; }

  response->response = emalloc(Z_STRLEN_P(body) + 2);
  sprintf(response->response, "%s", Z_STRVAL_P(body));

  bufs[buf_len].base = response->response;
  bufs[buf_len].len = strlen(Z_STRVAL_P(body));

  response->request.data = response;
  uv_write( &response->request
          , (uv_stream_t*)response->socket
          , bufs
          , ++buf_len
          , _after_http_response_end
          );

  RETURN_BOOL(1);
}
