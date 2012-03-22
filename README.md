Node.php
========
Evented I/O for PHP

### Example HTTP server
    <?php
    
    $http = new node_http();
    
    $http->listen(8080, function($request, $response) {
        $response->end("yay, super awesome response");
    });
    
    ?>

nodephp_run();

### To build:
(I apologize for such the manual build process, this will be simplified)
You have to first manually build everything in deps/
    git submodule init
    git submodule update
    make -C deps/http-parser
    make -C deps/libuv
Then you must build the PHP module.
    cd src/
    phpize
    ./configure
    make
    make install