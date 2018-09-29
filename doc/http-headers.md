# Ouinet-specific HTTP headers

The headers described in this document are used to communicate additional
instructions and results to and from a user program which utilizes a Ouinet
client's HTTP proxy interface.  These headers are handled by the Ouinet client
and injector and they are never sent to the origin nor cached.

## Synchronous insertion

This mechanism allows a Ouinet client triggering an injection to get the
related descriptor (and other data base-dependent information) in an atomical
way, right at the moment of the injection.  This can allow a content
publishing tool to get, without further lookups, the exact descriptors
corresponding to the injections that it caused, and thus distribute the
published content along with the information that allows its reseeding.

This behaviour is enabled by the user program adding the `X-Ouinet-Sync: true`
header to the HTTP request.  If (and only if) the request causes a new
injection, the HTTP response will contain an `X-Ouinet-Descriptor` header with
the zlib-compressed, Base64-encoded descriptor.  Here is a request/response
example:

    GET https://example.com/foo HTTP/1.1
    Host: example.com
    Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8
    X-Ouinet-Sync: true

    HTTP/1.1 200 OK
    Date: Mon, 15 Jan 2018 20:31:50 GMT
    Server: Apache
    Content-Type: text/html
    Content-Disposition inline; filename="foo.html"
    Content-Length: 38
    X-Ouinet-Descriptor: <BASE64(ZLIB(DESCRIPTOR))>

    <!DOCTYPE html>\n<p>Tiny body here!</p>

This type of insertion happens at the expense of increasing latency in the
handling of the response, since the injector is not able to generate the
descriptor until it has seen the full response (head and body) from the origin
and performed whatever distributed cache seeding to be announced in the
descriptor.  The added latency should be acceptable to non-interactive
publishing tools.

(Future versions of Ouinet may use HTTP trailers to avoid some delays.)
