#ifndef nodephp_function_h
#define nodephp_function_h

#include "php.h"

zval* node_function_call_zval(zval *func, int argc, ...);
zend_bool node_function_is_zval_callable(zval *func);

#endif
