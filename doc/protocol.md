# Content signing using Merkle list

To support seeding of infinite HTTP streams and/or big files which have not
been downloaded in its entirety by any peer currently available, the protocol
can't rely on signing only the digest of the body that comes after the entire
body has been transmitted (because it may never arrive).

## Hash and signature calculation

```javascript
L00| head.injection_id := generate_uuid()               // This is the X-Ouinet-Injection hdr field
L01| head.signature    := SIGN(HASH(head.injection_id)) // Plus other fields that need signing
L02| 
L03| // i = 0
L04| block[0].list_hash := HASH(block[0].data)
L05| block[0].signature := SIGN(HASH("B" + 0 + head.injection_id + block[0].list_hash))
L06| 
L07| // i > 0
L08| block[i].list_hash := HASH(block[i-1].list_hash + block[i].data)
L09| block[i].signature := SIGN(HASH("B" + i + head.injection_id + block[i].list_hash))
L10| 
L13| trailer.final_signature := SIGN(HASH("T" + size + head.injection_id + block[size-1].list_hash))
```

Note that the only distinguishing factor between the trailer and a block with
empty `data` field is the "T" prefix in signature calculation.

## Block verification

We'll consider nodes A, B and C. The node C is the one trying to download a
resource from A and B.

Node C sends a HTTP HEAD request to nodes A and B for URI and receives the
following information:

```
L00| Client A has:
L01|   * Resource-URI: URI
L02|   * Injection ID: IDA
L03|   * Has N=50 blocks
L04|   * block[N-1].is_trailer ∈ Bool
L05|   * block[N-1].list_hash
L06|   * block[N-2].list_hash
L07| 
L08| Client B has:
L09|   * Resource-URI: URI // Same as for client A
L10|   * Injection ID: IDB
L11|   * Has N=(e.g. 100) blocks
L12|   * block[N-1].is_trailer ∈ Bool
L13|   * block[N-1].list_hash
L14|   * block[N-2].list_hash
```

Assuming both responses have valid signatures for the last block and that both
are "fresh" enough, C will start downloading blocks from node B becuase it has
more blocks.

C shall create an associative array which maps `injection_id` to the block
range that can be downloaded for this transaction. Given that C starts
downloading from B in this example, it will be initialized as so: `Map[IDB] =
B.N`.

C can download blocks from B in a random order, each block from B shall contain
(among other things) the following information:

```
L00| i: Block's sequence number
L01| B.block[i].injection_id
L02| B.block[i-1].list_hash ("" if i == 0) 
L03| B.block[i].data
```

As long as `r = Map[block[i].injection_id]` exists and `i < r`, C can use this
block.

In parallel, C shall send a HTTP HEAD or GET request to B for block A.N-1.
Once the integrity is checked as explained above, C can compare
`B.block[A.N-1].list_hash` with `A.block[A.N-1].list_hash`. If the two hashes
are the same, that proves that all data in blocks `A.blocks[0:A.N]` with
Injection ID: IDA are the same as data in blocks `B.block[0:A.N]`.

With this in mind node C may insert IDA and range [0:A.N] into the map of valid
ranges: `Map[IDA] = A.N`. This way, node C can download block in a *random
order* from *anyone* who can prove they have blocks[0:A.N] with `injection_id`
IDA or blocks[0:B.N] with `injection_id` IDB.

Other nodes can be contacted in this manner and `Map` can be extended to
include more Injection IDs and ranges.

## Client's cache entry selection

Please ignore this section for now. It's just some of my brain dumps I did not
want to erase yet.

```python
find_entries(async_entry_reader)
    timeout_in 15 seconds
    let entries = {}
    while e = async_entry_reader()
        entries.insert(e);
        if is_fresh(e)
            if is_from_LAN(e):    break
            if entries.size >= 2: break
            timeout_in min(2 seconds, time_remaining)
        else
            halve_timeout
    return sort_entries(entries)
```

```python
sort_entries(entries)
    define (a >= b) as:
        if both_are_fresh(a,b)
            if both_are_full(a,b)
                if both_are_on_LAN(a,b)
                    return age(a) >= age(b)
                if one_is_on_LAN(a,b)
                    return is_on_LAN(a)
                return peer_count(a) >= peer_count(b)
            if one_is_full(a,b)
                return is_full(a)
            return peer_count(a) >= peer_count(b)
        if one_is_fresh(a,b) 
            return is_fresh(a)
        return age(a) >= age(b)
    return sort(entries, >=)
```










































