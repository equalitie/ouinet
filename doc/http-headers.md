# Ouinet-specific HTTP headers

The headers described in this document are used to communicate additional
instructions and results to and from a user program which utilizes a Ouinet
client's HTTP proxy interface.  These headers are handled by the Ouinet client
and injector and they are never sent to the origin nor cached.

## Synchronous injection

This mechanism allows a Ouinet client triggering an injection to get the
related descriptor (and other data base-dependent information) in an atomical
way, right at the moment of the injection.  This can allow a content
publishing tool to get, without further lookups, the exact descriptors
corresponding to the injections that it caused, and thus distribute the
published content along with the information that allows its reseeding.

This behaviour is enabled by the user program adding the `X-Ouinet-Sync: true`
header to the HTTP request.  If (and only if) the request causes a new
injection, the HTTP response will contain an `X-Ouinet-Descriptor` header with
the zlib-compressed, Base64-encoded descriptor, as well as an
`X-Ouinet-Descriptor-Link` header pointing to a distributed storage address
where the descriptor can be retrieved from (currently an IPFS address).

If the data base allows autonomous reinsertion of descriptors, an
`X-Ouinet-Insert-<DB>` header is added with Base64-encoded data base-specific
information to help reinsert.  If the data base supports storing the
descriptor inline, the value of `X-Ouinet-Descriptor` should be used for
reinsertion, otherwise the value of `X-Ouinet-Descriptor-Link` is used.

Here is a request/response example with synchronous injection enabled on a
BEP44 data base (which supports autonomous reinsertion):

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
    X-Ouinet-Descriptor-Link: /ipfs/Qm…
    X-Ouinet-Insertion-BEP44: <BASE64(BEP44_INSERTION_DATA))>

    <!DOCTYPE html>\n<p>Tiny body here!</p>

This type of injection happens at the expense of increasing latency in the
handling of the response, since the injector is not able to generate the
descriptor until it has seen the full response (head and body) from the origin
and performed whatever distributed cache seeding to be announced in the
descriptor.  The added latency should be acceptable to non-interactive
publishing tools.

(Future versions of Ouinet may use HTTP trailers to avoid some delays.)

## Asynchronous injection

In contrast with synchronous injections, those triggered by interactive user
agents (like browsers) behind Ouinet clients happen in an asynchronous way,
i.e. the injector may complete the injection (or decide to avoid it) way after
the response from the origin has been forwarded to the requesting Ouinet
client.

This avoids delays that may affect the browsing experience (hence this is the
default behaviour), but it also means that clients have no easy way of
identifying and seeding the descriptor corresponding to the request/response
whose content they are seeding (in fact, looking up its URI may yield a
descriptor for different data or an unrelated request).

To help identify the descriptor, injector HTTP responses (either from the
origin or the cache) include an `X-Ouinet-Injection-ID`  header with an
identifier string.  Here is a request/response example:

    GET https://example.com/foo HTTP/1.1
    Host: example.com
    Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8

    HTTP/1.1 200 OK
    Date: Mon, 15 Jan 2018 20:31:50 GMT
    Server: Apache
    Content-Type: text/html
    Content-Disposition inline; filename="foo.html"
    Content-Length: 38
    X-Ouinet-Injection-ID: <some unique identifier>

    <!DOCTYPE html>\n<p>Tiny body here!</p>

The identifier is eventually added to the resulting descriptor (when the
injection takes place) and the client can use it to track the state of an
injection asynchronously, for instance to retrieve the descriptor
corresponding to a URI that it requested and help decide whether to seed it or
not (in certain data bases like IPNS/IPFS, the lookup itself may help with
future lookups by retrieving intermediate structures like B-tree nodes).
