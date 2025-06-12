#!/usr/bin/python2

from twisted.internet import reactor, endpoints
from twisted.web.server import Site
from twisted.web.resource import Resource

import os
import logging

from test_fixtures import TestFixtures

class TestPage(Resource):
    isLeaf = True
    def render_GET(self, request):
        logging.debug(f"GET request received:")
        logging.debug(f"Path: {request.path}")
        logging.debug(f"Headers: {request.requestHeaders}")
        logging.debug(f"Client IP: {request.getClientIP()}")
        if b"content" in request.args:
            return request.args[b"content"][0]
        elif b"content_size" in request.args:
            content_size_in_bytes =  int(request.args[b"content_size"][0])
            return os.urandom(content_size_in_bytes)
        else:
            return TestFixtures.TEST_PAGE_BODY

def TestHttpServer(port):
        return reactor.listenTCP(port, Site(TestPage()))

if __name__ == "__main__":
    my_http_server = TestHttpServer(8080)
    reactor.run()
