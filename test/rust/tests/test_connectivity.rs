use ouinet_test_rs::{Client, Config, Context, Injector};
use std::{
    fs,
    net::{Ipv4Addr, SocketAddr},
};
use tempfile::TempDir;
use tokio::{
    net::{TcpListener, UdpSocket},
    sync::oneshot,
    task,
};
use warp::Filter;

#[tokio::test]
async fn sanity_check() {
    env_logger::init();

    let root_dir = TempDir::new().unwrap();

    let dht_node_socket = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
    let dht_node_addr = dht_node_socket.local_addr().unwrap();
    let dht_node = btdht::MainlineDht::builder()
        .start(dht_node_socket)
        .unwrap();
    assert!(dht_node.bootstrapped().await);

    let injector_dir = root_dir.path().join("injector");
    let injector_credentials = "username:password";

    // Create ouinet injector
    let (injector_tx, injector_rx) = oneshot::channel();
    task::spawn_blocking(move || {
        fs::create_dir_all(&injector_dir).unwrap();

        let mut ctx = Context::new();
        let injector = Injector::new(
            &mut ctx,
            Config::new()
                .arg("--repo", injector_dir.to_str().unwrap())
                .arg("--credentials", injector_credentials)
                .arg("--log-level", "DEBUG")
                .arg("--bt-bootstrap-extra", dht_node_addr.to_string())
                .flag("--bt-bootstrap-no-default"),
            "injector",
        )
        .unwrap();

        injector_tx.send(injector).unwrap();
        ctx.run();
    });

    let mut injector = injector_rx.await.unwrap();
    let injector_http_public_key = injector.cache_http_public_key();
    let injector_tls_cert_file = injector.tls_cert_file();

    // Create ouinet client
    let (client_tx, client_rx) = oneshot::channel();
    let client_dir = root_dir.path().join("client");
    task::spawn_blocking(move || {
        fs::create_dir_all(&client_dir).unwrap();

        let mut ctx = Context::new();
        let mut client = Client::new(
            &mut ctx,
            Config::new()
                .arg("--repo", client_dir.to_str().unwrap())
                .arg("--log-level", "DEBUG")
                .arg("--injector-credentials", injector_credentials)
                .arg("--cache-type", "bep5-http")
                .arg("--cache-http-public-key", injector_http_public_key)
                .arg("--injector-tls-cert-file", injector_tls_cert_file)
                .flag("--disable-origin-access")
                .arg(
                    "--listen-on-tcp",
                    SocketAddr::from((Ipv4Addr::LOCALHOST, 0)),
                )
                .arg("--front-end-ep", SocketAddr::from((Ipv4Addr::LOCALHOST, 0)))
                .flag("--bt-bootstrap-no-default")
                .arg("--bt-bootstrap-extra", dht_node_addr.to_string()),
            "client",
        )
        .unwrap();

        client.start();

        client_tx.send(client).unwrap();
        ctx.run();
    });

    let mut client = client_rx.await.unwrap();
    let proxy_addr = client.get_proxy_endpoint();

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
