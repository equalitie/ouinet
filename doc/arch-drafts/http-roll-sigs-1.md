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
  - Allow retrieving the same content shared by different injections of the same URI.
      - So that clients with different injections can still share data.
      - Even if the content is partial.
  - Computing signatures is CPU-intensive.
      - Do not sign too small blocks.
      - Not too big either, since each block must be fully received before it can be validated and used.
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
  - The key, algorithm and **data block size** used to sign partial data blocks.
  - A **partial signature** of the headers so far.

This signature is provided so that the partial response (head an body) can still be useful if the connection is interrupted later on.

The injector then sends data blocks of the size specified above (the last one may be smaller), each of them followed by a **data block signature** bound to its injection, offset, previous blocks' data, and its own data.  The client need not check the signatures but it can save them to provide them to other clients in case the connection to the injector is interrupted.

When all data blocks have been sent to the client, the injector sends additional headers to build the **final response head** including:

  - Content digests for the whole body.  SHA2-256 is used as a compromise between security and digest size.
  - The final content length.

HTTP chunked transfer encoding is used to enable providing a first set of headers, then a signature (as a chunk extension) after each sent block, then a final set of headers as a trailer.

Please note that neither the partial signature nor framing headers (`Transfer-Encoding:`, `Trailer:`, `Content-Length:`) are part of the final signature, so that the receiving client may serve to other clients the final signature in the initial headers instead of the partial one, or even serve the response with identity transfer encoding (without block signatures nor a trailer) and still enable full (but not partial) response verification. The purpose of the `X-Ouinet-Data-Size:` header is to allow verifying data size without forcing the presence or absence of `Content-Length:`, which would break chunked or identity transfer-encoded messages, respectively.

[Signing HTTP Messages][] is used here as a way to sign HTTP headers because of its simplicity, although other schemes may be used instead.

[Signing HTTP Messages]: https://datatracker.ietf.org/doc/html/draft-cavage-http-signatures-12

## Example of injection result

```
HTTP/1.1 200 OK
X-Ouinet-Version: 4
X-Ouinet-URI: https://example.com/foo
X-Ouinet-Injection: id=d6076384-2295-462b-a047-fe2c9274e58d,ts=1516048310
Date: Mon, 15 Jan 2018 20:31:50 GMT
Server: Apache
Content-Type: text/html
Content-Disposition: inline; filename="foo.html"
X-Ouinet-BSigs: keyId="ed25519=????",algorithm="hs2019",size=1048576
X-Ouinet-Sig0: keyId="ed25519=????",algorithm="hs2019",created=1516048310,
  headers="(response-status) (created) x-ouinet-version x-ouinet-uri x-ouinet-injection x-ouinet-http-status date server content-type content-disposition x-ouinet-bsigs",
  signature="BASE64(...)"
Transfer-Encoding: chunked
Trailer: Digest, X-Ouinet-Data-Size, X-Ouinet-Sig1

80000
0123456789...
80000;ouisig=BASE64(BSIG(d607…e58d NUL 0 NUL HASH[0]=SHA2-512(BLOCK[0])))
0123456789...
4;ouisig=BASE64(BSIG(d607…e58d NUL 1048576 NUL HASH[1]=SHA2-512(HASH[0] BLOCK[1])))
abcd
0;ouisig=BASE64(BSIG(d607…e58d NUL 2097152 NUL HASH[2]=SHA2-512(HASH[1] BLOCK[2])))
Digest: SHA-256=BASE64(SHA2-256(FULL_BODY))
X-Ouinet-Data-Size: 1048580
X-Ouinet-Sig1: keyId="ed25519=????",algorithm="hs2019",created=1516048311,
  headers="(response-status) (created) x-ouinet-version x-ouinet-uri x-ouinet-injection x-ouinet-http-status date server content-type content-disposition x-ouinet-bsigs digest x-ouinet-data-size",
  signature="BASE64(...)"
```

The signature for a given block comes in a chunk extension in the chunk right after the block's end (for the last block, in the final chunk); if the signature was placed at the beginning of the block, the injector would need to buffer the whole block in memory before sending the corresponding chunks.

The signature string for each block covers the following values (separated by null characters):

  - The injection identifier (string).

    This helps avoid replay attacks where the attacker sends correctly signed but different blocks from a different injection (for the same or a different URI).

  - The offset (decimal, no padding).

    This helps detecting an attacker which replies to a range request with a range of the expected length, with correctly signed and ordered blocks, that however starts at the wrong offset.

  - A **chain hash** (binary) computed from the chain hash of the previous block and the data of the block itself: for the i-th block, `HASH[i]=SHA2-512(HASH[i-1] BLOCK[i])`, with `HASH[0]=SHA2-512(BLOCK[0])`.

    Signing the hash instead of block data itself spares the signer from keeping the whole block in memory for producing the signature (the hash algorithm can be fed as data comes in from the origin).

    Keeping the injection identifier out of the hash allows to compare the hashes at particular blocks of different injections (if transmitted independently) to ascertain that their data is the same up to that block.

    The chaining precludes the attacker from reordering correctly signed blocks for this injection.  SHA2-512 is used as a compromise between security and speed on 64-bit platforms; although the hash is longer than the slower SHA2-256, it will be seldom transmitted (e.g. for range requests as indicated below).

Please note that this inlining of signatures also binds the stream representation of the body to this particular injection.  Storage that keeps signatures inline with block data should take this into account when trying to reuse body data.

