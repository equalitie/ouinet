# Metrics record format

## Content

The metrics records are formated with JSON (comments not included).

```javascript
{
    // See for possible values:
    // https://doc.rust-lang.org/std/env/consts/constant.OS.html
    "os": <string>,
    // Hour when this record was collecting data
    // YYYY: Year
    // WW:   Week of the year (ISO 8601), starts from 1
    // DD:   Day of the week, starts from 1
    // HH:   Hour of the day, starts from 0
    "interval": "YYYY:WW:DD:HH",
    // When acting as a bridge, how many bytes have been transferred from other clients to the injector
    "bridge_c2i": <number>,
    // When acting as a bridge, how many bytes have been transferred from the injector to other clients
    "bridge_i2c": <number>,
    // Information about DHT bootstrapping.
    "bootstraps": {
        "histogram": {
            // Duration length of the first bucket in milliseconds
            "scale": <number>,
            // The key `i ∈ {0, ..., 9}` represents a duration interval `[(2^i - 1) * scale, (2^(i+1) - 1)  * scale)`
            // The value represents number of successful bootstraps withint the duration interval
            "buckets": Map<number, number>,
            // Count of bootstrap durations which did not fit into `buckets`
            "more": <number>
        },
        // Number of bootstrap attempts that have not finished yet
        "unfinished": <number>,
        // Minimum duration it took to bootstrap in milliseconds
        "min": <number>,
        // Maximum duration it took to bootstrap in milliseconds
        "max": <number>,
    },
    "requests": {
        "origin": {
            // Count of successful HTTP resource retrieval
            "success_count": <number>,
            // Count HTTP resource retrieval failures
            "failure_count": <number>,
            // Cumulative size of HTTP resource body transferred in bytes
            "transferred": <number>
        },
        "injector_private": {
            // ... As above
        },
        "injector_public": {
            // ... As above
        },
        "cache_in": {
            // ... As above
        },
        "cache_out": {
            // ... As above
        }
    },
    // Key/value pairs
    "aux": {
        "my_key1": "my_value1",
        "my_key2": "my_value2",
        ..
    },
}
```

### Writing key/value pairs to metric records

The `"aux"` object may store key/value string pairs provided by an app
utilizing Ouinet. The pair can be set using the front end as such:

```bash
front_end_ep="<FRONT_END_IP>:<FRONT_END_PORT>"
record_id=$(curl --fail --silent --show-error http://$front_end_ep/api/status | jq -r '.current_metrics_record_id')
curl --silent --show-error "http://$front_end_ep/api/metrics/set_key_value?record_id=$record_id&key=$key&value=$value"
```

The above command will succeed with `HTTP 200 OK` when metrics are enabled and
`record_id` is the ID of the record currently collecting metrics.

The `record_id` of the current record changes in predefined time intervals.
When the `record_id` is no longer (or never was) valid the `HTTP 409 Conflict`
status code is returned.

Whenever the current record changes, the previously stored key/value pairs are
_not_ copied to the new current record.

## Encryption

The records are encrypted before being stored or transmitted. It uses a public-key authenticated
encryption which combines the X25519 Diffie-Hellman function and the AES-256-GCM authenticated
encryption cipher.

### Ciphertext structure

(`N` be the length of the encrypted record in bytes)

| bytes            | description
| ---------------- | ----------------------------
| 0      .. 32     | X25519 public key (ephemeral)
| 32     .. N - 16 | AES-256-GCM ciphertext
| N - 16 .. N      | AES-256-GCM authentication tag

### Decryption

The first 32 bytes of the record is the ephemeral public key. Use it and the static private key to
compute the shared secret using Elliptic-curve Diffie-Hellman protocol. Use this shared secret to
decrypt the rest of the message using the AES-256-GCM scheme. Use nonce of all zeroes (the secret
key is unique per message so the nonce doesn't have to be).

#### Example

Example decryption code in go:

```go
package main

import (
    "crypto/aes"
    "crypto/cipher"
    "crypto/ecdh"
    "crypto/x509"
    "encoding/pem"
    "fmt"
    "io"
    "log"
    "os"
)

func main() {
    // Read the x25519 private key from a PEM file
    privateKeyData, err := os.ReadFile("private_key.pem")
    if err != nil {
        log.Panic(err)
    }

    block, _ := pem.Decode(privateKeyData)
    privateKeyAny, err := x509.ParsePKCS8PrivateKey(block.Bytes)
    if err != nil {
        log.Panic(err)
    }
    privateKey := privateKeyAny.(*ecdh.PrivateKey)

    // Read the encrypted message from the stdin
    payload, err := io.ReadAll(os.Stdin)
    if err != nil {
        log.Panic(err)
    }

    // The first 32 bytes of the message is the ephemeral public key
    curve := ecdh.X25519()

    ephemeralPublicKeyBytes := payload[0 : 32]
    ephemeralPublicKey, err := curve.NewPublicKey(ephemeralPublicKeyBytes)
    if err != nil {
        log.Panic(err)
    }

    // Compute the shared session key by running a Diffie-Hellman using the static private key and
    // the ephemeral public key
    sessionKey, err := privateKey.ECDH(ephemeralPublicKey)
    if err != nil {
        log.Panic(err)
    }

    // Initialize AES-256-GCM cipher
    cipherAlgo, err := aes.NewCipher(sessionKey)
    if err != nil {
        log.Panic(err)
    }
    cipher, err := cipher.NewGCM(cipherAlgo)
    if err != nil {
        log.Panic(err)
    }

    // The rest of the message is the ciphertext
    ciphertext := payload[32:]
    // Nonce doesn't matter because we use unique shared secret per message. Using all-zeroes.
    nonce := []byte{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
    plaintextBytes, err := cipher.Open(nil, nonce, ciphertext, []byte{})
    if err != nil {
        log.Panic(err)
    }
    plaintext := string(plaintextBytes)

    fmt.Println(plaintext)
}
```

### Keys generation

Example how to generate the encryption keys using openssl:

Generate private key:

```
openssl genpkey -algorithm x25519 -out private_key.pem
```

Extract public key:

```
openssl pkey -in private_key.pem -pubout -out public_key.pem
```

The private key is stored on the server consuming the records. The public key is distributed with
the clients.


