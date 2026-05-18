use std::{
    ffi::{CString, c_char},
    fmt,
    net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr},
};

use cxx::UniquePtr;
use tokio::sync::oneshot;

#[cxx::bridge(namespace = "ouinet::test")]
mod ffi {
    // Intermediate type for converting between C++'s `tcp::endpoint` / `udp::endpoint` and rust's
    // `SocketAddr`
    struct SocketAddr {
        family: IpFamily,
        octets: [u8; 16],
        port: u16,
    }

    #[derive(Debug)]
    enum IpFamily {
        V4,
        V6,
    }

    extern "Rust" {
        type Completer;
        fn complete(self: &mut Completer);
        fn is_closed(self: &Completer) -> bool;
    }

    unsafe extern "C++" {
        include!("cxx/bridge.hpp");

        type Context;

        fn context_new() -> UniquePtr<Context>;
        fn run(self: Pin<&mut Context>) -> usize;

        type Client;

        fn client_new(
            ctx: Pin<&mut Context>,
            config: &[*const c_char],
            log_tag: &str,
        ) -> Result<UniquePtr<Client>>;
        fn start(self: Pin<&mut Client>);
        fn client_stop(client: UniquePtr<Client>, completer: Box<Completer>);
        fn client_get_proxy_endpoint(client: &Client) -> SocketAddr;

        type Injector;

        fn injector_new(
            ctx: Pin<&mut Context>,
            config: &[*const c_char],
            log_tag: &str,
        ) -> Result<UniquePtr<Injector>>;
        fn injector_stop(injector: UniquePtr<Injector>, completer: Box<Completer>);
        fn injector_cache_http_public_key(injector: &Injector) -> String;
        fn injector_tls_cert_file(injector: &Injector) -> String;
    }
}

impl From<ffi::SocketAddr> for SocketAddr {
    fn from(a: ffi::SocketAddr) -> Self {
        let ip = match a.family {
            ffi::IpFamily::V4 => {
                IpAddr::V4(Ipv4Addr::from_octets(*a.octets[..4].as_array().unwrap()))
            }
            ffi::IpFamily::V6 => IpAddr::V6(Ipv6Addr::from_octets(a.octets)),
            _ => panic!("invalid IP family: {:?}", a.family),
        };

        Self::from((ip, a.port))
    }
}

pub struct Context {
    inner: UniquePtr<ffi::Context>,
}

impl Context {
    pub fn new() -> Self {
        Self {
            inner: ffi::context_new(),
        }
    }

    // This function is blocking
    pub fn run(&mut self) {
        self.inner.pin_mut().run();
    }
}

pub struct Client {
    inner: Option<UniquePtr<ffi::Client>>,
}

impl Client {
    pub fn new(ctx: &mut Context, config: Config, log_tag: &str) -> Result<Self, anyhow::Error> {
        Ok(Self {
            inner: Some(ffi::client_new(ctx.inner.pin_mut(), &config.args, log_tag)?),
        })
    }

    pub fn start(&mut self) {
        self.inner.as_mut().unwrap().pin_mut().start();
    }

    /// Stops the client. Note dropping the Client also stops it, but calling this method is useful
    /// when one wants to wait until the stop completes.
    pub async fn stop(mut self) {
        if let Some(inner) = self.inner.take() {
            Completer::wait(|tx| ffi::client_stop(inner, Box::new(tx))).await;
        }
    }

    pub fn get_proxy_endpoint(&self) -> SocketAddr {
        ffi::client_get_proxy_endpoint(self.inner.as_ref().unwrap()).into()
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        if let Some(inner) = self.inner.take() {
            ffi::client_stop(inner, Box::new(Completer::detached()));
        }
    }
}

impl fmt::Debug for Client {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Client").finish_non_exhaustive()
    }
}

// SAFETY: All non thread-safe operations are invoked via `asio::post`.
unsafe impl Send for Client {}

pub struct Injector {
    inner: Option<UniquePtr<ffi::Injector>>,
}

impl Injector {
    pub fn new(ctx: &mut Context, config: Config, log_tag: &str) -> Result<Self, anyhow::Error> {
        Ok(Self {
            inner: Some(ffi::injector_new(
                ctx.inner.pin_mut(),
                &config.args,
                log_tag,
            )?),
        })
    }

    pub async fn stop(mut self) {
        if let Some(inner) = self.inner.take() {
            Completer::wait(|tx| ffi::injector_stop(inner, Box::new(tx))).await;
        }
    }

    pub fn cache_http_public_key(&self) -> String {
        ffi::injector_cache_http_public_key(self.inner.as_ref().unwrap())
    }

    pub fn tls_cert_file(&self) -> String {
        ffi::injector_tls_cert_file(self.inner.as_ref().unwrap())
    }
}

impl Drop for Injector {
    fn drop(&mut self) {
        if let Some(inner) = self.inner.take() {
            ffi::injector_stop(inner, Box::new(Completer::detached()));
        }
    }
}

impl fmt::Debug for Injector {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Injector").finish_non_exhaustive()
    }
}

// SAFETY: All non thread-safe operations are invoked via `asio::post`.
unsafe impl Send for Injector {}

pub struct Config {
    args: Vec<*const c_char>,
}

impl Config {
    pub fn new() -> Self {
        Self {
            // the 0-th arg needs to be the executable name. Use a dummy one here.
            args: vec![c"_".to_owned().into_raw()],
        }
    }

    pub fn flag(mut self, name: impl Into<String>) -> Self {
        self.args
            .push(CString::new(name.into()).unwrap().into_raw());
        self
    }

    pub fn arg(mut self, name: impl Into<String>, value: impl ToString) -> Self {
        self.args
            .push(CString::new(name.into()).unwrap().into_raw());
        self.args
            .push(CString::new(value.to_string()).unwrap().into_raw());
        self
    }
}

impl Drop for Config {
    fn drop(&mut self) {
        for arg in self.args.drain(..) {
            // SAFETY: The pointers were obtained from CString::into_raw
            unsafe {
                let _ = CString::from_raw(arg as *mut _);
            }
        }
    }
}

pub struct Completer {
    tx: Option<oneshot::Sender<()>>,
}

impl Completer {
    pub async fn wait<F>(f: F)
    where
        F: FnOnce(Self),
    {
        let (tx, rx) = oneshot::channel();
        let tx = Self { tx: Some(tx) };
        f(tx);

        rx.await.ok();
    }

    pub fn detached() -> Self {
        Self { tx: None }
    }

    pub fn complete(&mut self) {
        if let Some(tx) = self.tx.take() {
            tx.send(()).ok();
        }
    }

    pub fn is_closed(&self) -> bool {
        self.tx.as_ref().map(|tx| tx.is_closed()).unwrap_or(false)
    }
}
