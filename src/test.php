<?php

$http = new node_http();

$http->listen(8080, function($request, $response) {
    echo "in connection callback\n";
    print_r($request);
});

nodephp_run();