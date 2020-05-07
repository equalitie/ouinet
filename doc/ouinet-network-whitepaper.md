- [Overview](#overview)
- [Aims and Threat Model](#aims-and-threat-model)
- [Actors and Components](#actors-and-components)
  - [The injector servers](#the-injector-servers)
  - [The client library](#the-client-library)
  - [Support nodes](#support-nodes)
  - [End-user applications](#end-user-applications)
- [Content Access Systems](#content-access-systems)
  - [Direct origin access](#direct-origin-access)
  - [Injector server connection](#injector-server-connection)
  - [Distributed cache lookups](#distributed-cache-lookups)
- [Distributed Cache](#distributed-cache)
  - [Introduction](#introduction)
  - [Cache structure](#cache-structure)
    - [Cache entry construction](#cache-entry-construction)
      - [Resource canonicalization](#resource-canonicalization)
      - [Added metadata](#added-metadata)
      - [Cache eligibility](#cache-eligibility)
    - [Validity checking](#validity-checking)
    - [Signatures](#signatures)
      - [Signature computation](#signature-computation)
      - [Stream signatures](#stream-signatures)
      - [Cryptographic primitives](#cryptographic-primitives)
      - [Examples](#examples)
  - [Distribution](#distribution)
    - [Injector-to-client cache entry exchange](#injector-to-client-cache-entry-exchange)
      - [Signature streams](#signature-streams)
      - [Complete cache entry signatures](#complete-cache-entry-signatures)
      - [Error responses](#error-responses)
      - [Examples](#examples-1)
    - [Peer-to-peer cache entry exchange](#peer-to-peer-cache-entry-exchange)
      - [Peer-to-peer connections](#peer-to-peer-connections)
      - [Cache entry requests](#cache-entry-requests)
      - [Range requests](#range-requests)
      - [Examples](#examples-2)
      - [Distributed hash table](#distributed-hash-table)
      - [Resource groups](#resource-groups)
    - [Out of band cache entry exchange](#out-of-band-cache-entry-exchange)
- [Injector Servers](#injector-servers)
  - [Injector server connections](#injector-server-connections)
    - [Injector uTP sockets](#injector-utp-sockets)
    - [Peer-to-peer tunnels](#peer-to-peer-tunnels)
    - [Other connection methods](#other-connection-methods)
  - [Injector server functionality](#injector-server-functionality)
    - [Cache injection requests](#cache-injection-requests)
    - [Cache-ineligible proxy requests](#cache-ineligible-proxy-requests)
    - [TLS tunnels](#tls-tunnels)
- [Client Library](#client-library)
  - [Request headers](#request-headers)
  - [Response headers](#response-headers)

  
# Overview

The Ouinet network is a collection of software tools and support infrastructure that provide access to web resources, using a variety of different mechanisms, tailored for scenarios of limited internet connectivity and selective network traffic filtering. By using different content access mechanisms as network conditions require, Ouinet can provide the best web access the situation allows, degrading gracefully when faced with increasing degrees of network disruption.

The Ouinet network contains a collection of specialized public proxy servers, known as *injector servers*, combined with a variety of means by which a user can construct a direct or indirect connection to those injector servers. Together these form one way in which a user can access web resources they cannot reach directly.

In addition, Ouinet uses a peer-to-peer distributed content cache system that can store and distribute cached copies of web resources. Users and contributors of Ouinet can connect to this network to share copies of web content they recently accessed, This is especially useful to other users who cannot reliably reach that resource or an injector server. The content cache can be used only for those web resources that are eligible for caching, and as such cannot fully replace access to the injector servers. Nonetheless, the distributed content cache system can lighten the load on the injector server system during network conditions in which access to the injector servers is a sparse resource, and it can serve as a limited fallback in cases where the injector servers cannot be reached at all.

The Ouinet project consists of two primary pieces of software. The *client library* contains all the logic necessary to fetch web resources using the multiple Ouinet techniques for doing so, as well as optionally participating in the distributed cache. It is structured as a library, and can function as a base for end-user applications that access the web. The *injector daemon* implements the injector server software, which is hosted by the Ouinet network operator. This document describes the workings of both of these systems, as well as the networks underlying their operation.



# Aims and Threat Model



# Actors and Components

The Ouinet network relies on the cooperation of a few different parties with different roles and responsibilities. Some of these components form a centralized infrastructure, whereas others form a decentralized cooperative of users and contributors to the system.


## The injector servers

The main backing infrastructure of the Ouinet network consists of a set of *injector servers*. A Ouinet injector server is a variant of an open proxy server, that users can connect to in order to gain access to web resources they cannot access via more direct means. Injector servers also have other responsibilities, described in more detail in the [Distributed Cache](#distributed-cache) section.

Injector servers form a trusted part of the Ouinet network. In their role as an intermediary between a user and their communication to much of the internet, they are in a position where a malicious implementation could do serious harm to user security. To keep track of the impact of this trusted position, injector servers are identified by a cryptographic identity that is used to authenticate the injector server when connecting to it, as well as certifying data published by the injector server.

Different organizations can run their own collections of injector servers, and users of the Ouinet system can configure their devices or applications to use whatever injector servers they wish to trust. The Ouinet organization operates one such cluster of injector servers for general purpose use, but there is no requirement for anyone using the Ouinet system to make use of these injector servers, and grant it authority thereby.


## The client library

On the user end, the Ouinet project provides a piece of software that user applications can use to access web resources through the Ouinet network, referred to as the *client library*. As the name suggests, it is implemented as a library to be used by end-user applications that make use of access to the web, rather than being an application in its own right. The detailed behavior of the client library is described in the [Client Library](#client-library) section.

In addition to the logic necessary to access web resources through the different systems that Ouinet provides for this purpose, the client library also contains the functionality required for the application to participate in the peer-to-peer components of the Ouinet network. Active participation in these peer-to-peer systems is not a requirement when using the client library, and indeed there exist applications for which active involvement in the network is unlikely to work well. The aim of this integration is to allow an application to enable contribution to the network where this is reasonably practical, and forego it otherwise.


## Support nodes

The Ouinet network relies on the existence of a substantial community of users that contribute to the peer-to-peer systems that underlie Ouinet. However, it is to be expected that many users of the Ouinet library will do so on low-powered devices for which a significant peer-to-peer contribution is not very practical.

To remedy this, users with access to well-connected devices can contribute to the resources of the Ouinet network by operating a *support node*. This is a server that runs a configuration of the client library, implemented as a standalone program, which does not serve any immediate applications for its operator, but instead is setup solely to contribute its resources to the Ouinet peer-to-peer systems. The presence of a few strategically-placed volunteer support nodes can greatly benefit the performance and reliability of the Ouinet network.


## End-user applications

The Ouinet project proper does not contain any end-user applications that make use of the Ouinet client library; the client library is designed as a building block for others to build upon, rather than as a tool that is useful for end users directly. But of course, the Ouinet project relies very much on the existence, quality, and useful application of such external projects.



# Content Access Systems

The Ouinet client library uses three distinct methods for getting access to a web resource. It can establish a connection through an injector server, through one of a variety of methods: it can fetch a resource from the distributed cache, if the resource is eligible for caching; or it can make a direct connection to the authoritative webserver serving the resource, and avoid any Ouinet-specific complications.

These access methods have different strengths and weaknesses, and will work well in different situations. In the worst case, the Ouinet client will try all of these methods, with different configurations, until it succeeds in fetching the resource. Trying different methods exhaustively is quite inefficient, however, both in terms of request latency and in bandwidth usage. Thus, where possible, the Ouinet client will try to estimate which method is likely to work well for different resources and conditions, and minimize inefficiency by trying likely options first.


## Direct origin access

The most straightforward way the Ouinet client can satisfy a resource request is to simply forward the request to the webserver responsible for serving that. Ideally, the Ouinet client could try this before doing anything else, and only move on to more indirect ways of fetching the same resource if this direct attempt fails.

In practice, this is not as straightforward as it first appears. Networks that block access to particular content do not typically provide a machine-readable signal that access has been blocked. If a network blocks access to a resource by making it impossible for the Ouinet client to establish a connection to the origin webserver, or by terminating such a connection prematurely, the Ouinet library can indeed register the failure and move on to other content access mechanisms. Many content-blocking networks, however, instead choose to serve a webpage explaining in human-readable text that access to this content has been blocked. When this happens, it is more difficult for the Ouinet client to notice that the request did not complete as desired; such error pages do not usually come with an obvious marker by which the Ouinet client can distinguish the error message from the desired resource, and certainly do not do so reliably.

For content that is served over HTTPS, the TLS layer can be used to distinguish between genuine responses and network-inserted error messages. A content-blocking network will not be able to provide a valid TLS certificate for the domain whose content it seeks to block, and this failure to establish a properly-certified TLS connection functions as a reliable signal that the direct origin access attempt has failed.

For content served without TLS, however, the Ouinet client has no such recourse, and indeed Ouinet cannot reliably recognize blocked pages when using direct origin access. As a consequence, direct origin access without HTTPS will ideally only be enabled in applications where the application or the user can provide the Ouinet client with feedback about success or failure of a direct origin access attempt. For example, a web browser application might have a button whereby a user can report a block page to the Ouinet client; an application using the Ouinet library to access its HTTP API can report a failure if the API response does not have the expected syntax. When this facility is used, the Ouinet client can then choose to avoid direct origin access for future requests, entirely or for a particular class of requests.


## Injector server connection

The Ouinet project operates a collection of *injector servers*, which function as a variant of an HTTP proxy server. The client library can satisfy content requests by establishing a connection to such an injector server, and forwarding content requests to it.

In addition to functioning as a proxy server, the other main responsibility of injector servers is to add ("inject") web content into the distributed cache. When an injector server serves a proxy request for a Ouinet client, a determination is made whether the assorted response is eligible for caching. If it is, the injector server then creates a signature that covers the response body, headers, and key metadata such as the resource URI and retrieval date. This signature serves as a certificate that the complete HTTP response has been retrieved from the authoritative origin server by a trusted injector server. The injector server then sends this signature to the requesting Ouinet client, along with the content response proper.

The combination of a response body, head, metadata, and injector signature for a particular web resource forms a *cache entry* used by the distributed cache system. Any party in the possession of a cache entry can share this entry with other peers; by verifying the signature that is part of a cache entry, a peer receiving such an entry can verify the legitimacy of the entry, as certified by the signatory injector server. After receiving a combination of a content response and a cache entry signature from an injector server, the requesting client can choose to start sharing the resulting cache entry in the distributed cache. The injector server may also choose to start sharing the cache entry in the distributed cache on its own, if it so chooses.

The Ouinet project contains several different mechanisms by which a client can establish a connection to an injector server. These mechanisms take the form of different protocols and tunneling systems that can carry a stream-oriented connection as a payload, over which standard protocols such as TLS and HTTP can be transmitted. Injector servers are configured to serve requests via multiple such mechanisms, allowing the Ouinet client to use whichever mechanism that does not trigger network blockages.


## Distributed cache lookups

The Ouinet network uses a peer-to-peer network that different parties can use to share among each other stored versions of cache-eligible resources. When an injector server serves a client request and determines the corresponding response to be eligible for caching, it can create a *cache entry* that can be used by clients to satisfy content requests, in the same way as would be used by a standard caching HTTP proxy server. Clients holding a copy of such a cache entry can use the peer-to-peer network to share it with other interested clients; and conversely, a Ouinet client trying to satisfy a content request can do so by contacting a peer that holds a cache entry for this resource, and requesting a peer-to-peer transfer of this cache entry. Together, this system forms the Ouinet *distributed cache*.

Cache entries can only be prepared for distribution in the distributed cache by the authority of an injector server. When an injector server decides that a particular resource response is suitable for use as a cache entry, it will create a cryptographic signature certifying this decision, as described in the previous section. Only cache entries containing such a signature can be shared in the distributed cache, and clients trying to fetch a resource from the distributed cache will verify this signature. In this way, the injector servers as a whole form the root of trust for all content exchanged in the distributed cache. By only sharing and using cache entries that have been certified by a trusted injector server, clients can be confident that the cache data matches the response that a trusted party has received from an authoritative origin server. In particular, this makes it impossible for an attacker to add forged cache entries to the distributed cache, ensuring the legitimacy of responses served from it.

The Ouinet client does need to verify that a cache entry received from the distributed cache is usable for the assorted content request. It needs to verify, for example, that the cache entry is not expired, and that the response is applicable to the constraints set in the request headers. The details of this procedure are described in the [Distributed Cache](#distributed-cache) section.



# Distributed Cache

## Introduction

The Ouinet network uses a *distributed cache* as one of the methods of getting requested web content to users. If a web resource is fetched from the authoritative origin webserver on behalf of a user, in many cases the fetched resource can be used to satisfy future requests by different users trying to access the same content. If a user's device holds a copy of such suitable content, the Ouinet client can use peer-to-peer communication to transmit copies of this content to other users interested in the content; and conversely, if a user wants to access a certain web resource, the Ouinet client can use peer-to-peer communication to request a copy of the resource from a peer. In situations where user access to the centralized Ouinet injector infrastructure is unavailable, this technique provides a limited but useful alternative form of access to web content. If access to the injector infrastructure is unreliable or limited, the distributed cache can be used to satisfy resource requests wherever possible, allowing the Ouinet client to utilize the injectors only for those requests for which no alternative is available, reducing the load on the unreliable injectors and improving performance.

The caching of web resources is a standard functionality in the HTTP protocol. The HTTP protocol provides faculties by which an origin server can provide detailed instructions describing which resources are eligible for caching, which ones are not, the duration during which a resource can be cached, and requirements and limitations when caching the resource. HTTP clients implementing this system can satisfy HTTP requests by substituting an HTTP response stored in the cache, subject to the restrictions declared by the origin server, saving on network traffic and improving performance. HTTP client software such as web browsers commonly implement a cache for this purpose, which typically plays a major part in the performance characteristics of such software. The HTTP standard also describes caching HTTP proxies, which act as an performance-improving intermediary to a group of users by using a shared cache for all of them.

The Ouinet distributed cache is a variant implementation of such an HTTP cache, in which each Ouinet user has access to the combined cached resources of all Ouinet users worldwide. Each Ouinet client stores cached copies of resources they have recently accessed in the storage of their own device, and will use peer-to-peer communications to transfer these cached resources to other Ouinet clients that request access to them. From the viewpoint of a particular Ouinet user, the combined caches held by each Ouinet user worldwide function as a distributed filesystem containing more cached content than any one user device can realistically store.

Traditional HTTP caches are used in such a way that only a single party writes to, and reads from, the cache. The Ouinet distributed cache, on the other hand, forms a distributed filesystem that many different users can store resources in, and fetch resources from. This architectural difference comes with a number of complications that the Ouinet system needs to account for. With large numbers of people being able to participate in the distributed cache, the Ouinet software cannot assume that all these participants are necessarily trustworthy. When the Ouinet client requests a cached resource from some other Ouinet user using the peer-to-peer system, or sends some other Ouinet user a copy of a cached resource for their benefit, the Ouinet client must account for the possibility that their peer may have malicious intent. To accommodate this concern, the Ouinet distributed cache uses several systems to make it possible to cooperate on resource caching with untrusted peers.

When an HTTP client wishes to respond to an HTTP request by substituting a cached response, it needs to ensure that the stored cached response is in fact a legitimate response sent by the responsible origin server to the associated request. In a traditional HTTP cache operated by a single party, this is a trivial requirement, for the cache software will only store a cached response after receiving it from the responsible origin server, which means the cache storage serves as a trusted repository of cached content. In the Ouinet distributed cache, on the other hand, cached resources may be supplied by untrusted peers, and there is no obvious way in which the receiving party can verify that this response is a legitimate one; a malicious peer could easily create a forged response, add it to its local cache storage, and send it to its peers. This behavior is a threat that the Ouinet client needs to be able to guard against.

To avoid this problem, the Ouinet system makes use of trusted injector servers, charged with the authority of creating resource cache entries whose legitimacy can be verified by Ouinet clients. When these injector servers fetch an HTTP resource on behalf of a user, they determine if the resulting response is eligible for caching; if it is, they will then create a cryptographic signature covering the response, which enables the peer-to-peer distribution of the cached resource in the distributed cache. By verifying this signature, clients can confirm that a cached resource has been deemed legitimate by a trusted injector server.

A different security concern when using an HTTP cache shared between large numbers of people lies in the confidentiality of privacy-sensitive data communicated using web resources. Some HTTP responses contain private information intended only for the recipient, and nobody else; to avoid compromising confidentiality, responses with this characteristic must not be shared using the distributed cache. The Ouinet system therefore needs to be able to recognize confidential responses, and mark them as ineligible for caching.

The HTTP protocol contains functionality by which origin servers can specify resources that are ineligible for caching, or ineligible for public caching, on the grounds of confidentiality, which in theory should imply that recognizing confidential responses should be a simple matter. Unfortunately, origin servers in practice do not always adhere to this protocol very accurately. It is reasonably common for origin servers to serve confidential resources while failing to mark them as such, in which case it is critical that the Ouinet system is able to recognize the confidentiality of the resource by some other means. Much more common still is the reverse situation, in which a non-confidential resource is marked as ineligible for caching on confidentiality grounds by the origin server, typically for commercial reasons. If the Ouinet system were to accept these judgements uncritically, the amount of resources eligible for caching would be sharply limited, reducing the utility of the distributed cache considerably. To avoid both problems, the Ouinet system uses a heuristic analysis to recognize cases where the origin-supplied cache-eligibility judgement is misleading.

The remainder of this section describes the details of the operation of the Ouinet distributed cache system. It details the exact data stored in the cache, and its interpretation; the system used for signing and verification of cached resources; and finally, it describes the methods and protocols by which different actors in the Ouinet network may exchange cached resources with each other.


## Cache structure

The Ouinet distributed cache conceptually consists of a repository of cached web resources. Each such cached resource takes the form of a record referred to as a *cache entry*. A cache entry represents a single HTTP response suitable for using as a cached reply, along with assorted metadata that makes it possible to verify the legitimacy of the cached response, check the response for expiracy or being superceded, and assess its usability. Cache entries are created by the Ouinet injector servers, transmitted from injector servers to Ouinet clients, stored on client devices, and shared between different clients using peer-to-peer systems.

A cache entry is a data structure consisting of the resource URI, the HTTP response headers, the HTTP response body, additional metadata added by the Ouinet injector, and a cryptographic signature asserting the legitimacy of the cache entry.

Clients that participate in the distributed cache store a collection of such cache entries on their device's local storage, and can get access to many more cache entries using the peer-to-peer network. When satisfying an HTTP request, they can search the distributed cache for any cache entries with an URI matching the one in the HTTP request, checking them for validity, and substituting the cached resource as an HTTP response.

### Cache entry construction

Cache entries are created by the Ouinet injector servers. Injector servers can create a cache entry by requesting an HTTP resource on behalf of a user, checking the response for cache eligibility, adding necessary metadata, and signing the resulting package.

Cache entries are identified by their resource URI; when a Ouinet client seeks to resolve an HTTP request from the distributed cache, it can use any cache entry whose resource URI matches the URI in the HTTP request. For this behavior to work as expected without causing problems, the injector servers should avoid sending multiple HTTP requests for a particular resource URI that are interpreted by the responsible origin server as having different request semantics; this can happen, for example, if the origin server chooses to vary its response based on the user's user agent settings, which are communicated as part of the HTTP request using HTTP headers. In this scenario, the distributed cache would likely end up storing multiple semantically different responses for this resource. The Ouinet client would not be able to distinguish between the competing cache entries, on account of them using the same resource URI, causing confusion and unpredictable behavior.

To avoid this problem, the injector servers will not create cache entries based on HTTP responses received after forwarding arbitrary HTTP requests. Instead, when attemping to create a cache entry, the injector servers will only use a single predictable HTTP request for each resource URI, which is allowed to vary only on a carefully selected list of characteristics known not to affect the request semantics. For similar reasons, when creating a cache entry, the injector servers will remove all metadata from the HTTP response whose semantics are likely to change with different requests for the same resource. Together, these two procedures are referred to as *resource canonicalization*.

Separately from the above, the distributed cache mechanism also needs to check whether a particular HTTP response is eligible for storing in the cache at all. Many HTTP resources should not be stored in any cache, because their content changes frequently and unpredictably, or because their content is personalized specifically for the user requesting the resource; this eligibility is typically specified in HTTP response headers. In addition, clients sometimes wish to send an HTTP request without the limitations enforced by the resource canonicalization system; in such cases, the resource can neither be retrieved from the distributed cache, nor stored in it.

The process for constructing a cache entry and storing it in the distributed cache is a procedure that incorporates all these systems. It consists of the following steps:

* The Ouinet client wishes to perform an HTTP request.
* The Ouinet client checks whether the request is eligible for caching. If it is not, the distributed cache subsystem is not used.
* The Ouinet client contacts an injector server, and asks it to create a cache entry corresponding to the HTTP request.
* The injector server canonicalizes the HTTP request.
* The injector server sends the canonicalized request to the responsible origin server, and awaits a response.
* The injector server canonicalizes the HTTP response.
* The injector server adds metadata to the HTTP response in the form of additional HTTP headers, describing the characteristics of the cache entry.
* The injector server creates a cryptographic signature for the cache entry.
* The injector server sends the modified HTTP response to the client, along with the signature.
* The Ouinet client checks whether the response is eligible for caching. If it is, it stores the combination of the HTTP response and the signature to the distributed cache.
* The Ouinet client resolves the HTTP request, whether or not it was also stored in the distributed cache.

The exact communication between the Ouinet client and the injector server, as well as the details of the cryptographic signatures used in cache entry construction, are described in later sections. Other details are described below.

#### Resource canonicalization

When sending an HTTP request to an origin server for the purpose of creating a cache entry, the Ouinet injector creates a minimal canonical HTTP request based on the resource URI as well as a small number of request headers derived from the HTTP request sent by the Ouinet client. The *canonical request* also contains neutral generic values for certain headers that many origin servers expect to be present.

This canonical HTTP request takes the form of an HTTP/1.1 request, with a request target and `Host:` header derived from the resource URI in the standard way. The canonical request also contains the following headers:

* `Accept: */*`
* `Accept-Encoding: `
* `DNT: 1`
* `Upgrade-Insecure-Requests: 1`
* `User-Agent: Mozilla/5.0 (Windows NT 6.1; rv:60.0) Gecko/20100101 Firefox/60.0`
* `Origin:` whatever value is present in the client request, if any, or absent otherwise
* `From:` whatever value is present in the client request, if any, or absent otherwise

This canonical request is sent to the origin server, and the accompanying response used to create a cache entry.

After receiving a reply to this canonical request from the origin server, the injector server removes from the response all HTTP headers that are likely to describe details of the individual request that was performed, rather than the resource that was requested. It also removes headers that describe characteristics that do not apply to a cached version of the resource. This modified form of the HTTP response is then known as the *canonical response*.

The canonical response is created by removing from the origin response all headers, except for those that are explicitly allowed. The canonical response contains the following headers, insofar as they are present in the origin response:

* `Server`
* `Retry-After`
* `Content-Type`
* `Content-Encoding`
* `Content-Language`
* `Digest`
* `Accept-Ranges`
* `ETag`
* `Age`
* `Date`
* `Expires`
* `Via`
* `Vary`
* `Location`
* `Cache-Control`
* `Warning`
* `Last-Modified`
* `Access-Control-Allow-Origin`
* `Access-Control-Allow-Credentials`
* `Access-Control-Allow-Methods`
* `Access-Control-Allow-Headers`
* `Access-Control-Max-Age`
* `Access-Control-Expose-Headers`

All headers in the origin response that are not on this list are removed from the canonical response.

#### Added metadata

When a Ouinet injector has created a canonical response for a newly constructed cache entry, it then adds a series of headers that describe the properties of the cache entry itself. These headers aid a Ouinet client receiving the cache entry to interpret the cache entry correctly. These headers are stored in the cache entry as part of the HTTP response headers.

The Ouinet injector adds the following headers to the cache entry:

* `X-Ouinet-Version`: This describes the version of the Ouinet distributed cache storage format. This document describes the distributed cache storage format version **4**.
* `X-Ouinet-URI`: Contains the URI of the resource described by this cache entry.
* `X-Ouinet-Injection`: This describes a unique ID assigned to this cache entry, allowing a receiver to refer unambiguously to this specific cache entry, as well as the time at which the cache entry was created. Encoded as `X-Ouinet-Injection: id=<string>,ts=<timestamp>`, where `<string>` is a string containing only alphanumeric characters, dashes, and underscores; and `<timestamp>` is an integer value, representing a timestamp expressed as the number of seconds since 1970-01-01 00:00:00 UTC.

The Ouinet injector furthermore adds headers related to the cryptographic signature used to verify the legitimacy of the cache entry. This is described in more detail in the [Signatures](#signatures) section.

#### Cache eligibility

The distributed cache system makes a determination, for each resource request, whether that resource is eligible for caching. This process takes place in two parts. When the client is preparing to send an HTTP request, it first determines whether the request is one that can, in principle, be cached; if this is not the case, the Ouinet client does not use the distributed cache system at all when satisfying this request. If the request is eligible for caching, the request can be sent to an injector server, which --all going well-- will reply with a cache entry that can be stored in the distributed cache. The client then makes a second determination whether the response is also eligible for caching. If it is not, the resource request is completed successfully, but the cache entry is not stored.

The Ouinet client currently considers an HTTP request to be eligible for caching if it uses the GET HTTP access method, and moreover the resource URI is not on a configurable blacklist of resources that are never eligible. This is certainly an overestimate for general browsing; for one example, there are many web resources that can only be accessed after authenticating using HTTP authentication, or only after authenticating using some cookie-based authentication scheme. This scheme therefore relies on careful configuration of the resource blacklist for it to work well in practice. Improving this heuristic remains a fertile area for future improvement.

To determine whether an HTTP response is eligible for storing in the distributed cache, Ouinet uses a variant of the procedure described in [RFC 7234, section 3](https://tools.ietf.org/html/rfc7234#section-3). This RFC describes a procedure determining whether an HTTP response is allowed to be stored in a cache, based on the characteristics of the cache, expiracy information communicated in the response headers, and the `Cache-Control` header. The Ouinet distributed cache follows this procedure as written, with two major exceptions:

* The Ouinet distributed cache will only store HTTP responses with an status code of 200 (OK), 301 (Moved Permanently), 302 (Found), or 307 (Temporary Redirect). The Ouinet project does not wish to store error status pages in the distributed cache.
* If the HTTP response contains the `Cache-Control: private` clause, the Ouinet client will use a heuristic analysis to verify that this clause is warranted.

Many origin servers will declare a `Cache-Control: private` clause on resources that are not really private in reality, but which the origin server wishes to personalize in a non-confidential way for each request. For such resources, the Ouinet client ideally wishes to store the cache entry as if the `Cache-Control: private` clause was absent, but avoid satisfying requests for this resource using the distributed cache unless no other methods for accessing the resource are available. To determine whether the `Cache-Control: private` clause is used with good reason, Ouinet uses the following procedure:

* If the HTTP request uses an HTTP method other than GET, the `Cache-Control: private` clause is warranted.
* If the resource URI contains a query string (that is, if it contains a question mark character), the `Cache-Control: private` clause is warranted.
* If the HTTP request contains any header fields that might contain confidential information, the `Cache-Control: private` clause is warranted.
* If neither of the above applies, the `Cache-Control: private` is unwarranted, and the cache entry is eligible for storage in the distributed cache.

For the purposes of this procedure, the following HTTP request headers are considered to never contain confidential information:

* `Host`
* `User-Agent`
* `Cache-Control`
* `Accept`
* `Accept-Language`
* `Accept-Encoding`
* `From`
* `Origin`
* `Keep-Alive`
* `Connection`
* `Referer`
* `Proxy-Connection`
* `X-Requested-With`
* `Upgrade-Insecure-Requests`
* `DNT`

Any headers not on this list are considered to potentially contain confidential information. If any header not on this list is present in the HTTP request, the `Cache-Control: private` clause is considered to be warranted.

### Validity checking

When a Ouinet client wishes to use a cache entry stored in the distributed cache to satisfy a resource request, it must first verify that the cache entry has not expired, is unusable for some other reason, or needs revalidation from the origin server. To make this determination, the Ouinet client largely follows the procedure described in [RFC 7234, section 4](https://tools.ietf.org/html/rfc7234#section-4). It deviates from this procedure on two important points, however.

The procedure described in this RFC allows a client to use a cache entry that is expired, in certain cases where the client is unable to connect to the responsible origin server to request an up-to-date resource. However, this behavior is bound to strict conditions, and the great majority of origin servers specify `Cache-Control` clauses that disallow this behavior, making this mechanism of sharply limited practical value.

Because the Ouinet project aims to provide some limited form of access to web resources even in cases where no access to the responsible origin server can be arranged, the Ouinet client is willing to use expired cache entries in cases where the procedure described in the RFC specifically disallows this behavior. When the Ouinet client cannot establish contact to the responsible origin server in any way --either directly, or by using an injector server as an intermediary-- and when all cache entries for the resource the client has access to are expired, the client will use this cache entry to satisfy the resource request, despite any `Cache-Control` clauses that would disallow this. This ensures that the Ouinet client will provide some limited access to the resource, if this is at all possible.

For similar reasons, the Ouinet client is willing to use cache entries that have the `Cache-Control: private` clause set, if no other options are available. As described in the previous section, cache entries with this clause set should only be used by the client as an option of last resort. The Ouinet client treats such cache entries equivalently to cache entries that have expired.

### Signatures

When an injector server creates a cache entry for distribution, it also creates a cryptographic signature of this cache entry using a private key specific to the injector server, or group of injector servers to which the injector server belongs. By verifying this signature, recipients of the cache entry can confirm that the cache entry was created and retrieved from an origin server by one of the injector servers, which hold a trusted position in the Ouinet network. This is important particularly when retrieving the cache entry via peer-to-peer communication with another client, as the presence of the signature makes it impossible for a malicious client to forge a cache entry.

The cache entry signature is computed as a signature over the cache entry as created by the injector server, consisting of the response content, the canonicalized response headers, and the added metadata described in the [Cache entry construction](#cache-entry-construction) section. A Ouinet client receiving a copy of a cache entry from another client using the peer-to-peer cache system, or from some external source, should verify this signature before using the cache entry to satisfy requests, and reject the cache entry if the signature is invalid. If a Ouinet client receives a cache entry directly from a Ouinet injector server, it may choose to skip this verification, as long as the client has previously confirmed the identity of the injector server to which it is connected.

The requirement for a Ouinet client to verify the cache entry signature before using the cache entry --and, in particular, to avoid using any of the data in the cache entry before having performed this verification-- has unfortunate consequences when a client tries to fetch a cache entry that is large enough that the download process takes significant amounts of time. When a client wishes to download a cached copy of a large resource, such as a large media file, the client needs to download the entire resource, verify the signature, and only then start doing something useful with the resource. Many web applications contain functionality to start making use of a resource while the process to download that resource is still ongoing; examples include the streaming of audio and video content, and the incremental loading of large images. The signature verification process as described above would greatly limit this functionality, for the application could only start using such content after the signature has been verified, which can only happen after the entire resource download has completed.

To mitigate this limitation, the distributed cache can make use of an alternative signature scheme in which the injector server can produce signatures for individual fragments of a resource. When using this scheme, the injector server will split up a resource into a series of fragments, and generate a separate signature for each fragment, including enough metadata that a client can verify that these fragments form a contiguous resource. A client receiving a cache entry using such fragmented signatures can verify the signature of each fragment as it comes in, and incrementally start loading the content present in an individual fragment once its signature has been verified. A client can also deliberately fetch selected fragments of a cache entry and verify their legitimacy, which is useful if the client wishes to access only parts of a larger resource.

#### Signature computation

To compute a signature for a complete cache entry, the Ouinet distributed cache uses a version of the format specified in the [Signing HTTP Messages](https://datatracker.ietf.org/doc/html/draft-cavage-http-signatures-12) draft. This protocol specifies a way of computing signatures on HTTP messages that are robust to the transformations that frequently happen to HTTP messages in transit. This robustness makes it possible to send a signed HTTP message as part of an HTTP session, performing transit-related manipulations such as the addition of a `Connection: keep-alive` header where necessary, without causing the signature to become invalid.

Signing HTTP Messages signatures consist of a signature over a selected subset of the headers in an HTTP message; the distributed cache uses this system by including all headers in the signature that are part of the cache entry, as constructed per the process described in the [Cache entry construction](#cache-entry-construction) section. By design, these headers do not include any headers that might vary with different ways in which a cache entry might be communicated between peers, such as the `Connection`, `Transfer-Encoding`, and `Content-Length` headers.

The signature used by the distributed cache must cover certain information that is not stored in HTTP headers in the strictest sense. Most obviously, this includes the body of the HTTP response. To ensure that the body is covered by the cache entry signature, the injector server adds a `Digest` header to the cache entry, containing a cryptographic hash of the response body. This `Digest` header is then included in the headers covered by the signature.

In addition, Signing HTTP Messages signatures can include pieces of information regarding an HTTP message that are not stored in any HTTP headers, by including key/value pairs in the signature calculation as if they were HTTP headers. The distributed cache uses these *pseudo-headers* to describe the HTTP status code, which is not a header, as well as the time at which the signature was created.

To compute the cache entry signature, the injector server uses the following procedure:

* Construct a cache entry as described in the [Cache entry construction](#cache-entry-construction) section, containing a canonicalized HTTP response with added headers containing further metadata;
* Add a `Digest` header, containing a cryptographic hash of the response body;
* Add an `X-Ouinet-Data-Size` header, containing the length of the response body;
* Add a `(created)` pseudo-header, containing the time at which the signature was created, expressed as the number of seconds since 1970-01-01 00:00:00 UTC;
* Add a `(response-status)` pseudo-header, containing the HTTP status code such as `200` or `304`;
* Compute a Signing HTTP Messages signature over all headers in the cache entry, as well as the pseudo-headers above.

The signature for a complete cache entry is generally added to an HTTP response message as an additional header. The variations of this protocol are described in detail in the [Distribution](#distribution) section.

#### Stream signatures

In addition to using a signature to cover each complete cache entry, the Ouinet system implements a signature scheme in which a cache entry is split into multiple fragments, and a signature is computed for each separate fragment. When this scheme is used to sign a cache entry for a large resource, a Ouinet client can fetch only the parts of a large resource it is interested in, and verify their legitimacy without first having to download the resource in its entirety.

The streaming of media resources is one major application of this signature scheme, and the design of the scheme is motivated in large part by the wish to support that application. When streaming a large resource using the distributed cache, the Ouinet client can fetch each consecutive fragment of the resource along with the signature for that fragment, verify the signature, and start using the resource data present in the fragment without first having to download the entire resource.

To support this application, the streaming-signatures scheme splits the response body of the cache entry into a sequence of blocks, each consisting of the same number of bytes. It then constructs a *Merkle DAG* over this block sequence, consisting of a signature for each block that describes the content of the block, as well as a reference to the previous signed block in the sequence. This structure makes it possible to not only verify the legitimacy of the individual blocks, but also to confirm that the sequence of blocks forms a coherent whole.

To construct a signature stream, the injector server computes the following components:

* `block-size`: The size of each data block, measured as a number of bytes.
* `injection-id`: The unique ID of the cache entry, described in the `X-Ouinet-Injection` response header.
* `header-signature`: A signature over the headers of the cache entry. This is computed the same way as the complete cache entry signature described in the previous section, except that the `Digest` and `X-Ouinet-Data-Size` headers are absent. This absence makes it possible for the injector server to compute the `header-signature` before fetching the complete response body.
* `block(i)`: The sequence of bytes running from byte `i * block-size` in the response body, to byte `(i + 1) * block-size` in the response body. The last block in this sequence may have fewer than `block-size` bytes.
* `hash(0)`: The cryptographic hash of `block(0)`.
* `hash(i)`, for `i > 0`: The cryptographic hash of `hash(i - 1) ++ block(i)`.
* `block-signature(i)`: The signature of the bytestring `injection-id ++ '\0' ++ (i * block-size) ++ '\0' ++ hash(i)`.
* `data-size`: The size of the complete response body, measured as a number of bytes.
* `full-signature`: The signature of the complete cache entry, as described in the previous section.

In the above, the `++` operator denotes concatenation.

The complete signature stream consists of the `block-size`, `injection-id`, `header-signature`, `block-signature`, `data-size`, and `full-signature` values. The Ouinet client can verify this signature stream by first verifying the `header-signature` after having received the response header, and then verifying each `block-signature(i)` after having received `block(i)`, as well the `block-size` and `injection-id` which the client needs to receive before verifying any block signatures. After receiving the final block, the `data-size` value, and the `full-signature`, the client can verify that the sequence of blocks it has received together form the complete response body.

For a client to be able to perform the streaming verification of a cache entry as above, the party serving the stream-signed cache entry needs to transmit the different components of a signature stream in a careful order. Ideally, it should send the `block-size`, `injection-id`, and `header-signature` fields before sending any block data; proceed by alternating `block(i)` and `block-signature(i)` fields; and finally send the `data-size` and `full-signature` fields. Depending on what parts of a cache entry are available, however, other streaming protocols are also possible. This is described in more detail in the [Distribution](#distribution) section.

#### Cryptographic primitives

The descriptions in this section, as well as the signature format described in the [Signing HTTP Messages](https://datatracker.ietf.org/doc/html/draft-cavage-http-signatures-12) draft, both make use of a cryptographic hash function and a cryptographic signature scheme. These systems are not sensitive to the exact choice of these cryptographic primitives, and they can be selected and replaced based on improvements in cryptography.

The current version of the Ouinet distributed cache uses the following implementations for these primitives:

* The cryptographic hash of the complete response body stored in the `Digest` header uses **SHA-256**.
* The cryptographic hash used to construct stream signatures uses **SHA-512**.
* The cryptographic signature used to compute `block-signature(i)` uses **Ed25519**.
* The Signing HTTP Messages signature uses the **hs2019** signature scheme using the **SHA-512** hash function and **Ed25519** signature scheme.

A cache entry signed using implementations of these primitives different from the choices above is likely to be rejected by the current implementation of the Ouinet client. Each of these choices is likely to change in some future version of the Ouinet project.

#### Examples

An injector server using Ed25519 private key `KEY` might construct the following as-yet unsigned cache entry:

```
HTTP/1.1 200 OK
X-Ouinet-Version: 4
X-Ouinet-URI: https://example.com/hello
X-Ouinet-Injection: id=qwertyuiop-12345,ts=1584748800
Date: Sat, 21 Mar 2020 00:00:00 GMT
Content-Type: text/plain

Hello world!
```

The injector server would compute a message digest and content length of the `Hello world!` message body, and add the following headers to the cache entry:

```
Digest: SHA-256=wFNeS+K3n/2TKRMFQ2v4iTFOSj+uwF7P/Lt98xrZ5Ro=
X-Ouinet-Data-Size: 12
```

The injector server would create the following complete cache entry signature:

```
keyId="ed25519=<key>",algorithm="hs2019",created=1584748800, headers="(response-status) (created) x-Ouinet-version x-Ouinet-uri x-Ouinet-injection date content-type digest x-Ouinet-data-size",signature="<signature-base64>"
```

In this signature, `<key>` stands for the public key associated with the `KEY` private key, and `<signature-base64>` is the base64 encoding of the Ed25519 signature of the following string:

```
(response-status): 200
(created): 1584748800
x-Ouinet-version: 4
x-Ouinet-uri: https://example.com/hello
x-Ouinet-injection: id=qwertyuiop-12345,ts=1584748800
date: Sat, 21 Mar 2020 00:00:00 GMT
content-type: text/plain
digest: SHA-256=wFNeS+K3n/2TKRMFQ2v4iTFOSj+uwF7P/Lt98xrZ5Ro=
x-Ouinet-data-size: 12
```

Lines in this string are separated by newline `\n` characters. The string does not begin with or end in a newline character.

The injector server might choose not to create a signature stream for this cache entry, on account of its small size. If for the sake of this example the injector would choose to create a signature stream anyway, using a block size of 5 bytes, it would compute the following values:

* `block_size`: `5`
* `injection-id`: `qwertyuiop-12345`
* `header-signature`: `keyId="ed25519=<key>",algorithm="hs2019",created=1584748800, headers="(response-status) (created) x-Ouinet-version x-Ouinet-uri x-Ouinet-injection date content-type",signature="<header-signature-base64>"`
* `block(0)`: `Hello`
* `block(1)`: ` worl`
* `block(2)`: `d!`
* `hash(0)`: sha512(`Hello`) = bytes(`3615f80c9d293ed7402687f94b22d58e529b8cc7916f8fac7fddf7fbd5af4cf777d3d795a7a00a16bf7e7f3fb9561ee9baae480da9fe7a18769e71886b03f315`)
* `hash(1)`: sha512(bytes(`3615...f315`) ++ ` worl`) = bytes(`9f3cda2fa89c046c51a5635693a5c8e26a8a19fa0675c785bb1b318910db4ea49ee281e264d1f1461533f48e81d81eb53dd6addd21ad9e42ec46182b93c37d5e`)
* `hash(2)`: sha512(bytes(`9f3c...7d5e`) ++ `d!`) = bytes(`a5424a9d7faf90c636fe14686de2b44e490d36e94113aec176d68cd7e86b59e4d8df7712e62f967c27a54e7216f812b6371d456e94529b46423ac1acd797572c`)
* `block-signature(0)` = base64(signature(`qwertyuiop-12345` `\0` `0` `\0` bytes(`3615...f315`)))
* `block-signature(1)` = base64(signature(`qwertyuiop-12345` `\0` `5` `\0` bytes(`9f3c...7d5e`)))
* `block-signature(2)` = base64(signature(`qwertyuiop-12345` `\0` `10` `\0` bytes(`a542...572c`)))
* `data-size`: 12
* `full-signature`: The complete cache entry signature described above

In the computation of `header-signature` in the above, `<key>` stands for the public key associated with the `KEY` private key, and `<header-signature-base64>` is the base64 encoding of the Ed25519 signature of the following string:

```
(response-status): 200
(created): 1584748800
x-Ouinet-version: 4
x-Ouinet-uri: https://example.com/hello
x-Ouinet-injection: id=qwertyuiop-12345,ts=1584748800
date: Sat, 21 Mar 2020 00:00:00 GMT
content-type: text/plain
```

Of these values, the combination of `block-size`, `injection-id`, `header-signature`, `block-signature(0..2)`, `data-size`, and `full-signature` makes up the complete signature scheme. The way in which these values are communicated to the recipient of this cache entry is described in the [Distribution](#distribution) section.


## Distribution

The Ouinet distributed cache functions as a global distributed repository of cache entries, which different participants in the distributed cache can store in their device's local storage, as well as sharing them between each other. Besides implementing the logic of handling, interpreting, validating, and otherwise making use of such cache entries, as described in detail in the previous section, the Ouinet distributed cache also encompasses a collection of mechanisms by which different participants in the distributed cache can transfer cache entries between themselves.

The primary methods by which cache entries are transmitted between different distributed cache participants are requests from a Ouinet client to an injector server, and the peer-to-peer exchange of cache entries between clients. Clients can send a request to an injector server when they wish to access a resource that is not yet present in the distributed cache; the injector server can then fetch the resource, construct a cache entry based on this fetched resource, and transmit this cache entry to the requesting client. When the distributed cache already contains a cache entry for a particular resource, a client wishing to access that resource can send a peer-to-peer request to some other client that is storing that cache entry in their device storage, and access the resource that way.

Both requests sent by a client to an injector, and the peer-to-peer exchange of cache entries between clients, are systems that use a network connection to transfer cache entries towards a client when that client needs the cached resource. In addition to those methods, the Ouinet distributed cache can also support applications where cache entries are distributed in a more ad-hoc batched manner. These methods play a limited role in the most common applications of the Ouinet system, but have an important supportive function in certain more specialized applications.

### Injector-to-client cache entry exchange

The Ouinet client can send a request to an injector server for the injector to fetch a web resource, and construct a cache entry based on the response that the requesting client can then use and share in the distributed cache. An injector server serves this request by performing an HTTP request, verifying that the response is eligible for caching, constructing a cache entry, signing it, and sending the signed cache entry back to the requesting client.

To perform this procedure, a Ouinet client first establishes a connection to an injector server. The Ouinet system supports a variety of ways in which a client can establish a connection with an injector server; the different ways in which this connection can be established are described in the [Injector Servers](#injector-servers) section. These connections all make use of the TLS protocol, by which the client can verify that it is connected to a legitimate injector server using a secure channel.

Once the Ouinet client has established a connection to an injector server, the client can send a request for the injector to create a cache entry. This request is sent as a standard HTTP proxy request, along with an additional HTTP header signifying the intent to create a cache entry. This header distinguishes a cache-entry-creation request from other functionality offered by injector servers, as described in the [Injector Servers](#injector-servers) section.

When the injector server has performed the requested HTTP transaction and created a cache entry, it can then send the cache entry to the requesting client as a standard HTTP response. Because cache entries have the form of an HTTP response object combined with some assorted metadata, the injector server can simply send the cache entry as an HTTP response, and add the metadata in the form of additional HTTP headers.

To transport a cache entry as an HTTP response, the injector server may need to add some additional headers that are not part of the cache entry as such, but which are instead used to coordinate the transport process, such as the `Content-Length`, `Transfer-Encoding`, and `Connection: close` headers. Because the cache entry signature specifies exactly which headers are part of the cache entry proper, the receiving client knows exactly which headers are part of the cache control object, and which can be discarded after completion of the HTTP transaction.

The injector server can reply to a cache entry request with three different types of responses: a cache entry with a signature stream, a cache entry with only a signature that covers the complete entry, and a response indicating that no cache entry was created. The injector server uses different response formats for each.

#### Signature streams

When transmitting a cache entry signed using a signature stream, the injector server needs to communicate the `block-size`, `injection-id`, `header-signature`, `block-signature(i)`, `data-size`, and `full-signature` signature components to the client. It also needs to add the `Digest` and `X-Ouinet-Data-Size` headers to the cache entry.

Several of these pieces of information cannot be stored as HTTP headers in the straightforward way without undesirable side effects. The `full-signature`, `Digest`, and often the `data-size` fields are information the injector server only knows after having received the entire resource from the origin server; the different `block-signature(i)` values each become computable as the resource transfer from the origin server progresses. If the injector server would communicate this information as HTTP headers, which in the HTTP protocol are sent *before* the response body, the injector server would have to finish the download of the entire resource before beginning to send the cache entry response body to the requesting client. The resulting performance limitations would greatly limit the useful value of signature streams for streaming media content. It would be much preferable for the injector server to send fragments of the response body and elements of the signature stream as each becomes available, enabling the client to fetch and verify the cache entry in streaming form, without any part of the system having to wait for a large download to finish.

To support this behavior, the injector server communicates cache entries with signature streams to the client using the chunked transfer-encoding, storing signature stream fields in HTTP trailers and chunk extensions. This usage of HTTP makes it possible to communicate parts of the metadata in advance of the response body using headers, parts after the response body using trailers, and parts of the metadata interleaved with the response body using chunk extensions.

The chunk extension syntax of HTTP makes it possible for an HTTP stream to contain a set of key-value pairs attached to each response body chunk. This can be used to communicate metadata pertaining to the body chunk to which the key-value pairs are attached, or it can be used to communicate metadata pertaining to the response body as a whole, without the key-value pairs having any direct association with the body chunk to which they are attached. The distributed cache uses chunk extensions in the latter way, attaching a key-value pair for each `block-signature` field at the earliest opportunity.

When transmitting a cache entry using a signature stream, the injector server adds the following pieces of metadata to the HTTP response to communicate the signature stream fields:

* A `Digest` trailer containing the full message digest. This digest can only be computed after the injector has received the entire response body, and therefore is stored in a trailer.
* An `X-Ouinet-Data-Size` header, or trailer, containing the `data-size` field. This field can have the form of a header or a trailer, depending on the time at which the injector first learns this value.
* An `X-Ouinet-Sig0` header containing the `header-signature`;
* An `X-Ouinet-BSigs` header, containing the `block-size` as well as the cryptographic parameters used to compute the `block-signature` values. Encoded as `X-Ouinet-BSigs: keyId="<key>",algorithm="<algorithm>",size=<block-size>`, where `<key>` is the public key used to sign the block signatures; `<algorithm>` is the algorithm used to sign the block signatures; and `<block-size>` is the `block-size` as an integer value.
* An `X-Ouinet-Sig1` trailer containing the `full-signature`;
* A series of `ouisig=<signature>` chunk extensions, containing the different `block-signature(i)` values. These chunk extensions all use `ouisig` as a name, and are sent in increasing order; that is, the `i`th `ouisig` chunk extension contains `block-signature(i)`. `<signature>` is encoded as the base64 encoding of the block signature.

A client receiving a cache entry containing a signature stream can recognize this response type by the presence of the `X-Ouinet-BSigs` header, and handle it accordingly. A client that wishes not to implement streaming for this particular resource can choose to ignore the metadata specific to signature streams, and make use only of the `Digest`, `X-Ouinet-Data-Size`, and `X-Ouinet-Sig1` headers, and treat the response as if it were signed only using a complete cache entry signature instead.

#### Complete cache entry signatures

A cache entry that is signed only with a complete cache entry signature only contains the `Digest`, `X-Ouinet-Data-Size`, and `full-signature` signature fields that need to be communicated to the client by the injector server. The Ouinet injector communicates this using a simplified form of the signature streams format, in which only the `Digest`, `X-Ouinet-Data-Size`, and `X-Ouinet-Sig1` headers are present. Each of those headers may take the form of either a header or a trailer.

A client receiving such a cache entry can recognize this response type by the presence of the `X-Ouinet-Sig1` header, combined with the absence of the `X-Ouinet-BSigs` header. This combination indicates that the cache entry can only be verified in its complete form, and streaming verification is not possible.

#### Error responses

When an injector server is requested to create a cache entry for a resource, and such a cache entry cannot be created -- because the resource is not eligible for caching, indicates an error response, or perhaps because the resource could not be retrieved at all -- the injector server sends an HTTP response to the client that does not contain any of the signature headers described above. When a client receives a response containing neither the `X-Ouinet-Sig1` or `X-Ouinet-Sig0` headers, this reply can be served to the user application as normal, but it cannot be stored in the distributed cache.

#### Examples

An injector server transmitting the example cache entry described at the end of the [Cache structure](#cache-structure) section to a client might send the following HTTP response:

```
HTTP/1.1 200 OK
X-Ouinet-Version: 4
X-Ouinet-URI: https://example.com/hello
X-Ouinet-Injection: id=qwertyuiop-12345,ts=1584748800
Date: Sat, 21 Mar 2020 00:00:00 GMT
Content-Type: text/plain
X-Ouinet-Sig0: keyId="ed25519=<key>",algorithm="hs2019",created=1584748800, headers="(response-status) (created) x-Ouinet-version x-Ouinet-uri x-Ouinet-injection date content-type",signature="<header-signature-base64>"
X-Ouinet-BSigs: keyId="ed25519=<key>",algorithm="hs2019",size=5
Transfer-Encoding: chunked
Trailer: Digest, X-Ouinet-Data-Size, X-Ouinet-Sig1

5
Hello
5;ouisig=<block-signature(0)>
 worl
2;ouisig=<block-signature(1)>
d!
0;ouisig=<block-signature(2)>
Digest: SHA-256=wFNeS+K3n/2TKRMFQ2v4iTFOSj+uwF7P/Lt98xrZ5Ro=
X-Ouinet-Data-Size: 12
X-Ouinet-Sig1: keyId="ed25519=<key>",algorithm="hs2019",created=1584748800, headers="(response-status) (created) x-Ouinet-version x-Ouinet-uri x-Ouinet-injection date content-type digest x-Ouinet-data-size",signature="<full-signature-base64>"

```

If the injector server decided to only create a complete cache entry signature, it might instead send the following HTTP response:

```
HTTP/1.1 200 OK
X-Ouinet-Version: 4
X-Ouinet-URI: https://example.com/hello
X-Ouinet-Injection: id=qwertyuiop-12345,ts=1584748800
Date: Sat, 21 Mar 2020 00:00:00 GMT
Content-Type: text/plain
Transfer-Encoding: chunked
Trailer: Digest, X-Ouinet-Data-Size, X-Ouinet-Sig1

12
Hello world!
0
Digest: SHA-256=wFNeS+K3n/2TKRMFQ2v4iTFOSj+uwF7P/Lt98xrZ5Ro=
X-Ouinet-Data-Size: 12
X-Ouinet-Sig1: keyId="ed25519=<key>",algorithm="hs2019",created=1584748800, headers="(response-status) (created) x-Ouinet-version x-Ouinet-uri x-Ouinet-injection date content-type digest x-Ouinet-data-size",signature="<full-signature-base64>"

```

When sending only a complete cache signature like the example above, the injector server might also avoid chunked transfer encoding entirely, and send the following response:

```
HTTP/1.1 200 OK
X-Ouinet-Version: 4
X-Ouinet-URI: https://example.com/hello
X-Ouinet-Injection: id=qwertyuiop-12345,ts=1584748800
Date: Sat, 21 Mar 2020 00:00:00 GMT
Content-Type: text/plain
Content-Length: 12
Digest: SHA-256=wFNeS+K3n/2TKRMFQ2v4iTFOSj+uwF7P/Lt98xrZ5Ro=
X-Ouinet-Data-Size: 12
X-Ouinet-Sig1: keyId="ed25519=<key>",algorithm="hs2019",created=1584748800, headers="(response-status) (created) x-Ouinet-version x-Ouinet-uri x-Ouinet-injection date content-type digest x-Ouinet-data-size",signature="<full-signature-base64>"

Hello world!

```

Of these three examples, the last two would be considered equivalent by a recipient client. The client would recognize the first example as being semantically equivalent to the last two, but it would be able to stream the resource content incrementally while still downloading the remainder of the content simultaneously.

### Peer-to-peer cache entry exchange

When a Ouinet client stores a collection of cache entries in its device local storage, it can share these cache entries with other users that wish to access them. By fetching cache entries from other users in this way, without involvement of the injector servers, a Ouinet client can access web content even in cases when it cannot reach the injector servers.

A Ouinet client willing to share its cache entries with others can serve HTTP requests using a protocol very similar to that used by the injector servers. Unlike injector servers, a Ouinet client participating in the distributed cache will only respond to such requests by serving a copy of a cache entry it has stored in its local device storage. Using this system, a client wishing to fetch a cached resource from another client that stores a cache entry for that resource can establish a peer-to-peer connection to that client, send an HTTP request for the cached resource, and retrieve the cache entry. The recipient can then verify the legitimacy of the cache entry, use the resource in a user application, and optionally store the resource in its own local storage.

By this mechanism, Ouinet clients can fetch cache entries from other Ouinet clients that store a copy of the cache entry in their local storage. For this to function as a distributed cache, however, it is not sufficient for a Ouinet client to be able to fetch resources from other clients that store it; a client attempting to gain access to a web resource also needs some way to determine *which* users hold such cache entries, as well as details on how to establish a connection to these clients. Only with such a mechanism can a Ouinet client obtain access to a web resource by acquiring a list of other clients that hold a cached copy of the resource, connect to one such client, and fetch a copy of a cache entry describing that resource.

The Ouinet distributed cache uses a distributed hash table to store this index information in a distributed way. Using this distributed hash table, clients that store a particular cache entry and are willing to share this cache entry with others can announce this fact in the global distributed hash table. Clients that wish to access a certain resource using the distributed cache can query the distributed hash table for a list of clients that share it, and then proceed to connect to one or several of such clients in an attempt to fetch the cache entry for their own use.

#### Peer-to-peer connections

When a Ouinet client wishes to share its stored cache entries with other clients, it can open a server socket using the [uTP protocol](http://bittorrent.org/beps/bep_0029.html), and publish the IP address and UDP port on which they are listening in the distributed hash table. Clients wishing to download cache entries from them can connect to this socket. Once a connection is established, the connecting client can request cache entries using the HTTP protocol, as described in the next section.

The uTP protocol is rarely blocked by network operators, due to its association with the BitTorrent system. For this reason, it is a more suitable choice in the Ouinet project for a stream-oriented peer-to-peer protocol than alternatives such as TCP. Additionally, the uTP protocol has several characteristics that make it particularly reliable in networks that are restricted by the limitations of NAT routing, which makes it a good choice for peer-to-peer communication on mobile devices.

#### Cache entry requests

When a client wishing to fetch a cache entry has established a peer-to-peer connection to a client that has such a cache entry in storage, it can then start sending HTTP requests over this connection. The sharing client can respond to these requests by serving a stored cache entry as a response, or it can respond with an error message if it does not store such a cache entry.

A cache entry HTTP request sent over a peer-to-peer connection between two Ouinet clients must have the form of a GET or HEAD request, and use a request URI specified in absolute form; that is, the HTTP request must use a full URI on the first line of the HTTP request, which includes a protocol specification and a hostname. Any `Host` headers present in the HTTP request are ignored. The HTTP request must include an `X-Ouinet-Version` header, describing the version of the Ouinet protocol in use; any other headers in the HTTP request that are not described in this section are ignored by the sharing Ouinet client.

When a Ouinet client sharing cache entries over a peer-to-peer connection receives such a request, it can reply with one of the three response types specified in the [Injector-to-client cache entry exchange](#injector-to-client-cache-entry-exchange) section. If the client stores a cache entry for the requested URI, and that cache entry is signed using a signature stream, the client can send a signature stream response. If the client stores a cache entry for the requested URI which only contains a complete cache entry signature, the client can send a complete cache entry signature response. If the client does not store any cache entries for the requested URI, it must reply with an HTTP reply using the `404 Not Found` status code.

If the client sharing a cache entry sends an HTTP response containing a cache entry stored in its local storage, it generally knows the contents of the `Digest`, `X-Ouinet-Data-Size`, and `X-Ouinet-Sig1` headers before starting transmission of the cache entry. In this case, it may choose to send these fields as HTTP headers instead of trailers.

#### Range requests

When a Ouinet client stores a cache entry signed using a signature stream, it can transfer fragments of the cached resource to a recipient client, who can then verify that these fragments form part of a legitimate cache entry without holding a copy of the complete cache entry. Besides streaming media resources from the distributed cache by fetching and verifying consecutive blocks of resource data, this structure also makes it possible for a client to fetch and verify a fragment in the *middle* of a large resource. This functionality can be used by clients to fetch a small part of a larger resource, or to resume downloading a cache entry from one client after having fetched parts of the entry from a different client.

The Ouinet client implements this functionality in peer-to-peer connections through the mechanism of HTTP range requests. A client can send a cache entry HTTP request containing the `Range` header, requesting that the responding client sends it only the fragment of the resource delineated by this range; for example, a client might request the second unit of thousand bytes by sending the `Range: bytes=1000-1999` request header. If the responding client stores a cache entry for the requested resource signed using a signature stream, it can respond by sending only those resource data blocks that cover this byte range, along with block signatures for those data blocks. If the requested byte range does not align to the block size, the responding client has to send somewhat more data than was requested, rounded up to the nearest block size boundary.

For a client to verify the legitimacy of a sequence of consecutive blocks that do not include the first block in the resource, it is not sufficient for the client to hold the block signatures for those blocks. Because each block signature contains a reference to the block hash of the previous block, the verifying client additionally needs to hold the block hash of the block immediately preceeding the first block it wishes to verify, unless the first block it wishes to verify is also the first block in the resource. That is to say, if a client holds `block(i)`, `block(i + 1)`, ..., `block(j)`, for `0 < i <= j`, it needs `block-signature(i)`, `block-signature(i + 1)`, ... `block-signature(j)`, and `hash(i - 1)` to be able to perform a verification of this signature stream. To make this possible, a client responding to a range request with a partial content response will add this preceeding hash to the HTTP response, in the form of an additional chunk extension.

A client receiving a range request for a cache entry can reply with a response containing that partial data, as described above, if it has the facilities and metadata to do so; if it does not, it can send a response containing the complete resource, as if the `Range` header were not present at all. If it does reply with a response containing partial data, the client will send a response structured as a variant of the regular signature stream response, to which the following modifications apply:

* A partial data response uses HTTP status code `206 Partial Content`. This indicates to the recipient that the response contains partial data.
* The response contains a `Content-Range` header, describing the resource fragment included in the response. This range may be broader than the requested range, to ensure the range is aligned to the block boundary. A partial data response may include multiple ranges.
* The response contains an additional `X-Ouinet-HTTP-Status` header, containing the status code of the response that would have been sent if no `Range` was requested, denoted as an integer value. The recipient requires this original status code to be able to verify the cache entry signature, and will substitute it when performing this verification.
* The response contains one `ouisig` chunk extension for each data block included in the resource fragment, in the order that those data blocks are transferred, containing the block signatures for those blocks. Block signatures for blocks that are not covered by the response are not included.
* The response contains one `ouihash=<hash>` chunk extension for each separate range described in the `Content-Range` header, except for those ranges that contain the first block in the resource, in the same order as the ranges described in the `Content-Range` header. For each range described in the `Content-Range` header that covers the fragment of the resource from `block(i)` up to `block(j)`, the `<hash>` contains `hash(i - 1)`, in base64 encoding.

#### Examples

A client wishing to fetch only the second half of the example cache entry described at the end of the [Cache structure](#cache-structure) section might send the following peer-to-peer cache entry request to a different client:

```
GET https://example.com/hello HTTP/1.1
X-Ouinet-Version: 4
Range: bytes=6-11

```

If the receiving client contains a cache entry for this resource signed using a signature stream, it might send the following reply:

```
HTTP/1.1 206 Partial Content
X-Ouinet-Version: 4
X-Ouinet-URI: https://example.com/hello
X-Ouinet-Injection: id=qwertyuiop-12345,ts=1584748800
Date: Sat, 21 Mar 2020 00:00:00 GMT
Content-Type: text/plain
X-Ouinet-Sig0: keyId="ed25519=<key>",algorithm="hs2019",created=1584748800, headers="(response-status) (created) x-Ouinet-version x-Ouinet-uri x-Ouinet-injection date content-type",signature="<header-signature-base64>"
X-Ouinet-BSigs: keyId="ed25519=<key>",algorithm="hs2019",size=5
Digest: SHA-256=wFNeS+K3n/2TKRMFQ2v4iTFOSj+uwF7P/Lt98xrZ5Ro=
X-Ouinet-Data-Size: 12
X-Ouinet-Sig1: keyId="ed25519=<key>",algorithm="hs2019",created=1584748800, headers="(response-status) (created) x-Ouinet-version x-Ouinet-uri x-Ouinet-injection date content-type digest x-Ouinet-data-size",signature="<full-signature-base64>"
Transfer-Encoding: chunked
Content-Range: bytes 5-11/12
X-Ouinet-HTTP-Status: 200

5;ouihash=NhX4DJ0pPtdAJof5SyLVjlKbjMeRb4+sf933+9WvTPd309eVp6AKFr9+fz+5Vh7puq5IDan+ehh2nnGIawPzFQ==
 worl
2;ouisig=<block-signature(1)>
d!
0;ouisig=<block-signature(2)>

```

#### Distributed hash table

When a Ouinet client stores a cache entry for a particular resource and is willing to share it with others, it needs to communicate this fact to other users that might be interested in accessing this cache entry. After all, a client wishing to fetch this cache entry can only do so if it knows where the entry is to be found. For the same reason, the client storing the entry needs to communicate the address by which it can be reached.

The Ouinet distributed cache uses the BitTorrent distributed hash table as a structure for communicating this information. The BitTorrent distributed hash table is a distributed structure traditionally used by the BitTorrent system to store information on which users share pieces of which BitTorrent files; the Ouinet distributed cache uses it in a similar fashion, and uses it to store which Ouinet clients are sharing which cache entries.

When a Ouinet client stores a particular cache entry on its device storage, and is willing to share it with others using a peer-to-peer connection, it sends an announcement message to the BitTorrent distributed hash table containing the IP address and port by which other clients can establish a peer-to-peer connection with the client. This announcement message is addressed to the distributed hash table location consisting of a hash of the URI of the resource contained in the cache entry. Clients wishing to fetch a cache entry for a particular URI can send a request to the BitTorrent distributed hash table, addressed to the distributed hash table location consisting of that same hash, requesting a list of IP addresses and port numbers of clients that have sent such an announcement message. After receiving such a list from the distributed hash table, the client can then establish a peer-to-peer connection to one of the IP addresses listed in the reply, and start requesting cache entries.

The list of Ouinet clients that are sharing a particular cache entry using the peer-to-peer system is stored at a distributed hash table location that is based on the URI of the resource contained in the cache entry, as well as the injector server that created the cache entry. This location is computed as the SHA1 hash of the string `ed25519:<public-key>/v<protocol-version>/uri/<uri>`, where `<public-key>` is the public key of the injector server used to sign the cache entry, encoded using lower-case unpadded base32; `<protocol-version>` is the version of the Ouinet protocol; and `<uri>` is the URI of the resource contained in the cache entry.

#### Resource groups

In some applications of the Ouinet distributed cache, clients frequently store significant numbers of cache entries that are all related to each other. For example, a web browser making use of the Ouinet client library might store a cache entry for a web page, as well as several dozens of cache entries for the different media files referred to by that web page. In a web browser application, this behavior is often statistically reliable: almost every user storing a cache entry for a given web page will also store cache entries for linked resources, because the web browser has loaded those resources on each user's device.

In such cases, the clients holding these cache entries will have to send and periodically repeat announcements to the distributed hash table for dozens, if not hundreds, of separate resources. This maintenance traffic required to participate in the distributed hash table can be a nontrivial drain on resources. What is more, this usage of the distributed hash table is unnecessarily inefficient, for the distributed hash table will store very nearly identical lists of Ouinet clients for each of those dozens or hundreds of resources.

To avoid this inefficiency, the user application making use of the Ouinet client library can instruct the client library to combine the registrations in the distributed hash table for all these related cache entries into a single registration. To do so, the user application would specify a *resource group* for this collection of resources; if such a resource group is specified, the Ouinet client will not send an announcement to the distributed hash table for each stored cache entry in the resource group, but will instead send a single announcement to the distributed hash table location consisting of a hash of the resource group description. Clients wishing to access resources in this resource group would need to configure the same resource group on their devices, and send a request for connectivity information to the distributed hash table addressed to the hash of the resource group instead. As an example, a web browser application using the Ouinet client library might configure a resource group for each web page, containing the web page itself as well as all resources referenced from that web page.

If registrations for a group of cache entries are announced to the distributed hash table using a resource group, announcements and client requests for those resources are addressed to a location in the distributed hash table that is based on the resource group, rather than on the URI of the individual resources. This location is computed as the SHA1 hash of the string `ed25519:<public-key>/v<protocol-version>/uri/<resource-group>`, where `<public-key>` is the public key of the injector server used to sign the cache entries, encoded using lower-case unpadded base32; `<protocol-version>` is the version of the Ouinet protocol; and `<resource-group>` is the resource group bytestring specified by the application.

The Ouinet client can only fetch resources collected into a resource group if clients storing cache entries in the resource group can be relied on to store cache entries for *all* resources in the resource group, the majority of the time. If a substantial number of clients store only a limited subset of the resources collected in the resource group, a client trying to fetch such a resource stands a good chance of accidentally connecting to a client that does store some cache entry in the resource group, but not the entry the client is interested in. Therefore, the Ouinet client does not attempt to identify resource groups automatically, and instead relies on a user application to configure resource groups, if applicable.

### Out of band cache entry exchange

In addition to transferring cache entries between different participants in the distributed cache using a network connection, applications using the Ouinet system can also distribute cache entries in more ad-hoc ways, such as by distributing storage devices containing collections of cache entries. Techniques such as this do not play a role in the primary application of the Ouinet system, but can be applicable to more specialist applications. The different ways in which such techniques could be arranged logistically are not specified in further detail in this document.



# Injector Servers

The Ouinet network contains a collection of *injector servers*, which are modified HTTP proxy servers that can perform HTTP requests on behalf of Ouinet clients that cannot reach the required HTTP origin servers directly. Injector servers have the authority to create cache entries for the Ouinet distributed cache based on the HTTP requests they perform on behalf of Ouinet clients. Their responsibility is not limited to the distributed cache, however; Ouinet clients can use the injector servers to fetch resources to be stored in the distributed cache, as well as to perform HTTP requests on their behalf that are not eligible for caching, serving in that facility like a standard HTTP proxy.

Where standard HTTP proxies usually can be connected to in the form of a TCP connection, the Ouinet injector servers can be reached through a variety of methods that encode a network connection in different ways. The primary reason for this variation is to make it difficult for a network operator to block or restrict access by users to the Ouinet injector servers; instead of blocking one particular protocol with an easily recognizable signature, a network operator would have to block a larger collection of individual techniques, each with different strengths and weaknesses. For the same reason, the collection of connection techniques used by the injector servers can vary over time. Some of the mechanisms by which injector servers might be reached are applications of existing network obfuscation projects, such as the [Pluggable Transports](https://www.pluggabletransports.info/) project; others are innovations of the Ouinet project.

Injector servers form a trusted part of the Ouinet network. With the authority to create cache entries for the distributed cache, as well as the position as an intermediary for many Ouinet client HTTP requests, a compromised injector server could do signicant harm to users of the Ouinet system. The Ouinet project operates a collection of injector servers; however, to reduce the reliance on the trustworthiness of these servers, the Ouinet network is set up in such a way that any organization can run its own collection of injector servers, independent from the Ouinet project. Users or applications wishing to make use of such third-party injector servers can configure their injector servers of choice in the Ouinet client. This chapter specifies the ways by which injector servers operated by the Ouinet project can be reached; different configurations will likely apply to third-party injector servers.


## Injector server connections

The Ouinet project implements several different mechanisms by which a Ouinet client can establish a connection to an injector server. The Ouinet client and injector servers both can be configured to use selected subsets of these mechanisms, adapting their strategies to the conditions of the networks on which they operate. Besides making it difficult for a network operator to block connections to the injector servers, this configurable varied approach makes it possible to respond to developments in network blocking technology.

When an injector server is configured to use multiple connection mechanisms, it will listen for connections through any of these mechanisms. A Ouinet client configured with multiple connection mechanisms will try to reach an injector server through any of these mechanisms, trying different options until a connection is established successfully. A Ouinet client may keep track of which mechanisms tend to work well and which ones work poorly, and try to use the most effective option in the future.

Once a Ouinet client establishes a connection to an injector server, a TLS session is established over the connection. The configuration of the Ouinet client includes a specification of the TLS certificate used by the injector server, which is used by the client to verify that it is connected to an authentic injector server. Once it has confirmed this authenticity, the Ouinet client can then start sending HTTP proxy requests to the injector server.

### Injector uTP sockets

As one of the mechanisms by which a connection to an injector server can be established, injector servers accept connections using the [uTP protocol](http://bittorrent.org/beps/bep_0029.html). As mentioned in the [Peer-to-peer cache entry exchange](#peer-to-peer-cache-entry-exchange) section, connections using the uTP protocol are rarely blocked by network operators, making this a good first choice option for connecting to an injector server.

The IP addresses on which the injector servers can be reached through the uTP protocol are not published using the DNS system, because DNS communications are very frequently blocked by network operators. Instead, the Ouinet network stores the IP addresses and uTP port numbers of the injector servers in the BitTorrent distributed hash table. Active injector servers announce their IP address and port number to the BitTorrent distributed hash table addressed to a configured distributed hash table location, and Ouinet clients request a list of such injector servers from the distributed hash table in the same way.

The location in the distributed hash table used to store the list of active injector servers is configured differently for each organization operating a collection of injector servers, and needs to be configured the same way in Ouinet clients wishing to use these injector servers. This location is derived from the public key of the configured injector servers, and computed as the SHA1 hash of the string `ed25519:<public-key>/v<protocol-version>/injectors`, where `<public-key>` is the public key of the injector server used to sign cache entries for the distributed cache, encoded using lower-case unpadded base32; and `<protocol-version>` is the version of the Ouinet protocol.

### Peer-to-peer tunnels

When a client establishes a connection to an injector server and verifies that the injector server it is connected to is genuine, it may then choose to function as an intermediary, allowing less fortunate clients to reach the injector server through them. A client functioning as an intermediary in this way is referred to as a *bridge node*.

If a client chooses to function as a bridge node, it will accept connections using the uTP protocol, and announce its address details to the BitTorrent distributed hash table. Whenever the client accepts a connection in this way, it will create a connection to an injector server through one of the methods described in this chapter, and forward all traffic received over the incoming connection to the connection with the injector server, and vice versa. This lets the client function as an intermediary between an injector server, and a different client that is unable to connect to the injector servers directly.

A client wishing to function as a bridge node should start announcing its participation as soon as it has established a verified connection to an authentic injector server, while using a connection that does not itself rely on a bridge node. Likewise, the client should avoid making a connection to an injector server on behalf of another client if that connection itself makes use of a bridge node, for such a connection with multiple intermediaries would be unnecessarily inefficient.

Like the distributed hash table location used to store the list of active injector servers, the location used to store the list of bridge nodes is something that varies based on the configuration of the Ouinet client. This location is computed as the SHA1 hash of the string `ed25519:<public-key>/v<protocol-version>/bridges`, where `<public-key>` is the public key of the injector server used to sign cache entries for the distributed cache, encoded using lower-case unpadded base32; and `<protocol-version>` is the version of the Ouinet protocol.

### Other connection methods

Besides the two connection mechanisms mentioned above, the Ouinet client and injector servers can also be configured to use a variety of connection mechanisms based on network protocols implemented in different projects. Examples include the suite of protocol implementations developed as part of the [Pluggable Transports](https://www.pluggabletransports.info/) project, and connections established using the [I2P protocol](https://geti2p.net/). The exact list of such mechanisms implemented in the Ouinet software varies frequently, and is not described in detail in this document.


## Injector server functionality

Once a Ouinet client has established a connection to an injector through any of the mechanisms mentioned in the previous section, the client can request several different operations to be performed by the injector server. These operations are all requests to perform a proxied HTTP request in some form, but with different semantics.

For the injector server to perform any proxy requests at all, the client needs to send a `Proxy-Authenticate` header as part of each request sent to it, authenticating itself to the injector server. For injector servers operated by the Ouinet project, the authentication required to access these servers is published as public knowledge, and this authentication requirement serves no real security purpose, but instead ensures that the injector servers are not used by accident by non-Ouinet applications that did not expect to be connected to a Ouinet injector server. For injector servers operated by third parties, this authentication mechanism may have a legitimate security purpose.

### Cache injection requests

The Ouinet client can request the injector server to fetch a resource and construct a cache entry based on the response, which the client can then share in the distributed cache. The injector server will send a canonical request to the origin server and construct a cache entry after receiving a response, using the procedure described in the [Cache entry construction](#cache-entry-construction) section.

The Ouinet client can request this behavior by sending an HTTP request that includes an `X-Ouinet-Version` header. The injector server will reply with a response described in the [Injector-to-client cache entry exchange](#injector-to-client-cache-entry-exchange) section.

### Cache-ineligible proxy requests

If a Ouinet client wishes to perform a request that is not eligible for caching, such as a POST request or a GET request that contains confidential information, it can request the injector to just forward such a request to the origin server, in the same manner as a standard proxy server. To request this, the client can send any HTTP request that does not include an `X-Ouinet-Version` header. The injector server will respond with a standard HTTP request that does not include any extensions specific to the Ouinet project.

### TLS tunnels

If a Ouinet client sends a request to an injector server using one of the above two mechanisms, the injector server has full access to both the HTTP request and the resulting HTTP response, due to its role as an intermediary. For reasons of confidentiality, this is not always desirable, for the HTTP request or response may contain private information that would ideally be known only to the client and the origin server. When the HTTP request is a request for an HTTPS resource, the origin server may even be implicitly relying on this being the case. While the injector server is a trusted part of the Ouinet network, it would still be ideal if such a request could be processed without depending on the security of the injector server.

To this end, if the Ouinet client wishes to request an HTTPS resource, it may send a CONNECT request to the injector server. When receiving a CONNECT request, the injector server will establish a connection with the origin server, and then proceed to only relay information between the Ouinet client and origin server, without doing any processing on this traffic. The Ouinet client can then establish a TLS session over this tunneled connection, ensuring that the injector server cannot eavesdrop on or interfere with traffic between the Ouinet client and origin server. As a side effect, the injector server will never and cannot create a cache entry based on requests performed in this way.



# Client Library

The Ouinet client library is a software library that implements the Ouinet client, as described in this document. The Ouinet client library is structured as an HTTP proxy server that application software can connect to, and which can perform HTTP requests sent to it by the application software using the mechanisms of the Ouinet project. Applications using the Ouinet client library can invoke a library operation to start this proxy server in the background, and then configure usage of this proxy server for HTTP requests performed by the application.

When the Ouinet client library gets initialized, it will attempt to establish a connection to one of the configured injector servers, using any of the mechanisms described in the [Injector server connections](#injector-server-connections) section. Depending on the configuration, it may also begin participating in the Ouinet distributed cache, operating as a bridge node, or both.

Once the Ouinet client library proxy server is operational, it will start accepting HTTP proxy requests. The client will attempt to satisfy these requests using any of the mechanisms specified in the [Content Access Systems](#content-access-systems) section, and reply with a response when it has succeeded.

When an application functioning as an HTTP client is configured to use an HTTP proxy server, it will generally not send HTTPS requests to this proxy server, to avoid a breach of confidentiality. Instead, such applications will usually send a CONNECT request to the proxy server instead, establishing a tunneled connection to the origin server that the proxy server cannot eavesdrop on or manipulate. While this is generally a sensible decision when applied to most proxy servers, this behavior would make it impossible for the Ouinet client to perform any of the Ouinet functionality to HTTPS requests.

In order to be able to apply the Ouinet mechanisms to HTTPS requests despite this complication, the Ouinet client proxy terminates all TLS sessions that would otherwise be tunneled through the client proxy. This functionality makes the Ouinet client library behave as a type of man-in-the-middle, which applications will typically rightly reject as a security breach. If an application wishes to use the Ouinet library to perform HTTPS requests, it must therefore configure the TLS certificate used by the Ouinet client proxy as being authorized for this purpose. The Ouinet client library generates such a certificate, private to the specific device running the Ouinet client, when starting the client proxy; this certificate functions as a certificate authority for connections made to the Ouinet client proxy, and should be configured as such in the application.

When an end-user application sends an HTTP request to the Ouinet client proxy, it can include certain configuration options to adjust the behavior of the Ouinet client; in particular, it can include instructions as to how the Ouinet client should and should not attempt to resolve individual HTTP requests. These configuration settings are included in the form of additional HTTP headers specifically to the Ouinet project. Likewise, the Ouinet client can communicate additional details regarding resources it has fetched for the benefit of the application; these details, too, are communicated in the form of additional HTTP headers included with the HTTP response. The exact list of such headers used in the communications between the end-user application and the Ouinet client proxy are described below.


## Request headers

The application sending HTTP requests to the client library can include the following HTTP headers to customize the Ouinet client behavior:

* `X-Ouinet-Private`: If equal to `true`, indicates that this request is ineligible for caching. The Ouinet client may not use the distributed cache lookup mechanism, nor attempt to create a cache entry based on this request.
* `X-Ouinet-Group`: If set, specifies the resource group to which the requested resource belongs. The Ouinet client should search the distributed cache using a distributed hash table location based on this group, both when attempting to fetch the resource from the distributed cache, and when sharing a cache entry for this resource using the distributed cache.

## Response headers

When sending a response to an HTTP request, the Ouinet client may append the following HTTP headers to the response:

* `X-Ouinet-Version`: Contains the version of the Ouinet protocol used by the Ouinet client.
* `X-Ouinet-Error`: Added by the Ouinet client to any responses indicating that the Ouinet client could not resolve the assorted request. If set, the value of this header consists of a numeric error code, followed by an error description in plain text.
* `X-Ouinet-Warning`: Added by the Ouinet client when a resource request has not failed, but did encounter a situation that the user should be made aware of. If set, the value of this header is a description in plain text.
* `X-Ouinet-Source`: Contains the content access mechanism used to satisfy the assorted request. Possible values include:
  * `front-end`: The request was not a proxy request, but rather an HTTP request to the Ouinet client control panel. The Ouinet client control panel is not described further in this document.
  * `origin`: The request was satisfied using a direct HTTP request to the responsible origin server.
  * `proxy`: The request was satisfied using a request to the injector server that is not eligible for caching.
  * `injector`: The request was satisfied using a request to the injector server that is eligible for caching, and that may have been stored in the distributed cache.
  * `local-cache`: The request was satisfied using a cache entry stored in device local storage.
  * `dist-cache`: The request was satisfied by retrieving a cache entry using the distributed cache.
* `X-Ouinet-Injection`: Added by the Ouinet client to any response that was satisfied using the distributed cache, either by retrieving a cache entry from the local storage or a peer-to-peer connection, or by requesting a cache entry created by an injector server. If set, this header contains the unique ID representing the cache entry containing the requested resource.
