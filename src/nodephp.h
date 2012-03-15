#ifndef nodephp_node_h
#define nodephp_node_h

#define NODEPHP_VERSION "1.0"
#define NODEPHP_EXTNAME "nodephp"

PHP_FUNCTION(nodephp_run);

extern zend_module_entry nodephp_module_entry;
#define phpext_nodephp_ptr &nodephp_module_entry

#endif // nodephp_node_h
