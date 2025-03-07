#!/usr/bin/python2

from twisted.internet import reactor, endpoints
from twisted.web.server import Site
from twisted.web.resource import Resource
import time
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
        else:
            return TestFixtures.TEST_PAGE_BODY

def TestHttpServer(port):
        return reactor.listenTCP(port, Site(TestPage()))

if __name__ == "__main__":
    my_http_server = TestHttpServer(8080)
    reactor.run()
