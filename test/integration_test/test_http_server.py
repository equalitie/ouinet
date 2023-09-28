#!/usr/bin/env python

from twisted.internet import reactor
from twisted.web.server import Site
from twisted.web.resource import Resource

from test_fixtures import TestFixtures


class TestPage(Resource):
    isLeaf = True

    def render_GET(self, request):
        if b"content" in request.args:
            return request.args[b"content"][0]
        else:
            return TestFixtures.TEST_PAGE_BODY


def test_http_server(port):
    return reactor.listenTCP(port, Site(TestPage()))


if __name__ == "__main__":
    my_http_server = test_http_server(8080)
    reactor.run()
