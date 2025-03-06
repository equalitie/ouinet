fn main() {
    cxx_build::bridge("src/bridge.rs")
        // Include C++/Rust bridge headers as `#include "cxx/foo.h".
        .include("./")
        // Include C++ Ouinet headers as `#include "bar.h".
        .include("../src")
        .include("../build/boost/install/include")
        .file("cxx/metrics.cpp")
        .file("cxx/record_processor.cpp")
        .std("c++17")
        .compile("rust-bridge");

    println!("cargo:rerun-if-changed=src/lib.rs");
    println!("cargo:rerun-if-changed=src/record_processor.rs");
    println!("cargo:rerun-if-changed=cxx/metrics.cpp");
    println!("cargo:rerun-if-changed=cxx/metrics.h");
    println!("cargo:rerun-if-changed=cxx/record_processor.cpp");
    println!("cargo:rerun-if-changed=cxx/record_processor.h");
}
