#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// deps
#include "uv.h"
#include "http_parser.h"

#include "nodephp.h"
#include "node_http.h"
#include "node_function.h"

// private protocols
void _http_wrap_free(void *object TSRMLS_DC);
void _http_response_free(void *object TSRMLS_DC);
void _on_http_connection(uv_stream_t* server_handle, int status);
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
void _after_http_response_end(uv_write_t *request, int status);

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
  PHP_ME(node_http_response, end, NULL, ZEND_ACC_PUBLIC)
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
  
  zval_dtor(request->request);
  zval_dtor(request->headers);

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

  add_assoc_stringl(data, "url", (char*)at, length, 1);

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
  //Z_DELREF_P(request->headers);

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
    // free the result of the closure imediately
    FREE_ZVAL(result);
    // free the response object if it's not longer referenced
    if (Z_DELREF_P(r_zval) == 0) {
        FREE_ZVAL(r_zval);
    }

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

PHP_METHOD(node_http_response, end) {
  zval *arg1;
  zend_object *self;
  http_response_t *response;
  char *http_res = "HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/plain\r\n"
                   "Content-Length: %d\r\n"
                   "\r\n%s";
  int result = zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &arg1);

  if (result == FAILURE || Z_TYPE_P(arg1) != IS_STRING) {
    RETURN_BOOL(0);
  }

  self = zend_object_store_get_object(getThis() TSRMLS_CC);
  response = (http_response_t*) self;
  zend_objects_store_add_ref_by_handle(response->handle);

  response->response = emalloc(strlen(http_res) + 20 + Z_STRLEN_P(arg1));
  sprintf(response->response, http_res, Z_STRLEN_P(arg1), Z_STRVAL_P(arg1));

  uv_buf_t buf;
  buf.base = response->response;
  buf.len = strlen(response->response);

  response->request.data = response;

  uv_write( &response->request
          , (uv_stream_t*)response->socket
          , &buf
          , 1
          , _after_http_response_end
          );

  RETURN_BOOL(1);
}
