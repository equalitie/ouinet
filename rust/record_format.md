# Metrics record format

## Content

The metrics records are formated with JSON.

```json
{
    // See for possible values:
    // https://doc.rust-lang.org/std/env/consts/constant.OS.html
    "os": <string>,
    // Time when metrics were started (usually on app start) OR on DeviceID rotation
    "start": <string>,
    // Time when a this record started collecting
    "record_start": <string>,
    // When acting as a bridge, how many bytes have been transferred from other clients to the injector
    "bridge_c2i": <number>,
    // When acting as a bridge, how many bytes have been transferred from the injector to other clients
    "bridge_i2c": <number>,
    "bootstraps": {
        TODO...
    },
    "requests": {
        TODO...
    },
    "aux": {
        TODO...
    },
}
```

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


