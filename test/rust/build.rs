use std::env;

fn main() {
    let include_dirs = env::var("INCLUDE_DIRS").unwrap_or_default();
    let include_dirs = include_dirs.split(",").filter(|s| !s.trim().is_empty());

    let lib_dirs = env::var("LIB_DIRS").unwrap_or_default();
    let lib_dirs = lib_dirs.split(",").filter(|s| !s.trim().is_empty());

    let libs = env::var("LIBS").unwrap_or_default();
    let libs = libs.split(",").filter(|s| !s.trim().is_empty());

    for dir in lib_dirs {
        println!("cargo:rustc-link-search=native={dir}");
        println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
    }

    for lib in libs {
        println!("cargo:rustc-link-lib={lib}");
    }

    cxx_build::bridge("src/lib.rs")
        .std("c++23")
        .include(".")
        .include("../../src")
        .includes(include_dirs)
        // Don't include asio *.ipp headers as we're using a separately built library
        .define("BOOST_ASIO_SEPARATE_COMPILATION", None)
        .file("cxx/bridge.cpp")
        .compile("rust-test-bridge");

    println!("cargo:rerun-if-env-changed=INCLUDE_DIRS");
    println!("cargo:rerun-if-env-changed=LIB_DIRS");
    println!("cargo:rerun-if-env-changed=LIBS");
    println!("cargo:rerun-if-changed=cxx/bridge.hpp");
    println!("cargo:rerun-if-changed=cxx/bridge.cpp");
}
