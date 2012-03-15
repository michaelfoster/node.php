<?php

$http = new node_http();

$http->listen(8080, function($request, $response) {
    echo "in connection callback\n";
    print_r($request);
    print_r($response);
    $response->end("yay, super awesome response");
});

nodephp_run();