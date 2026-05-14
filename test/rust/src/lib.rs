use std::{
    net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr},
    pin::Pin,
};

#[cxx::bridge(namespace = "ouinet::test")]
pub mod ffi {

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

    unsafe extern "C++" {
        include!("cxx/bridge.hpp");

        pub type Context;

        pub fn new_context() -> UniquePtr<Context>;
        pub fn run(self: Pin<&mut Context>) -> usize;

        pub type Client;

        pub fn new_client(
            ctx: Pin<&mut Context>,
            config: Vec<String>,
            log_tag: &str,
        ) -> Result<UniquePtr<Client>>;

        pub fn start(self: Pin<&mut Client>);
        pub fn stop(self: Pin<&mut Client>);

        fn get_proxy_endpoint_raw(client: &Client) -> SocketAddr;
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

impl ffi::Client {
    pub fn get_proxy_endpoint(&self) -> SocketAddr {
        ffi::get_proxy_endpoint_raw(self).into()
    }

    /// Returns a RAII guard which stops the client on drop.
    pub fn stop_guard<'a>(self: Pin<&'a mut ffi::Client>) -> ClientStopGuard<'a> {
        ClientStopGuard(self)
    }
}

pub struct ClientStopGuard<'a>(Pin<&'a mut ffi::Client>);

impl Drop for ClientStopGuard<'_> {
    fn drop(&mut self) {
        self.0.as_mut().stop();
    }
}

// Safety: asio's io_context should be thread-safe.
unsafe impl Send for ffi::Context {}

pub struct ConfigBuilder {
    args: Vec<String>,
}

impl ConfigBuilder {
    pub fn new() -> Self {
        Self {
            // the 0-th arg needs to be the executable name. Use a dummy one here.
            args: vec!["_".to_owned()],
        }
    }

    pub fn flag(mut self, name: impl Into<String>) -> Self {
        self.args.push(name.into());
        self
    }

    pub fn arg(mut self, name: impl Into<String>, value: impl ToString) -> Self {
        self.args.push(name.into());
        self.args.push(value.to_string());
        self
    }

    pub fn build(self) -> Vec<String> {
        self.args
    }
}
