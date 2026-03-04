use std::{fmt, future::Future, net::IpAddr, sync::Arc};

use cxx::UniquePtr;
use ffi::IpAddress;
use hickory_resolver::{
    config::{NameServerConfigGroup, ResolverConfig, ResolverOpts},
    name_server::TokioConnectionProvider,
    proto::{ProtoError, ProtoErrorKind},
    IntoName, ResolveError, ResolveErrorKind, TokioResolver,
};
use tokio::{select, sync::Notify, task::JoinSet};
use crate::dns::ffi::{Config, Protocol};
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

    enum Protocol {
        // Unencrypted DNS queries via UDP or TCP
        Plain = 0,
        // DNS over HTTPS
        Https = 1,
    }

    struct Config {
        protocols: Vec<Protocol>,
    }

    extern "Rust" {
        type Resolver;

        fn new_resolver(cfg: Config) -> Box<Resolver>;
        fn resolve(&mut self, name: &str, completer: UniquePtr<BasicCompleter>);
        fn str_to_proto(s: &str) -> Result<Protocol>;

        fn proto_to_str(p: Protocol) -> String;
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
fn str_to_proto(s: &str) -> Result<Protocol, ProtocolParseError> {
    match s {
        "plain" => Ok(Protocol::Plain),
        "https" => Ok(Protocol::Https),
        _ => Err(ProtocolParseError)
    }
}

fn proto_to_str(p: Protocol) -> String {
    match p {
        Protocol::Plain => "plain".to_string(),
        Protocol::Https => "https".to_string(),
        _ => "undefined".to_string(),
    }
}

struct ProtocolParseError;

impl fmt::Display for ProtocolParseError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "failed to parse protocol")
    }
}

/// DNS resolver.
pub struct Resolver {
    inner: Arc<TokioResolver>,
    tasks: JoinSet<()>,
}

impl Resolver {
    fn new(cfg: Config) -> Self {
        let mut name_servers: NameServerConfigGroup = NameServerConfigGroup::new();

        // TODO: consider making the nameservers configurable
        for proto in cfg.protocols {
            match proto {
                Protocol::Https => {
                    name_servers.merge(NameServerConfigGroup::quad9_https());
                    name_servers.merge(NameServerConfigGroup::cloudflare_https());
                    name_servers.merge(NameServerConfigGroup::google_https());
                }
                _ => { // Protocol::Plain
                    name_servers.merge(NameServerConfigGroup::quad9());
                    name_servers.merge(NameServerConfigGroup::cloudflare());
                    name_servers.merge(NameServerConfigGroup::google());
                }
            }
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

fn new_resolver(cfg: Config) -> Box<Resolver> {
    Box::new(Resolver::new(cfg))
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
