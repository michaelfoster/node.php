#ifndef nodephp_http_h
#define nodephp_http_h

#include "php.h"

typedef struct _http_wrap_t http_wrap_t;
typedef struct _http_request_t http_request_t;
typedef struct _http_response_t http_response_t;

struct _http_wrap_t {
  zend_object obj;
  uv_tcp_t handle;
  zval *close_cb;
  zval *connection_cb;
#ifdef ZTS
  TSRMLS_D;
#endif
};

struct _http_request_t {
  uv_tcp_t handle;
  http_parser parser;
  http_wrap_t *parent;
  char *header;
  zval *request;
  zval *headers;
};

struct  _http_response_t {
  zend_object obj;
  uv_write_t request;
  zend_object_handle handle;
  uv_tcp_t *socket;
  zval *headers;
  zval *status;
  unsigned int headers_sent : 1;
  char *response;
  zval *callback;
  zval *string;
#ifdef ZTS
  TSRMLS_D;
#endif
};

// object ctors and dtors
zend_object_value http_new(zend_class_entry *class_type TSRMLS_DC);
void http_wrap_free(void *object TSRMLS_DC);
zend_object_value http_response_new(zend_class_entry *class_type TSRMLS_DC);
void http_response_free(void *object TSRMLS_DC);

// node_http object methods
PHP_METHOD(node_http, listen);

// node_http_response object methods
PHP_METHOD(node_http_response, writeContinue);
PHP_METHOD(node_http_response, writeHead);
PHP_METHOD(node_http_response, setStatus);
PHP_METHOD(node_http_response, setHeader);
PHP_METHOD(node_http_response, getHeader);
PHP_METHOD(node_http_response, removeHeader);
PHP_METHOD(node_http_response, addTrailers);
PHP_METHOD(node_http_response, write);
PHP_METHOD(node_http_response, end);

extern zend_function_entry http_server_methods[];
extern zend_function_entry http_server_response_methods[];

#endif
