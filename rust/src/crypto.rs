//! Cryptography utilities

use aes_gcm::{aead::AeadMutInPlace, Aes256Gcm, KeyInit, Nonce};
use rand::CryptoRng;
use thiserror::Error;
use x25519_dalek::EphemeralSecret;

/// Static public key for asymmetric encryption
#[derive(Clone, Copy)]
pub struct PublicKey(x25519_dalek::PublicKey);

impl PublicKey {
    const SIZE: usize = 32;

    /// Parse the public key from the content of a PEM file
    pub fn from_pem(src: &str) -> Result<Self, KeyParseError> {
        use pkcs8::{der::asn1::BitString, der::Any, Document, SubjectPublicKeyInfo};

        let (_, doc) = Document::from_pem(src)?;
        let info = SubjectPublicKeyInfo::<Any, BitString>::try_from(doc.as_bytes())?;
        let bytes: [u8; Self::SIZE] = info
            .subject_public_key
            .raw_bytes()
            .try_into()
            .map_err(|_| KeyParseError::InvalidKeyLength)?;

        Ok(Self::from(bytes))
    }
}

impl From<[u8; Self::SIZE]> for PublicKey {
    fn from(bytes: [u8; Self::SIZE]) -> Self {
        Self(bytes.into())
    }
}

pub(crate) struct SecretKey(x25519_dalek::StaticSecret);

impl SecretKey {
    #[cfg_attr(not(test), expect(dead_code))]
    pub(crate) fn random<R: CryptoRng>(rng: &mut R) -> Self {
        use rand::Rng;

        // TODO: There is `StaticSecret::random_from_rng` but it requires an old version of rand.
        // When `x25519_dalek` is updated to the latest version, we can switch.
        let bytes: [u8; 32] = rng.random();

        Self(x25519_dalek::StaticSecret::from(bytes))
    }
}

impl<'a> From<&'a SecretKey> for PublicKey {
    fn from(sk: &'a SecretKey) -> Self {
        PublicKey(x25519_dalek::PublicKey::from(&sk.0))
    }
}

#[derive(Debug, Error)]
pub enum KeyParseError {
    #[error("failed to parse pem")]
    ParsePem(#[from] pkcs8::der::Error),
    #[error("failed to extract key")]
    ExtractKey(#[from] pkcs8::spki::Error),
    #[error("invalid key length")]
    InvalidKeyLength,
}

#[derive(Debug, Error)]
#[error("failed to encrypt message")]
pub struct EncryptError;

/// Encrypt the given plaintext using a public-key authenticated encryption which combines the
/// X25519 Diffie-Hellman function and the AES-256-GCM authenticated encryption cipher.
///
/// The output has the following structure (N is the output length):
///
///     | bytes        | description
///     +--------------+-------------------------------
///     | 0    .. 32   | x25519 ehemeral public key
///     | 32   .. N-16 | AES-256-GCM ciphertext
///     | N-16 .. N    | AEC-256-GCM authentication tag
///
/// Note: because unique ephemeral key is used each time this function is invoked, it's not
/// necessary to also use unique nonce. For this reason and to avoid having to transmit the nonce
/// with the message, a fixed, all-zeroes nonce is used.
///
/// To decrypt the message, extract the ephemeral public key from the first 32 bytes of the message,
/// perform a diffie-hellman with the static secret key and this public key to obtain the shared
/// secret, then decrypt the rest of the message using the AES-256-GCM scheme with the previously
/// obtained shared secret and a zero nonce.
pub fn encrypt(static_public_key: &PublicKey, plaintext: &[u8]) -> Result<Vec<u8>, EncryptError> {
    let ephemeral_secret_key = EphemeralSecret::random();
    let ephemeral_public_key = x25519_dalek::PublicKey::from(&ephemeral_secret_key);

    let shared_secret = ephemeral_secret_key.diffie_hellman(&static_public_key.0);
    let mut cipher = Aes256Gcm::new(shared_secret.as_bytes().into());

    let mut output = Vec::new();
    output.extend_from_slice(ephemeral_public_key.as_bytes());
    output.extend_from_slice(plaintext);

    // We use unique key for every message so there is no need to have unique nonce as well. Using
    // 0000... so that we don't have to embed the nonce with the message.
    let tag = cipher
        .encrypt_in_place_detached(&Nonce::default(), &[], &mut output[PublicKey::SIZE..])
        .map_err(|_| EncryptError)?;

    output.extend_from_slice(&tag);

    Ok(output)
}

#[cfg(test)]
mod tests {
    use aes_gcm::aead::Aead;
    use rand::{
        distr::{Alphanumeric, SampleString},
        rngs::StdRng,
        Rng, SeedableRng,
    };

    use super::*;

    #[test]
    fn roundtrip() {
        let mut rng = StdRng::seed_from_u64(0);
        let n = 32;

        for _ in 0..n {
            let static_secret_key = SecretKey::random(&mut rng);
            let static_public_key = PublicKey::from(&static_secret_key);

            let plaintext_len = rng.random_range(1..=1024);
            let plaintext = Alphanumeric.sample_string(&mut rng, plaintext_len);

            let ciphertext = encrypt(&static_public_key, plaintext.as_bytes()).unwrap();

            let roundtrip = decrypt(&static_secret_key, &ciphertext).unwrap();
            let roundtrip = String::from_utf8(roundtrip).unwrap();

            assert_eq!(plaintext, roundtrip);
        }
    }

    #[derive(Debug)]
    struct DecryptError;

    fn decrypt(static_secret_key: &SecretKey, ciphertext: &[u8]) -> Result<Vec<u8>, DecryptError> {
        let ephemeral_public_key: [u8; 32] =
            ciphertext[..32].try_into().map_err(|_| DecryptError)?;
        let ephemeral_public_key = x25519_dalek::PublicKey::from(ephemeral_public_key);

        let shared_secret = static_secret_key.0.diffie_hellman(&ephemeral_public_key);
        let cipher = Aes256Gcm::new(shared_secret.as_bytes().into());

        cipher
            .decrypt(&Nonce::default(), &ciphertext[32..])
            .map_err(|_| DecryptError)
    }
}
