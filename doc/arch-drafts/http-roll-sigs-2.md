# Partial content signing

  - Allow streaming content from origin to client.
      - Less memory usage in injector: do not "slurp" whole response.
      - Less latency to get beginning of content in client.
  - Allow streaming content from client to client.
      - Allow progressive validation of content: do not wait to full response (esp. if big like a video).
      - Allow clients to provide data from interrupted downloads (equivalent: some support for "infinite" responses).
  - Allow sending and validating partial content (i.e. HTTP ranges):
      - Injector-to-client
      - Client-to-client
  - Computing signatures is CPU-intensive.
      - Do not sign too small blocks.
      - Not too big either, since each block must be fully received before it can be validated and used.
      - Ideally, allow partial responses with smaller granularity than signature blocks.
  - Avoid replay attacks, also while streaming content:
      - Detect body data from an unrelated exchange, but signed by a trusted injector.
      - Detect blocks from the right exchange, but sent in wrong order or at the wrong offset.
  - If possible, reuse body data stored at the client.

This proposes an HTTP-based protocol to convey necessary information to fullfill the requirements above.

## Summary

The injector owns a **signing key pair** whose public key is known by the client as part of its configuration.

When the injector gets an injection request from the client, it gets the response head from the origin and sends a **partial response head** back to the client with relevant response headers, plus headers allowing other clients to validate the cached response when it is provided to them in future distributed cache lookups.  The later headers include:

  - The request URI (so that the response stands on its own).
  - The identifier of the injection and its time stamp (to tell this exchange apart from others of the same URI).
  - The key, algorithm, **data hash block length** and **data signature block length** used to sign partial data blocks.
  - A **partial signature** of the headers so far.

This signature is provided so that the partial response (head an body) can still be useful if the connection is interrupted later on.

The injector then sends data blocks of the maximum length specified above, each of them followed by a **data block signature** bound to this injection and its offset.  The client need not check the signatures but it can save them to provide them to other clients in case the connection to the injector is interrupted.

HTTP chunked transfer encoding is used to enable providing a first set of headers, then a signature (as a chunk extension) after each sent block, then a final set of headers as a trailer.  The partial signature (and other headers related to transfer encoding) are not part of the final signature.

[Signing HTTP Messages][] is used here as a way to sign HTTP headers because of its simplicity, although other schemes may be used instead.

[Signing HTTP Messages]: https://datatracker.ietf.org/doc/html/draft-cavage-http-signatures-12

## Signature scheme using Merkle DAGs

This signature scheme makes it possible to support verifiable client-to-client range requests with a granularity much smaller than the signature block size. The injector still needs to sign entire blocks of data, but this block size is not tied to the range request granularity.

Parameters of this signature scheme are a **data hash block length**, which is the block size to which range requests are rounded up; a **data signature block length**, which is the size of blocks signed by the injector, which must be a power-of-2 multiple of the data hash block length; and a hash algorithm and public-key signature algorithm.

For a chunk of data the size of up to the data signature block length, the Merkle DAG signature scheme interprets this chunk as a sequence of blocks the size of the data hash block length. This sequence is interpreted as a balanced binary tree of which the leaves are data hash block.

The Merkle DAG signature scheme assigns a **node hash** to each node of this block tree. For a leaf node (that is, a block of data the size of the data hash block length), the node hash of the node is the hash of that data block. For a non-leaf node, the node hash is the hash of the concatenation of the node hashes of the two child nodes.

The hash of the entire data signature block is the node hash of the root of the tree. The public-key signature of the data signature block is a signature based on this hash, along with assorted metadata.

For any subset of the hash blocks that make up a signature block, the authenticity of these hash blocks can be verified if one also knows the node hashes of all maximal unowned subtrees. In particular, for any subsequence of hash blocks that make up a signature block, the authenticity of that subsequence can be verified with access to a logarithmic number of additional hashes. When servicing any range request, the serving peer can support this verification by supplying the missing hashes as well as the signature.

When servicing any range request, peers SHOULD transfer tree hashes for maximal not-requested subtrees, as well as the signature of the entire block, as soon as these hashes become known to the peer. For a peer that is implementing the request using a recursive origin request, this is typically immediately after those subtrees have been downloaded; for a peer that is implementing the request from cache, this is typically before any block data is sent.

## Issues

  - Choose an adequate data block length (can use different ones according to ``Content-Length:``).
  - Choose block signature algorithm.
  - It may only be usable for single ranges (i.e. no ``multipart/byteranges`` body).
  - Block hashes are outside of the signed HTTP head.  Inlining them in the final head may require long Base64-encoded headers for long files.
  - Find out how to easily encode this with extra headers in HTTP
