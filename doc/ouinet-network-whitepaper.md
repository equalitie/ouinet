# Overview

The ouinet network is a collection of software tools and support infrastructure that provide access to web resources when access to the unrestricted internet is unreliable or unavailable. Ouinet provides a variety of different mechanisms through which web content can be accessed, and aims to provide a way to access requested content, even when using a limited internet connection on which access to these resources would normally be impossible. By using different content access mechanisms as network conditions require, ouinet can provide the best web access the situation allows, degrading gracefully when faced with increasing degrees of network disruption.

The ouinet network contains a collection of specialized public proxy servers, known as *injector servers*, combined with a variety of means by which a user can construct a direct or indirect connection to those injector servers. Together these form one way in which a user can access web resources they cannot reach directly.

In addition, ouinet uses a peer-to-peer distributed content cache system that can store and distribute cached copies of web resources. Users and contributors of ouinet can connect to this network to share copies of web content they recently accessed with other users, especially when those other users are not able to reliably reach an injector server. The content cache can be used only for those web resources that are eligible for caching, and as such cannot fully replace access to the injector servers. Nonetheless, the distributed content cache system can lighten the load on the injector server system during network conditions in which access to the injector servers is a sparse resource, and it can serve as a limited fallback in cases where the injector servers cannot be reached at all.

The ouinet project consists of two primary pieces of software. The *client library* contains all the logic necessary to fetch web resources using the multiple ouinet techniques for doing so, as well as optionally participating in the distributed cache. It is structured as a library, and can function as a base for end-user applications that access the web. The *injector daemon* implements the injector server software, which are hosted by the ouinet network operator. This document describes the workings of both of these systems, as well as the networks underlying their operation.



# Aims and Threat Model



# Actors and Components

The ouinet network relies on the cooperation of a few different parties with different roles and responsibilities. Some of these components form a centralized infrastructure, whereas others form a decentralized cooperative of users and contributors to the system.


## The injector servers

