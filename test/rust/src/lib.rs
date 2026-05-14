use std::{
    ffi::CString,
    net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr},
    pin::Pin,
};

use cxx::UniquePtr;
use tokio::{
    sync::oneshot,
    task::{self, JoinHandle},
};

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

        fn new_context() -> UniquePtr<Context>;
        fn run(self: Pin<&mut Context>) -> usize;

        type Client;

        fn new_client(
            ctx: Pin<&mut Context>,
            config: &[*const c_char],
            log_tag: &str,
        ) -> Result<UniquePtr<Client>>;
        fn start(self: Pin<&mut Client>);
        fn stop(client: Pin<&mut Client>, completer: Box<Completer>);
        fn get_proxy_endpoint(client: &Client) -> SocketAddr;
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
    run_handle: Option<JoinHandle<()>>,
}

impl Context {
    pub fn new() -> Self {
        Self {
            inner: ffi::new_context(),
            run_handle: None,
        }
    }

    pub fn run(&mut self) {
        if self.run_handle.is_some() {
            return;
        }

        let ptr = self.inner.as_mut_ptr();
        let ptr = ptr as usize;

        let run_handle = task::spawn_blocking(move || {
            let ptr = ptr as *mut ffi::Context;
            let ctx = unsafe { &mut *ptr };
            let ctx = unsafe { Pin::new_unchecked(ctx) };
            ctx.run();
        });

        self.run_handle = Some(run_handle);
    }

    pub fn is_running(&self) -> bool {
        self.run_handle.is_some()
    }

    pub async fn stopped(&mut self) {
        if let Some(handle) = self.run_handle.take() {
            handle.await.unwrap();
        }
    }
}

pub struct Client {
    inner: UniquePtr<ffi::Client>,
}

impl Client {
    pub fn new(ctx: &mut Context, config: Config, log_tag: &str) -> Result<Self, anyhow::Error> {
        let argv: Vec<_> = config.args.iter().map(|a| a.as_ptr()).collect();

        Ok(Self {
            inner: ffi::new_client(ctx.inner.pin_mut(), &argv, log_tag)?,
        })
    }

    pub fn start(&mut self) {
        self.inner.pin_mut().start();
    }

    pub async fn stop(&mut self) {
        let (tx, rx) = oneshot::channel();
        let tx = Completer { tx: Some(tx) };
        let tx = Box::new(tx);

        ffi::stop(self.inner.pin_mut(), tx);

        rx.await.ok();
    }

    pub fn get_proxy_endpoint(&self) -> SocketAddr {
        ffi::get_proxy_endpoint(&self.inner).into()
    }
}

pub struct Config {
    args: Vec<CString>,
}

impl Config {
    pub fn new() -> Self {
        Self {
            // the 0-th arg needs to be the executable name. Use a dummy one here.
            args: vec![c"_".into()],
        }
    }

    pub fn flag(mut self, name: impl Into<String>) -> Self {
        self.args.push(CString::new(name.into()).unwrap());
        self
    }

    pub fn arg(mut self, name: impl Into<String>, value: impl ToString) -> Self {
        self.args.push(CString::new(name.into()).unwrap());
        self.args.push(CString::new(value.to_string()).unwrap());
        self
    }
}

pub struct Completer {
    tx: Option<oneshot::Sender<()>>,
}

impl Completer {
    pub fn complete(&mut self) {
        if let Some(tx) = self.tx.take() {
            tx.send(()).ok();
        }
    }

    pub fn is_closed(&self) -> bool {
        self.tx.as_ref().map(|tx| tx.is_closed()).unwrap_or(true)
    }
}
