use ouinet_test_rs::{CacheType, ClientBuilder, ffi};
use std::{fs, net::Ipv4Addr};

use tempfile::TempDir;

#[test]
fn sanity_check() {
    let root_dir = TempDir::new().unwrap();
    let repo_dir = root_dir.path().join("client");
    fs::create_dir_all(&repo_dir).unwrap();

    let mut ctx = ffi::new_context();
    let mut client = ClientBuilder::new()
        .repo(&repo_dir)
        .cache_type(CacheType::None)
        .listen_on_tcp((Ipv4Addr::LOCALHOST, 0).into())
        .front_end_ep((Ipv4Addr::LOCALHOST, 0).into())
        .build(ctx.pin_mut())
        .unwrap();

    client.pin_mut().start();

    ctx.pin_mut().run();
}

// #[test]
// fn public_to_public() {
//     println!("public to public connectivity");
// }
