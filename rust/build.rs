fn main() {
    cxx_build::bridge("src/bridge.rs")
        // Include C++/Rust bridge headers as `#include "cxx/foo.h".
        .include("./")
        // Include C++ Ouinet headers as `#include "bar.h".
        .include("../src")
        // Don't include asio *.ipp headers as we're using a separately built library
        .define("BOOST_ASIO_SEPARATE_COMPILATION", None)
        .file("cxx/metrics.cpp")
        .file("cxx/record_processor.cpp")
        .std("c++20")
        .compile("rust-bridge");

    println!("cargo:rerun-if-changed=src");
    println!("cargo:rerun-if-changed=cxx");
    println!("cargo:rerun-if-env-changed=CXXFLAGS");
}
