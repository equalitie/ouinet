use std::{
    borrow::Cow,
    ffi::{CStr, CString, c_char},
    fmt, iter,
    net::SocketAddr,
    path::PathBuf,
    pin::Pin,
};

use cxx::{Exception, UniquePtr};

#[cxx::bridge(namespace = "ouinet::test")]
pub mod ffi {
    unsafe extern "C++" {
        include!("cxx/bridge.hpp");

        pub type Context;

        pub fn new_context() -> UniquePtr<Context>;
        pub fn run(self: Pin<&mut Context>) -> usize;

        pub type Client;

        pub fn new_client(
            ctx: Pin<&mut Context>,
            argv: &[*const c_char],
            log_tag: &str,
        ) -> Result<UniquePtr<Client>>;

        pub fn start(self: Pin<&mut Client>);
        // pub fn stop(self: Pin<&mut Client>);
    }
}

#[derive(Default)]
pub struct ClientBuilder {
    repo: Option<PathBuf>,
    cache_type: CacheType,
    listen_on_tcp: Option<SocketAddr>,
    front_end_ep: Option<SocketAddr>,
    log_tag: Option<String>,
}

impl ClientBuilder {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn repo(self, value: impl Into<PathBuf>) -> Self {
        Self {
            repo: Some(value.into()),
            ..self
        }
    }

    pub fn cache_type(self, value: CacheType) -> Self {
        Self {
            cache_type: value,
            ..self
        }
    }

    pub fn listen_on_tcp(self, value: SocketAddr) -> Self {
        Self {
            listen_on_tcp: Some(value),
            ..self
        }
    }

    pub fn front_end_ep(self, value: SocketAddr) -> Self {
        Self {
            front_end_ep: Some(value),
            ..self
        }
    }

    pub fn log_tag(self, value: &str) -> Self {
        Self {
            log_tag: Some(value.to_owned()),
            ..self
        }
    }

    pub fn build(self, ctx: Pin<&mut ffi::Context>) -> Result<UniquePtr<ffi::Client>, Exception> {
        fn entry<T: ToString + ?Sized>(name: &'static CStr, value: &T) -> [Cow<'static, CStr>; 2] {
            [name.into(), CString::new(value.to_string()).unwrap().into()]
        }

        // The 0-th argument must be the executable name. Use a dummy one here.
        let options: Vec<Cow<'static, CStr>> = iter::once(c"_".into())
            .chain(
                self.repo
                    .as_ref()
                    .into_iter()
                    .flat_map(|v| entry(c"--repo", v.as_path().to_str().unwrap())),
            )
            .chain(
                self.listen_on_tcp
                    .as_ref()
                    .into_iter()
                    .flat_map(|v| entry(c"--listen-on-tcp", v)),
            )
            .chain(
                self.front_end_ep
                    .as_ref()
                    .into_iter()
                    .flat_map(|v| entry(c"--front-end-ep", v)),
            )
            .chain(entry(c"--cache-type", &self.cache_type))
            .collect();

        let options: Vec<*const c_char> = options.iter().map(|item| item.as_ptr()).collect();

        ffi::new_client(ctx, &options, self.log_tag.as_deref().unwrap_or("client"))
    }
}

#[derive(Default)]
pub enum CacheType {
    #[default]
    None,
    Bep5Http,
    Bep3HttpOverI2p,
    Ouisync,
}

impl fmt::Display for CacheType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::None => write!(f, "none"),
            Self::Bep5Http => write!(f, "beb5-http"),
            Self::Bep3HttpOverI2p => write!(f, "bep3-http-over-i2p"),
            Self::Ouisync => write!(f, "ouisync"),
        }
    }
}
