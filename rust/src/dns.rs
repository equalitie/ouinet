use std::{net::IpAddr, sync::Arc};

use cxx::UniquePtr;
use ffi::IpAddress;
use hickory_resolver::{
    config::{NameServerConfigGroup, ResolverConfig, ResolverOpts},
    name_server::TokioConnectionProvider,
    proto::{ProtoError, ProtoErrorKind},
    IntoName, ResolveError, ResolveErrorKind, TokioResolver,
};

use crate::runtime;

#[cxx::bridge(namespace = "ouinet::dns::bridge")]
mod ffi {
    struct IpAddress {
        // IPv6 are stored as is, IPv4 as IPv6-mapped
        octets: [u8; 16],
    }

    enum Error {
        // Success
        Ok = 0,
        // No records found for the domain name
        NotFound = 1,
        // Name server(s) busy, try again later
        Busy = 2,
        // Other unspecified error
        Other = 3,
    }

    extern "Rust" {
        type Resolver;

        fn new_resolver() -> Box<Resolver>;
        fn resolve(&self, name: &str, completer: UniquePtr<BasicCompleter>);
    }

    unsafe extern "C++" {
        include!("cxx/dns.h");

        type BasicCompleter;

        fn complete(self: Pin<&mut BasicCompleter>, error: Error, addrs: Vec<IpAddress>);
    }
}

/// DNS resolver.
pub struct Resolver {
    inner: Arc<TokioResolver>,
}

impl Resolver {
    fn new() -> Self {
        // TODO: consider making the nameservers configurable
        let mut name_servers = NameServerConfigGroup::quad9_https();
        name_servers.merge(NameServerConfigGroup::cloudflare_https());
        name_servers.merge(NameServerConfigGroup::google_https());

        let inner = TokioResolver::builder_with_config(
            ResolverConfig::from_parts(None, vec![], name_servers),
            TokioConnectionProvider::default(),
        )
        // TODO: customize the options
        .with_options(ResolverOpts::default())
        .build();

        Self {
            inner: Arc::new(inner),
        }
    }

    /// Asynchronously resolve the given DNS name and invoke `completer` with the resolved ip
    /// addresses or error.
    fn resolve(&self, name: &str, mut completer: UniquePtr<ffi::BasicCompleter>) {
        let name = name.into_name();
        let inner = self.inner.clone();

        runtime::handle().spawn(async move {
            let completer = completer.pin_mut();

            let name = match name {
                Ok(name) => name,
                Err(error) => {
                    completer.complete(ffi::Error::from(&error), vec![]);
                    return;
                }
            };

            match inner.lookup_ip(name).await {
                Ok(lookup) => {
                    completer
                        .complete(ffi::Error::Ok, lookup.iter().map(IpAddress::from).collect());
                }
                Err(error) => {
                    completer.complete(ffi::Error::from(&error), vec![]);
                }
            }
        });
    }
}

fn new_resolver() -> Box<Resolver> {
    Box::new(Resolver::new())
}

impl From<IpAddr> for ffi::IpAddress {
    fn from(value: IpAddr) -> Self {
        match value {
            IpAddr::V4(addr) => ffi::IpAddress {
                octets: addr.to_ipv6_mapped().octets(),
            },
            IpAddr::V6(addr) => ffi::IpAddress {
                octets: addr.octets(),
            },
        }
    }
}

impl<'a> From<&'a ProtoError> for ffi::Error {
    fn from(value: &'a ProtoError) -> Self {
        match value.kind() {
            ProtoErrorKind::NoRecordsFound { .. } => ffi::Error::NotFound,
            ProtoErrorKind::Busy => ffi::Error::Busy,
            _ => ffi::Error::Other,
        }
    }
}

impl<'a> From<&'a ResolveError> for ffi::Error {
    fn from(value: &'a ResolveError) -> Self {
        match value.kind() {
            ResolveErrorKind::Proto(error) => Self::from(error),
            ResolveErrorKind::Message(_) | ResolveErrorKind::Msg(_) | _ => Self::Other,
        }
    }
}

unsafe impl Send for ffi::BasicCompleter {}
