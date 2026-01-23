use std::{future::Future, net::IpAddr, sync::Arc};

use cxx::UniquePtr;
use ffi::IpAddress;
use hickory_resolver::{
    config::{NameServerConfigGroup, ResolverConfig, ResolverOpts},
    name_server::TokioConnectionProvider,
    proto::{ProtoError, ProtoErrorKind},
    IntoName, ResolveError, ResolveErrorKind, TokioResolver,
};
use tokio::{select, sync::Notify, task::JoinSet};

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

        fn new_resolver(doh: bool) -> Box<Resolver>;
        fn resolve(&mut self, name: &str, completer: UniquePtr<BasicCompleter>);
    }

    extern "Rust" {
        type CancellationToken;
        fn cancel(&self);
    }

    unsafe extern "C++" {
        include!("cxx/dns.h");

        type BasicCompleter;

        fn complete(self: Pin<&mut BasicCompleter>, error: Error, addrs: Vec<IpAddress>);
        fn on_cancel(self: Pin<&mut BasicCompleter>, token: Box<CancellationToken>);
    }
}

/// DNS resolver.
pub struct Resolver {
    inner: Arc<TokioResolver>,
    tasks: JoinSet<()>,
}

impl Resolver {
    fn new(doh: bool) -> Self {
        // TODO: consider making the nameservers configurable
        let mut name_servers:NameServerConfigGroup;
        if doh {
            name_servers = NameServerConfigGroup::quad9_https();
            name_servers.merge(NameServerConfigGroup::cloudflare_https());
            name_servers.merge(NameServerConfigGroup::google_https());
        } else {
            name_servers = NameServerConfigGroup::quad9();
            name_servers.merge(NameServerConfigGroup::cloudflare());
            name_servers.merge(NameServerConfigGroup::google());
        }

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

        let (cancellation_token, cancelled) = CancellationToken::new();
        completer.pin_mut().on_cancel(Box::new(cancellation_token));

        let cancellation_guard = CancellationGuard::new(completer);

        let task = async move {
            let name = match name {
                Ok(name) => name,
                Err(error) => {
                    cancellation_guard
                        .release()
                        .pin_mut()
                        .complete(ffi::Error::from(&error), vec![]);
                    return;
                }
            };

            let result = inner.lookup_ip(name).await;

            let mut completer = cancellation_guard.release();

            match result {
                Ok(lookup) => {
                    completer
                        .pin_mut()
                        .complete(ffi::Error::Ok, lookup.iter().map(IpAddress::from).collect());
                }
                Err(error) => {
                    completer
                        .pin_mut()
                        .complete(ffi::Error::from(&error), vec![]);
                }
            }
        };

        self.tasks.spawn_on(
            async move {
                select! {
                    _ = task => (),
                    _ = cancelled => (),
                }
            },
            runtime::handle(),
        );
    }
}

fn new_resolver(doh: bool) -> Box<Resolver> {
    Box::new(Resolver::new(doh))
}

// Cancels the operation if cancellation has been triggered by the caller.
struct CancellationToken(Arc<Notify>);

impl CancellationToken {
    fn new() -> (Self, impl Future<Output = ()>) {
        let notify = Arc::new(Notify::new());
        let notified = notify.clone().notified_owned();

        (Self(notify), notified)
    }

    fn cancel(&self) {
        self.0.notify_waiters();
    }
}

// Triggers the completer with `Error::Cancelled` if the task has been dropped before completion.
struct CancellationGuard {
    completer: Option<UniquePtr<ffi::BasicCompleter>>,
}

impl CancellationGuard {
    fn new(completer: UniquePtr<ffi::BasicCompleter>) -> Self {
        Self {
            completer: Some(completer),
        }
    }

    fn release(mut self) -> UniquePtr<ffi::BasicCompleter> {
        self.completer.take().unwrap()
    }
}

impl Drop for CancellationGuard {
    fn drop(&mut self) {
        if let Some(mut completer) = self.completer.take() {
            completer.pin_mut().complete(ffi::Error::Cancelled, vec![]);
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
