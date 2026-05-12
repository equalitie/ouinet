#[test]
fn sanity_check() {
    let mut ctx = ouinet_test_rs::ffi::new_io_context();
    let _client = ouinet_test_rs::ffi::new_client(ctx.pin_mut(), &[]);

    // fff

    ctx.pin_mut().run();
}

// #[test]
// fn public_to_public() {
//     println!("public to public connectivity");
// }
