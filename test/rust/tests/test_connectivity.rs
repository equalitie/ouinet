use ouinet_test_rs::{ConfigBuilder, ffi};
use std::{
    fs,
    net::{Ipv4Addr, SocketAddr},
};
use tempfile::TempDir;
use tokio::{net::TcpListener, task};
use warp::Filter;

#[tokio::test]
async fn sanity_check() {
    env_logger::init();

    // Create ouinet client
    let root_dir = TempDir::new().unwrap();
    let repo_dir = root_dir.path().join("client");
    fs::create_dir_all(&repo_dir).unwrap();

    let mut ctx = ffi::new_context();
    let mut client = ffi::new_client(
        ctx.pin_mut(),
        ConfigBuilder::new()
            .arg("--repo", repo_dir.to_str().unwrap())
            .arg("--log-level", "DEBUG")
            .arg("--cache-type", "none")
            .arg(
                "--listen-on-tcp",
                SocketAddr::from((Ipv4Addr::LOCALHOST, 0)),
            )
            .arg("--front-end-ep", SocketAddr::from((Ipv4Addr::LOCALHOST, 0)))
            .flag("--bt-bootstrap-no-default")
            .build(),
        "client",
    )
    .unwrap();

    client.pin_mut().start();

    let proxy_addr = client.get_proxy_endpoint();
    let _client_stop_guard = client.pin_mut().stop_guard();

    task::spawn_blocking(move || {
        ctx.pin_mut().run();
    });

    // HTTP server
    let content = "hello world";
    let server_addr = spawn_http_server(content.to_owned()).await;

    // HTTP client proxied through the ouinet client
    let http_client = reqwest::Client::builder()
        .proxy(reqwest::Proxy::all(format!("http://{}", proxy_addr)).unwrap())
        .build()
        .unwrap();

    let response = http_client
        .get(format!("http://{}/", server_addr))
        .version(reqwest::Version::HTTP_11)
        .header(reqwest::header::USER_AGENT, "ouinet rust test")
        .send()
        .await
        .unwrap();

    assert_eq!(response.status(), reqwest::StatusCode::OK);
    assert_eq!(
        response
            .headers()
            .get("X-Ouinet-Source")
            .map(|v| v.to_str().unwrap()),
        Some("origin")
    );
    assert_eq!(response.text().await.unwrap(), content);
}

async fn spawn_http_server(content: String) -> SocketAddr {
    let socket = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
    let addr = socket.local_addr().unwrap();

    let routes = warp::path::end().map(move || content.clone());
    let server = warp::serve(routes).incoming(socket);
    task::spawn(server.run());

    addr
}
