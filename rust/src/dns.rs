use std::{net::IpAddr, sync::Arc};

use cxx::UniquePtr;
use ffi::IpAddress;
use hickory_resolver::{
    config::{NameServerConfigGroup, ResolverConfig, ResolverOpts},
    name_server::TokioConnectionProvider,
    IntoName, TokioResolver,
};

use crate::runtime;

#[cxx::bridge(namespace = "ouinet::dns::bridge")]
mod ffi {
    struct IpAddress {
        // IPv6 are stored as is, IPv4 as IPv6-mapped
        octets: [u8; 16],
    }

    extern "Rust" {
        type Resolver;

        fn new_resolver() -> Box<Resolver>;
        fn resolve(&self, name: &str, completer: UniquePtr<Completer>);
    }

    unsafe extern "C++" {
        include!("cxx/dns.h");

        type Completer;

        fn on_success(&self, addrs: Vec<IpAddress>);
        fn on_failure(&self, error: String);
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
    fn resolve(&self, name: &str, completer: UniquePtr<ffi::Completer>) {
        let name = name.into_name();
        let inner = self.inner.clone();

        runtime::handle().spawn(async move {
            let name = match name {
                Ok(name) => name,
                Err(error) => {
                    completer.on_failure(error.to_string());
                    return;
                }
            };

            match inner.lookup_ip(name).await {
                Ok(lookup) => {
                    completer.on_success(lookup.iter().map(IpAddress::from).collect());
                }
                Err(error) => {
                    completer.on_failure(error.to_string());
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

unsafe impl Send for ffi::Completer {}
