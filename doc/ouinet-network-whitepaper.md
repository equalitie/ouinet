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



# Injector Servers



# Client Library



# Security Concerns


