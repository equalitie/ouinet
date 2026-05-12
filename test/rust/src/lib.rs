#[cxx::bridge(namespace = "ouinet::test")]
pub mod ffi {
    unsafe extern "C++" {
        include!("cxx/bridge.hpp");

        pub type IoContext;

        pub fn new_io_context() -> UniquePtr<IoContext>;
        pub fn run(self: Pin<&mut IoContext>) -> usize;

        pub type Client;

        pub fn new_client(ctx: Pin<&mut IoContext>, config: &[&str]) -> UniquePtr<Client>;
        pub fn start(self: Pin<&mut Client>);
        // pub fn stop(self: Pin<&mut Client>);
    }
}
