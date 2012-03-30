<?php

$http = new node_http();

$http->listen(8080, function($request, $response) {
    $response->setHeader("Content-Type", "text/plain");
    $response->end("yay, super awesome response");
});

nodephp_run();
echo "after loop\n";