The main backing infrastructure of the ouinet network consists of a set of *injector servers*. A ouinet injector server is a variant of an open proxy server, that users can connect to in order to gain access to web resources they cannot access via more direct means. Injector servers also have other responsibilities, described in more detail in the [next section](#injector-server-connection) and [beyond](#injector-servers).

Injector servers form a trusted part of the ouinet network. In their role as an intermediary between a user and their communication to much of the internet, they are in a position where a malicious implementation could do serious harm to user security. To keep track of the impact of this trusted position, injector servers are identified by a cryptographic identity that is used to authenticate the injector server when connecting to it, as well as certifying data published by the injector server.

Different organizations can run their own collections of injector servers, and users of the ouinet system can configure their devices or applications to use whatever injector servers they wish to trust. The ouinet organization operates one such cluster of injector servers for general purpose use, but there is no requirement for anyone using the ouinet system to make use of these injector servers, and grant it authority thereby.


## The client library

On the user end, the ouinet project provides a piece of software that user applications can use to access web resources through the ouinet network, referred to as the *client library*. As the name suggests, it is implemented as a library to be used by end-user applications that make use of access to the web, rather than being an application in its own right. The detailed behavior of the client library is described in the [Client Library](#client-library) section.

In addition to the logic necessary to access web resources through the different systems that ouinet provides for this purpose, the client library also contains the functionality required for the application to participate in the peer-to-peer components of the ouinet network. Active participation in these peer-to-peer systems is not a requirement when using the client library, and indeed there exist applications for which active involvement in the network is unlikely to work well. The aim of this integration is to allow an application to enable contribution to the network where this is reasonably practical, and forego it otherwise.


## Support nodes

The ouinet network relies on the existence of a substantial community of users that contribute to the peer-to-peer systems that underlie ouinet. However, it is to be expected that many users of the ouinet library will do so on low-powered devices for which a significant peer-to-peer contribution is not very practical.

To remedy this, users with access to well-connected devices can contribute to the resources of the ouinet network by operating a *support node*. This is a server that runs a configuration of the client library, implemented as a standalone program, which does not serve any immediate applications for its operator, but instead is setup solely to contribute its resources to the ouinet peer-to-peer systems. The presence of a few strategically-placed volunteer support nodes can greatly benefit the performance and reliability of the ouinet network.


## End-user applications

The ouinet project proper does not contain any end-user applications that make use of the ouinet client library; the client library is designed as a building block for others to build upon, rather than as a tool that is useful for end users directly. But of course, the ouinet project requires very much on the existence, quality, and useful application of such external projects.



# Content Access Systems

The ouinet client library uses three distinct methods for getting access to a web resource. It can establish a connection through an injector server, through one of a variety of methods; it can fetch a resource from the distributed cache, if the resource is eligible for caching; or it can make a direct connection to the authoritative webserver serving the resource, and avoid any ouinet-specific complications.

These access methods have different strengths and weaknesses, and will work well in different situations. In the worst case, the ouinet client will try all of these methods, with different configurations, until it succeeds in fetching the resource. Trying different methods exhaustively is quite inefficient, however, both in terms of request latency and in bandwidth usage. Thus, where possible, the ouinet client will try to estimate which method is likely to work well for different resources and conditions, and minimize inefficiency by trying likely options first. This process is described in detail in the section on [Content Dispatch](#content-dispatch).


## Direct origin access

The most straightforward way the ouinet client can satisfy a resource request is to simply forward the request to the webserver responsible for serving the resource, like a standard proxy server. Ideally, the ouinet client could try this before doing anything else, and only move on to more indirect ways of fetching the same resource if this direct attempt fails.

In practice, this is not as straightforward as it first appears. Networks that block access to particular content do not typically provide a machine-readable signal that access has been blocked. If a network blocks access to a resource by making it impossible for the ouinet client to establish a connection to the origin webserver, or by terminating such a connection prematurely, the ouinet library can indeed register the failure and move on to other content access mechanisms. Many content-blocking networks, however, instead choose to serve a webpage explaining in human-readable text that access to this content has been blocked. When this happens, it is more difficult for the ouinet client to notice that the request did not complete as desired; such error pages do not usually come with an obvious marker by which the ouinet client can distinguish the error message from the desired resource, and certainly do not do so reliably.

For content that is served over https, the TLS layer can be used to distinguish between genuine responses and network-inserted error messages. A content-blocking network will not be able to provide a valid TLS certificate for the domain whose content it seeks to block, and this failure to establish a properly-certified TLS connection functions as a reliable signal that the direct origin access attempt has failed.

For content served without TLS, however, the ouinet client has no such recourse, and indeed ouinet cannot reliably recognize blocked pages when using direct origin access. As a consequence, direct origin access without https will ideally only be enabled in applications where the application or the user can provide the ouinet client with feedback about success or failure of a direct origin access attempt. For example, a web browser application might have a button whereby a user can report a block page to the ouinet client; an application using the ouinet library to access its http API can report a failure if the API response does not have the expected syntax. When this facility is used, the ouinet client can then choose to avoid direct origin access for future requests, entirely or for a particular class of requests.


## Injector server connection

The ouinet project operates a collection of *injector servers*, which function as a variant of a HTTP proxy server. The client library can satisfy content requests by establishing a connection to such an injector server, and forwarding content requests to it.

In addition to functioning as a proxy server, the other main responsibility of injector servers is to add ("inject") web content into the distributed cache. When an injector server serves a proxy request for a ouinet client, a determination is made whether the assorted response is eligible for caching. If it is, the injector server then creates a signature that covers the response body, headers, and key metadata such as the resource URI and retrieval date. This signature serves as a certificate that the complete HTTP response has been retrieved from the authoritative origin server by a trusted injector server. The injector server then sends this signature to the requesting ouinet client, along with the content response proper.

The combination of a response body, head, metadata, and injector signature for a particular web resource forms a *cache entry* used by the distributed cache system. Any party in the possession of a cache entry can share this entry with other peers; by verifying the signature that is part of a cache entry, a peer receiving such an entry can verify the legitimacy of the entry, as certified by the signatory injector server. After receiving a combination of a content response and a cache entry signature from an injector server, the requesting client can choose to start sharing the resulting cache entry in the distributed cache. The injector server may also choose to start sharing the cache entry in the distributed cache on its own, if it so chooses.

The ouinet project contains several different mechanisms by which a client can establish a connection to an injector server. These mechanisms take the form of different protocols and tunneling systems that can carry a stream-oriented connection as a payload, over which standard protocols such as TLS and HTTP can be transmitted. Injector servers are configured to serve requests via multiple such mechanisms, allowing the ouinet client to use whichever mechanism that does not trigger network blockages.


## Distributed cache lookups

The ouinet network uses a peer-to-peer network that different parties can use to share among each other stored versions of cache-eligible resources. When an injector server serves a client request and determines the corresponding response to be eligible for caching, it can create a *cache entry* that can be used by clients to satisfy content requests, in the same way as would be used by a standard caching HTTP proxy server. Clients holding a copy of such a cache entry can use the peer-to-peer network to share it with other interested clients; and conversely, a ouinet client trying to satisfy a content request can do so by contacting a peer that holds a cache entry for this resource, and requesting a peer-to-peer transfer of this cache entry. Together, this system forms the ouinet *distributed cache*.

Cache entries can only be prepared for distribution in the distributed cache by the authority of an injector server. When an injector server decides that a particular resource response is suitable for use as a cache entry, it will create a cryptographic signature certifying this decision, as described in the previous section. Only cache entries containing such a signature can be shared in the distributed cache, and clients trying to fetch a resource from the distributed cache will verify this signature. In this way, the injector servers as a whole form the root of trust for all content exchanged in the distributed cache. By only sharing and using cache entries that have been certified by a trusted injector server, clients can be confident that the cache data matches the response that a trusted party has received from an authoritative origin server. In particular, this makes it impossible for an attacker to add forged cache entries to the distributed cache, ensuring the legitimacy of responses served from it.

The ouinet client does need to verify that a cache entry received from the distributed cache is usable for the assorted content request. It needs to verify, for example, that the cache entry is not expired, and that the response is applicable to the constraints set in the request headers. The details of this procedure are described in the [Distributed Cache](#distributed-cache) section.



# Distributed Cache

## Introduction

The ouinet network uses a *distributed cache* as one of the methods of getting web content to users that try to access that content. If a web resource is fetched from the authoritative origin webserver on behalf of a user, in many cases the fetched resource can be used to satisfy future requests by different users trying to access the same content. If a user's device holds a copy of such suitable content, the ouinet client can use peer-to-peer communication to transmit copies of this content to other users interested in the content; and conversely, if a user wants to access a certain web resource, the ouinet client can use peer-to-peer communication to request a copy of the resource from a peer. In situations where user access to the centralized ouinet injector infrastructure is unavailable, this technique provides a limited but useful alternative form of access to web content. If access to the injector infrastructure is unreliable or limited, the distributed cache can be used to satisfy resource requests wherever possible, allowing the ouinet client to utilize the injectors only for those requests for which no alternative is available, reducing the load on the unreliable injectors and improving performance.

The caching of web resources is a standard functionality in the HTTP protocol. The HTTP protocol provides faculties by which an origin server can provide detailed instructions describing which resources are eligible for caching, which ones are not, the duration during which a resource can be cached, and requirements and limitations when caching the resource. HTTP clients implementing this system can satisfy HTTP requests by substituting an HTTP response stored in the cache, subject to the restrictions declared by the origin server, saving on network traffic and improving performance. HTTP client software such as web browsers commonly implement a cache for this purpose, which typically plays a major part in the performance characteristics of such software. The HTTP standard also describes caching HTTP proxies, which act as an performance-improving intermediary to a group of users by using a shared cache for all of them.

The ouinet distributed cache is a variant implementation of such an HTTP cache, in which each ouinet user has access to the combined cached resources of all ouinet users worldwide. Each ouinet client stores cached copies of resources they have recently accessed in the storage of their own device, and will use peer-to-peer communications to transfer these cached resources to other ouinet clients that request access to them. From the viewpoint of a particular ouinet user, the combined caches held by each ouinet user worldwide function as a distributed filesystem containing more cached content than any one user device can realistically store.

Traditional HTTP caches are used in such a way that only a single party writes to, and reads from, the cache. The ouinet distributed cache, on the other hand, forms a distributed filesystem that many different users can store resources in, and fetch resources from. This architectural difference comes with a number of complications that the ouinet system needs to account for. With large numbers of people being able to participate in the distributed cache, the ouinet software cannot assume that all these participants are necessarily trustworthy. When the ouinet client requests a cached resource from some other ouinet user using the peer-to-peer system, or sends some other ouinet user a copy of a cached resource for their benefit, the ouinet client must account for the possibility that their peer may have malicious intent. To accommodate this concern, the ouinet distributed cache uses several systems to make it possible to cooperate on resource caching with untrusted peers.

When an HTTP client wishes to respond to an HTTP request by substituting a cached response, it needs to ensure that the stored cached response is in fact a legitimate response sent by the responsible origin server to the associated request. In a traditional HTTP cache operated by a single party, this is a trivial requirement, for the cache software will only store a cached response after receiving it from the responsible origin server, which means the cache storage serves as a trusted repository of cached content. In the ouinet distributed cache, on the other hand, cached resources may be supplied by untrusted peers, and there is no obvious way in which the receiving party can verify that this response is a legitimate one; a malicious peer could easily create a forged response, add it to its local cache storage, and send it to its peers. This behavior is a threat that the ouinet client needs to be able to guard against.

To avoid this problem, the ouinet system makes use of trusted injector servers, charged with the authority of creating resource cache entries whose legitimacy can be verified by ouinet clients. When these injector servers fetch an HTTP resource on behalf of a user, they determine if the resulting response is eligible for caching; if it is, they will then create a cryptographic signature covering the response, which enables the peer-to-peer distribution of the cached resource in the distributed cache. By verifying this signature, clients can confirm that a cached resource has been deemed legitimate by a trusted injector server.

A different security concern when using an HTTP cache shared between large numbers of people lies in the confidentiality of privacy-sensitive data communicated using web resources. Some HTTP responses contain private information intended only for the recipient, and nobody else; to avoid compromising confidentiality, responses with this characteristic must not be shared using the distributed cache. The ouinet system therefore needs to be able to recognize confidential responses, and mark them as ineligible for caching.

The HTTP protocol contains functionality by which origin servers can specify resources that are ineligible for caching, or ineligible for public caching, on the grounds of confidentiality, which in theory should imply that recognizing confidential responses should be a simple matter. Unfortunately, origin servers in practice do not always adhere to this protocol very accurately. It is reasonably common for origin servers to serve confidential resources while failing to mark them as such, in which case it is critical that the ouinet system is able to recognize the confidentiality of the resource by some other means. Much more common still is the reverse situation, in which a non-confidential resource is marked as ineligible for caching on confidentiality grounds by the origin server, typically for commercial reasons. If the ouinet system were to accept these judgements uncritically, the amount of resources eligible for caching would be sharply limited, reducing the utility of the distributed cache considerably. To avoid both problems, the ouinet system uses a heuristic analysis to recognize cases where the origin-supplied cache-eligibility judgement is misleading.

The remainder of this section describes the details of the operation of the ouinet distributed cache system. It details the exact data stored in the cache, and its interpretation; the system used for signing and verification of cached resources; and finally, it describes the methods and protocols by which different actors in the ouinet network may exchange cached resources with each other.


## Cache structure

The ouinet distributed cache conceptually consists of a repository of cached web resources. Each such cached resource takes the form of a record referred to as a *cache entry*. A cache entry represents a single HTTP response suitable for using as a cached reply, along with assorted metadata that makes it possible to verify the legitimacy of the cached response, check the response for expiracy or being superceded, and assess its usability. Cache entries are created by the ouinet injector servers, transmitted from injector servers to ouinet clients, stored on client devices, and shared between different clients using peer-to-peer systems.

A cache entry is a data structure consisting of the resource URI, the HTTP response headers, the HTTP response body, additional metadata added by the ouinet injector, and a cryptographic signature asserting the legitimacy of the cache entry.

Clients that participate in the distributed cache store a collection of such cache entries on their device's local storage, and can get access to many more cache entries using the peer-to-peer network. When satisfying an HTTP request, they can search the distributed cache for any cache entries with an URI matching the one in the HTTP request, checking them for validity, and substituting the cached resource as an HTTP response.

### Cache entry construction

Cache entries are created by the ouinet injector servers. Injector servers can create a cache entry by requesting an HTTP resource on behalf of a user, checking the response for cache eligibility, adding necessary metadata, and signing the resulting package.

Cache entries are identified by their resource URI; when a ouinet client seeks to resolve an HTTP request from the distributed cache, it can use any cache entry whose resource URI matches the URI in the HTTP request. For this behavior to work as expected without causing problems, the injector servers should avoid sending multiple HTTP requests for a particular resource URI that are interpreted by the responsible origin server as having different request semantics; this can happen, for example, if the origin server chooses to vary its response based on the user's user agent settings, which are communicated as part of the HTTP request using HTTP headers. In this scenario, the distributed cache would likely end up storing multiple semantically different responses for this resource. The ouinet client would not be able to distinguish between the competing cache entries, on account of them using the same resource URI, causing confusion and unpredictable behavior.

To avoid this problem, the injector servers will not create cache entries based on HTTP responses received after forwarding arbitrary HTTP requests. Instead, when attemping to create a cache entry, the injector servers will only use a single predictable HTTP request for each resource URI, which is allowed to vary only on a carefully selected list of characteristics known not to affect the request semantics. For similar reasons, when creating a cache entry, the injector servers will remove all metadata from the HTTP response whose semantics are likely to change with different requests for the same resource. Together, these two procedures are referred to as *resource canonicalization*.

Separately from the above, the distributed cache mechanism also needs to check whether a particular HTTP response is eligible for storing in the cache at all. Many HTTP resources should not be stored in any cache, because their content changes frequently and unpredictably, or because their content is personalized specifically for the user requesting the resource; this eligibility is typically specified in HTTP response headers. In addition, clients sometimes wish to send an HTTP request without the limitations enforced by the resource canonicalization system; in such cases, the resource can neither be retrieved from the distributed cache, nor stored in it.

The process for constructing a cache entry and storing it in the distributed cache is a procedure that incorporates all these systems. It consists of the following steps:

* The ouinet client wishes to perform an HTTP request.
* The ouinet client checks whether the request is eligible for caching. If it is not, the distributed cache subsystem is not used.
* The ouinet client contacts an injector server, and asks it to create a cache entry corresponding to the HTTP request.
* The injector server canonicalizes the HTTP request.
* The injector server sends the canonicalized request to the responsible origin server, and awaits a response.
* The injector server canonicalizes the HTTP response.
* The injector server adds metadata to the HTTP response in the form of additional HTTP headers, describing the characteristics of the cache entry.
* The injector server creates a cryptographic signature for the cache entry.
* The injector server sends the modified HTTP response to the client, along with the signature.
* The ouinet client checks whether the response is eligible for caching. If it is, it stores the combination of the HTTP response and the signature to the distributed cache.
* The ouinet client resolves the HTTP request, whether or not it was also stored in the distributed cache.

The exact communication between the ouinet client and the injector server, as well as the details of the cryptographic signatures used in cache entry construction, are described in later sections. Other details are described below.

#### Resource canonicalization

When sending an HTTP request to an origin server for the purpose of creating a cache entry, the ouinet injector creates a minimal canonical HTTP request based on the resource URI as well as a small number of request headers derived from the HTTP request sent by the ouinet client. The *canonical request* also contains neutral generic values for certain headers that many origin servers expect to be present.

This canonical HTTP request takes the form of a HTTP/1.1 request, with a request target and `Host:` header derived from the resource URI in the standard way. The canonical request also contains the following headers:

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
* `Content-Length`
* `Content-Type`
* `Content-Encoding`
* `Content-Language`
* `Digest`
* `Transfer-Encoding`
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

When a ouinet injector has created a canonical response for a newly constructed cache entry, it then adds a series of headers that describe the properties of the cache entry itself. These headers aid a ouinet client receiving the cache entry to interpret the cache entry correctly. These headers are stored in the cache entry as part of the HTTP response headers.

The ouinet injector adds the following headers to the cache entry:

* `X-Ouinet-Version`: This describes the version of the ouinet distributed cache storage format. This document describes the distributed cache storage format version **4**.
* `X-Ouinet-URI`: Contains the URI of the resource described by this cache entry.
* `X-Ouinet-Injection`: This describes a unique ID assigned to this cache entry, allowing a receiver to refer unambiguously to this specific cache entry, as well as the time at which the cache entry was created. Encoded as `X-Ouinet-Injection: id=<string>,ts=<timestamp>`, where <string> is a string containing only alphanumeric characters, dashes, and understores; and <timestamp> is an integer value, representing a timestamp expressed as the number of seconds since 1970-01-01 00:00:00 UTC.

The ouinet injector furthermore adds headers related to the cryptographic signature used to verify the legitimacy of the cache entry. This is described in more detail in the [Signatures](#signatures) section.

#### Cache eligibility

The distributed cache system makes a determination, for each resource request, whether that resource is eligible for caching. This process takes place in two parts. When the client is preparing to send an HTTP request, it first determines whether the request is one that can, in principle, be cached; if this is not the case, the ouinet client does not use the distributed cache system at all when satisfying this request. If the request is eligible for caching, the request can be sent to an injector server, which ---all going well--- will reply with a cache entry that can be stored in the distributed cache. The client then makes a second determination whether the response is also eligible for caching. If it is not, the resource request is completed successfully, but the cache entry is not stored.

The ouinet client currently considers a HTTP request to be eligible for caching if it uses the GET HTTP access method, and moreover the resource URI is not on a configurable blacklist of resources that are never eligible. This is certainly an overestimate for general browsing; for one example, there are many web resources that can only be accessed after authenticating using HTTP authentication, or only after authenticating using some cookie-based authentication scheme. This scheme therefore relies on careful configuration of the resource blacklist for it to work well in practice. Improving this heuristic remains a fertile area for future improvement.

To determine whether an HTTP response is eligible for storing in the distributed cache, ouinet uses a variant of the procedure described in [RFC 7234, section 3](https://tools.ietf.org/html/rfc7234#section-3). This RFC describes a procedure determining whether an HTTP response is allowed to be stored in a cache, based on the characteristics of the cache, expiracy information communicated in the response headers, and the `Cache-Control` header. The ouinet distributed cache follows this procedure as written, with two major exceptions:

* The ouinet distributed cache will only store HTTP responses with an status code of 200 (OK), 301 (Moved Permanently), 302 (Found), or 307 (Temporary Redirect). The ouinet project does not wish to store error status pages in the distributed cache.
* If the HTTP response contains the `Cache-Control: private` clause, the ouinet client will use a heuristic analysis to verify that this clause is warranted.

Many origin servers will declare a `Cache-Control: private` clause on resources that are not really private in reality, but which the origin server wishes to personalize in a non-confidential way for each request. For such resources, the ouinet client ideally wishes to store the cache entry as if the `Cache-Control: private` clause was absent, but avoid satisfying requests for this resource using the distributed cache unless no other methods for accessing the resource are available. To determine whether the `Cache-Control: private` clause is used with good reason, ouinet uses the following procedure:

* If the HTTP request uses an HTTP method other than GET, the `Cache-Control: private` clause is warranted.
* If the resource URI contains a query string (that is, if it contains a question mark character), the `Cache-Control: private` clause is warranted.
* If the HTTP request contains any header fields that might contain confidential information, the `Cache-Control: private` clause is warranted.
* If neither of the above applies, the `Cache-Control: private` is unwarranted, and the cache entry is eligible for storage in the distributed cache.

For the purposes of this procedure, the following HTTP response headers are considered to never contain confidential information:

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

When a ouinet client wishes to use a cache entry stored in the distributed cache to satisfy a resource request, it must first verify that the cache entry has not expired, is unusable for some other reason, or needs revalidation from the origin server. To make this determination, the ouinet client largely follows the procedure described in [RFC 7234, section 4](https://tools.ietf.org/html/rfc7234#section-4). It deviates from this procedure on two important points, however.

The procedure described in this RFC allows a client to use a cache entry that is expired, in certain cases where the client is unable to connect to the responsible origin server to request an up-to-date resource. However, this behavior is bound to strict conditions, and the great majority of origin servers specify `Cache-Control` clauses that disallow this behavior, making this mechanism of sharply limited practical value.

Because the ouinet project aims to provide some limited form of access to web resources even in cases where no access to the responsible origin server can be arranged, the ouinet client is willing to use expired cache entries in cases where the procedure described in the RFC specifically disallows this behavior. When the ouinet client cannot establish contact to the responsible origin server in any way ---either directly, or by using an injector server as an intermediary--- and when all cache entries for the resource the client has access to are expired, the client will use this cache entry to satisfy the resource request, despite any `Cache-Control` clauses that would disallow this. This ensures that the ouinet client will provide some limited access to the resource, if this is at all possible.

For similar reasons, the ouinet client is willing to use cache entries that have the `Cache-Control: private` clause set, if no other options are available. As described in the previous section, cache entries with this clause set should only be used by the client as an option of last resort. The ouinet client treats such cache entries equivalently to cache entries that have expired.

### Signatures


## Distribution

### Injector-to-client cache entry exchange

### Peer-to-peer cache entry exchange

### Out of band cache entry exchange



# Injector Servers



# Client Library



# Security Concerns


