use std::{net::IpAddr, pin::Pin, sync::Arc};

use cxx::UniquePtr;
use ffi::IpAddress;
use hickory_resolver::{
    config::{NameServerConfigGroup, ResolverConfig, ResolverOpts},
    name_server::TokioConnectionProvider,
    proto::{ProtoError, ProtoErrorKind},
    IntoName, ResolveError, ResolveErrorKind, TokioResolver,
};
use tokio::task::JoinSet;

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
        // Operation cancelled
        Cancelled = 3,
        // Other unspecified error
        Other = 4,
    }

    extern "Rust" {
        type Resolver;

        fn new_resolver() -> Box<Resolver>;
        fn resolve(&mut self, name: &str, completer: UniquePtr<BasicCompleter>);
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
    tasks: JoinSet<()>,
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
            tasks: JoinSet::new(),
        }
    }

    /// Asynchronously resolve the given DNS name and invoke `completer` with the resolved ip
    /// addresses or error.
    fn resolve(&mut self, name: &str, mut completer: UniquePtr<ffi::BasicCompleter>) {
        let name = name.into_name();
        let inner = self.inner.clone();

        // Clean up completed tasks
        while self.tasks.try_join_next().is_some() {}

        self.tasks.spawn_on(
            async move {
                let completer = completer.pin_mut();

                let name = match name {
                    Ok(name) => name,
                    Err(error) => {
                        completer.complete(ffi::Error::from(&error), vec![]);
                        return;
                    }
                };

                let cancel_guard = CancelGuard::new(completer);
                let result = inner.lookup_ip(name).await;
                let completer = cancel_guard.release();

                match result {
                    Ok(lookup) => {
                        completer
                            .complete(ffi::Error::Ok, lookup.iter().map(IpAddress::from).collect());
                    }
                    Err(error) => {
                        completer.complete(ffi::Error::from(&error), vec![]);
                    }
                }
            },
            runtime::handle(),
        );
    }
}

fn new_resolver() -> Box<Resolver> {
    Box::new(Resolver::new())
}

struct CancelGuard<'a> {
    completer: Option<Pin<&'a mut ffi::BasicCompleter>>,
}

impl<'a> CancelGuard<'a> {
    fn new(completer: Pin<&'a mut ffi::BasicCompleter>) -> Self {
        Self {
            completer: Some(completer),
        }
    }

    fn release(mut self) -> Pin<&'a mut ffi::BasicCompleter> {
        self.completer.take().unwrap()
    }
}

impl<'a> Drop for CancelGuard<'a> {
    fn drop(&mut self) {
        if let Some(completer) = self.completer.take() {
            completer.complete(ffi::Error::Cancelled, vec![]);
        }
    }
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