Common parameters to all block signatures are kept the same and factored out to `X-Ouinet-BSigs` for simplicity and bandwidth efficiency.  Even if each block size could be inferred from the presence of a chunk extension, having the signer commit to a fixed and explicit size up front (with the exception of the last block) helps the consumer of the signed response to easily validate chunk boundaries and discard responses with too big blocks.  In the example, chunks are equivalent to blocks; this is the simplest implementation but it is not compulsory: blocks could be splitted in several chunks if needed (to save injector memory, since otherwise it cannot start sending a chunk as its size comes before data, and the last chunk may be shorter).  However, for the sake of simplicity, chunks should be aligned to block boundaries (i.e. blocks should consist of a natural number of chunks).

If a client got to get and save a full response from the injector, it may send to other clients the final response head straight away (i.e. skipping the partial signature or a trailer).

## Range requests

If a client sends an HTTP range request to another client, the later aligns it to block boundaries (this is acceptable according to [RFC7233#4.1][] — "a client cannot rely on receiving the same ranges that it requested").  The `Content-Range:` header in the response is not part of the partial nor final signatures.  If the range does not start at the beginning of the data, the first block `i` is accompanied with a `ouihash=BASE64(HASH[i-1])` chunk extension to enable checking its `ouisig`.  Please note that to ease serving range requests, a client storing a response may cache all chain hashes along their blocks, so as to avoid having to compute the `ouihash` of the first block in the range.

[RFC7233#4.1]: https://tools.ietf.org/html/rfc7233#section-4.1

Also note that partial responses mush have a `206 Partial Content` status, which would break signature verification.  To avoid this issue, the original response status code (usually `200`) is saved to the `X-Ouinet-HTTP-Status` header, and head verification should automatically replace the `206` status (if so allowed) with the saved one (only for verification purposes).

HTTP range requests from client to injector may not be supported since the injector would need to download all data from the beginning to compute an initial `ouihash`.  This could be abused to make the injector use resources by asking for the last block of a big file.  At any rate, in such an injection, `Digest:` and `X-Ouinet-Data-Size:` may be missing in the final response head, if the injector did not have access to the whole body data.  Also, the (aligned) `Content-Range:` header would never be signed to allow later sharing of different subranges, which can be validated independently anyway.

## HEAD requests

If a client sends an HTTP `HEAD` request to another client, the later includes in its response an `X-Ouinet-Avail-Range:` header showing the available stored data (in the same format as the `Content-Range:` header described in [RFC7233#4.2][]). Data is considered to be available from a client if (i) it is actually stored in the client's local storage, and (ii) data block signatures covering it are stored as well.

[RFC7233#4.2]: https://tools.ietf.org/html/rfc7233#section-4.2

For example, a client having completely stored the response shown above may reply with a head like the following one:

```
HTTP/1.1 200 OK
X-Ouinet-Version: 4
X-Ouinet-URI: https://example.com/foo
X-Ouinet-Injection: id=d6076384-2295-462b-a047-fe2c9274e58d,ts=1516048310
Date: Mon, 15 Jan 2018 20:31:50 GMT
Server: Apache
Content-Type: text/html
Content-Disposition: inline; filename="foo.html"
X-Ouinet-BSigs: keyId="ed25519=????",algorithm="hs2019",size=1048576
X-Ouinet-Data-Size: 1048580
X-Ouinet-Sig1: keyId="ed25519=????",algorithm="hs2019",created=1516048311,
  headers="(response-status) (created) x-ouinet-version x-ouinet-uri x-ouinet-injection x-ouinet-http-status date server content-type content-disposition x-ouinet-bsigs digest x-ouinet-data-size",
  signature="BASE64(...)"
X-Ouinet-Avail-Range: bytes 0-1048579/1048580
```

A client having received the head of such response from another client, but only its first two data blocks may include the following header instead:

```
X-Ouinet-Avail-Range: bytes 0-131071/1048580
```

In contrast, a client having received only a partial response from the injector (hence with an unknown total size) including only the first two data blocks may reply with:

```
HTTP/1.1 200 OK
X-Ouinet-Version: 4
X-Ouinet-URI: https://example.com/foo
X-Ouinet-Injection: id=d6076384-2295-462b-a047-fe2c9274e58d,ts=1516048310
Date: Mon, 15 Jan 2018 20:31:50 GMT
Server: Apache
Content-Type: text/html
Content-Disposition: inline; filename="foo.html"
X-Ouinet-BSigs: keyId="ed25519=????",algorithm="hs2019",size=1048576
X-Ouinet-Sig0: keyId="ed25519=????",algorithm="hs2019",created=1516048310,
  headers="(response-status) (created) x-ouinet-version x-ouinet-uri x-ouinet-injection x-ouinet-http-status date server content-type content-disposition x-ouinet-bsigs",
  signature="BASE64(...)"
X-Ouinet-Avail-Range: bytes 0-131071/*
```

If the client did not save any data blocks from the injector, the following header would be used instead (note that this deviates slightly from `Content-Range:` format):

```
X-Ouinet-Avail-Range: bytes */*
```

## Issues

  - Choose an adequate data block size (can use different ones according to `Content-Length:` from the origin).
  - This may only be usable for single ranges (i.e. no `multipart/byteranges` body).
  - Block hashes are outside of the signed HTTP head.  Inlining them in the final head may require long Base64-encoded headers for long files.
  - While several signers are supported via different `keyId`s in `X-Ouinet-Sig<N>:` headers, only one signer can provide a single signature for data blocks.  This is to avoid mismatching block sizes and having to link each block signature to a key.  However, other signers may still trust those signatures by covering the `X-Ouinet-BSigs:` header (which includes the public key for validation) in their signature.
