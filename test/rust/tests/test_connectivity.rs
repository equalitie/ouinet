use btdht::MainlineDht;
use ouinet_test_rs::{Client, Config, Context, Injector};
use patchbay::{Device, Lab, Router, RouterPreset};
use reqwest::{IntoUrl, RequestBuilder, Response, StatusCode};
use std::{
    fs,
    net::{Ipv4Addr, SocketAddr},
    time::Duration,
};
use tempfile::TempDir;
use tokio::{
    net::{TcpListener, UdpSocket},
    sync::oneshot,
    time,
};
use warp::Filter;

// Initialize user namespace for `patchbay`. This needs to run before anything else (especially
// before any threads are spawned). Using the `ctor` crate to achieve that.
#[ctor::ctor(unsafe)]
fn init() {
    unsafe {
        patchbay::init_userns_for_ctor();
    }
}

#[tokio::test]
async fn sanity_check() {
    let env = Env::create().await;

    // DHT swarm
    let dht_nodes = create_dht_nodes(&env.lab, 2).await;

    // origin server
    let content = "hello world";
    let origin = OriginServer::create(&env, content.to_owned()).await;

    let injector = InjectorHolder::create(&env).await;
    let seeder = ClientHolder::create(
        &env,
        "seeder",
        &injector.inner.cache_http_public_key(),
        Some(injector.addr()),
        vec![dht_nodes[0].addr()],
    )
    .await;

    let url = format!("http://{}/", origin.addr());

    // Make HTTP request proxied through the seeder to seed the cache
    let response = seeder
        .request(&url, |request| {
            request
                .version(reqwest::Version::HTTP_11)
                .header("X-Ouinet-Group", &url)
        })
        .await
        .unwrap();

    assert_eq!(response.status(), StatusCode::OK);
    assert_eq!(
        response
            .headers()
            .get("X-Ouinet-Source")
            .map(|v| v.to_str().unwrap()),
        Some("injector")
    );
    assert_eq!(response.text().await.unwrap(), content);

    let leecher = ClientHolder::create(
        &env,
        "leecher",
        &injector.inner.cache_http_public_key(),
        None,
        vec![dht_nodes[0].addr()],
    )
    .await;

    let response = loop {
        let response = leecher
            .request(&url, |request| {
                request
                    .version(reqwest::Version::HTTP_11)
                    .header("X-Ouinet-Group", &url)
            })
            .await
            .unwrap();

        match response.status() {
            StatusCode::OK => break response,
            StatusCode::BAD_GATEWAY => {
                time::sleep(Duration::from_millis(250)).await;
                continue;
            }
            code => panic!("Unexpected response status code: {:?}", code),
        }
    };

    assert_eq!(
        response
            .headers()
            .get("X-Ouinet-Source")
            .map(|v| v.to_str().unwrap()),
        Some("dist-cache")
    );
    assert_eq!(response.text().await.unwrap(), content);
}

// -----------------------------------------------------------------------------

struct Env {
    root_dir: TempDir,
    lab: Lab,
}

impl Env {
    async fn create() -> Self {
        tracing_subscriber::fmt::try_init().ok();

        let root_dir = TempDir::new().unwrap();
        let lab = Lab::new().await.unwrap();

        Self { root_dir, lab }
    }
}

// -----------------------------------------------------------------------------

struct OriginServer {
    #[expect(dead_code)]
    router: Router,
    device: Device,
}

impl OriginServer {
    async fn create(env: &Env, content: String) -> Self {
        let router = env
            .lab
            .add_router("origin-router")
            .preset(RouterPreset::Public)
            .build()
            .await
            .unwrap();

        let device = env
            .lab
            .add_device("origin")
            .iface("eth0", router.id())
            .build()
            .await
            .unwrap();

        device
            .spawn(async move |_device| {
                let socket = TcpListener::bind((Ipv4Addr::UNSPECIFIED, 80))
                    .await
                    .unwrap();
                let routes = warp::path::end()
                    .map(move || content.clone())
                    .with(warp::log("origin"));
                let server = warp::serve(routes).incoming(socket);
                server.run().await;
            })
            .unwrap();

        Self { router, device }
    }

    fn addr(&self) -> SocketAddr {
        (self.device.ip().unwrap(), 80).into()
    }
}

// -----------------------------------------------------------------------------

const DHT_BOOTSTRAP_PORT: u16 = 54321;

struct DhtNode {
    #[expect(dead_code)]
    router: Router,
    device: Device,
    dht: MainlineDht,
}

impl DhtNode {
    fn addr(&self) -> SocketAddr {
        (self.device.ip().unwrap(), DHT_BOOTSTRAP_PORT).into()
    }
}

async fn create_dht_nodes(lab: &Lab, count: usize) -> Vec<DhtNode> {
    assert!(count > 0);

    // All nodes are publicly reachable so we put them behind a single public router for simplicity.
    let router = lab
        .add_router("dht-router")
        .preset(RouterPreset::Public)
        .build()
        .await
        .unwrap();

    let mut devices = Vec::with_capacity(count);
    for i in 0..count {
        let device = lab
            .add_device(&format!("dht-node-{i}"))
            .iface("eth0", router.id())
            .build()
            .await
            .unwrap();

        devices.push(device);
    }

    let ips: Vec<_> = devices.iter().map(|device| device.ip().unwrap()).collect();

    let mut nodes = Vec::with_capacity(devices.len());
    for (i, device) in devices.into_iter().enumerate() {
        let socket = device
            .spawn(move |_device| UdpSocket::bind((Ipv4Addr::UNSPECIFIED, DHT_BOOTSTRAP_PORT)))
            .unwrap()
            .await
            .unwrap()
            .unwrap();

        let dht = ips
            .iter()
            .enumerate()
            .filter(|(j, _)| *j != i)
            .map(|(_, addr)| SocketAddr::from((*addr, DHT_BOOTSTRAP_PORT)))
            .fold(
                MainlineDht::builder().set_read_only(false),
                |builder, addr| builder.add_node(addr),
            )
            .start(socket)
            .unwrap();

        nodes.push(DhtNode {
            router: router.clone(),
            device,
            dht,
        });
    }

    for node in &nodes {
        assert!(node.dht.bootstrapped().await);
    }

    nodes
}

// -----------------------------------------------------------------------------

const INJECTOR_PORT: u16 = 52001;
const INJECTOR_CREDENTIALS: &str = "username:password";

struct InjectorHolder {
    inner: Injector,
    #[expect(dead_code)]
    router: Router,
    device: Device,
}

impl InjectorHolder {
    async fn create(env: &Env) -> Self {
        let name = "injector";
        let repo_dir = env.root_dir.path().join(name);
        let router = env
            .lab
            .add_router(&format!("{name}-router"))
            .preset(RouterPreset::Public)
            .build()
            .await
            .unwrap();
        let device = env
            .lab
            .add_device(name)
            .iface("eth0", router.id())
            .build()
            .await
            .unwrap();

        let (tx, rx) = oneshot::channel();
        device
            .spawn_thread(move || {
                fs::create_dir_all(&repo_dir).unwrap();

                let mut ctx = Context::new();
                let injector = Injector::new(
                    &mut ctx,
                    Config::new()
                        .arg("--repo", repo_dir.to_str().unwrap())
                        .arg("--log-level", "DEBUG")
                        .arg("--credentials", INJECTOR_CREDENTIALS)
                        .arg(
                            "--listen-on-utp",
                            SocketAddr::new(Ipv4Addr::UNSPECIFIED.into(), INJECTOR_PORT),
                        )
                        .flag("--bt-bootstrap-no-default"),
                    "injector",
                )
                .unwrap();

                tx.send(injector).unwrap();
                ctx.run();

                Ok(())
            })
            .unwrap();

        Self {
            inner: rx.await.unwrap(),
            router,
            device,
        }
    }

    fn addr(&self) -> SocketAddr {
        SocketAddr::new(self.device.ip().unwrap().into(), INJECTOR_PORT)
    }
}

// -----------------------------------------------------------------------------

struct ClientHolder {
    inner: Client,
    #[expect(dead_code)]
    router: Router,
    device: Device,
}

impl ClientHolder {
    async fn create(
        env: &Env,
        name: &str,
        cache_http_pub_key: &str,
        injector_addr: Option<SocketAddr>,
        bootstrap_addrs: Vec<SocketAddr>,
    ) -> Self {
        let repo_dir = env.root_dir.path().join(name);

        let router = env
            .lab
            .add_router(&format!("{name}-router"))
            .preset(RouterPreset::Home)
            .build()
            .await
            .unwrap();
        let device = env
            .lab
            .add_device(name)
            .iface("eth0", router.id())
            .build()
            .await
            .unwrap();

        let name = name.to_owned();
        let cache_http_pub_key = cache_http_pub_key.to_owned();

        let (tx, rx) = oneshot::channel();
        device
            .spawn_thread(move || {
                fs::create_dir_all(&repo_dir).unwrap();

                let config = Config::new()
                    .arg("--repo", repo_dir.to_str().unwrap())
                    .arg("--log-level", "DEBUG")
                    .arg("--cache-type", "bep5-http")
                    .arg("--cache-http-public-key", cache_http_pub_key)
                    .flag("--disable-origin-access")
                    .flag("--bt-bootstrap-no-default")
                    .arg(
                        "--listen-on-tcp",
                        SocketAddr::new(Ipv4Addr::UNSPECIFIED.into(), 0),
                    )
                    .arg(
                        "--front-end-ep",
                        SocketAddr::new(Ipv4Addr::UNSPECIFIED.into(), 0),
                    );

                let config = if let Some(addr) = injector_addr {
                    config
                        .arg("--injector-ep", format!("utp:{}", addr))
                        .arg("--injector-credentials", INJECTOR_CREDENTIALS)
                } else {
                    config.flag("--disable-injector-access")
                };

                let config = bootstrap_addrs.into_iter().fold(config, |config, addr| {
                    config.arg("--bt-bootstrap-extra", addr)
                });

                let mut ctx = Context::new();
                let mut client = Client::new(&mut ctx, config, &name).unwrap();

                client.start();

                tx.send(client).unwrap();
                ctx.run();

                Ok(())
            })
            .unwrap();

        Self {
            inner: rx.await.unwrap(),
            router,
            device,
        }
    }

    async fn request<F>(
        &self,
        url: impl IntoUrl,
        request_builder: F,
    ) -> Result<Response, reqwest::Error>
    where
        F: FnOnce(RequestBuilder) -> RequestBuilder,
    {
        let proxy_addr = self.inner.get_proxy_endpoint();
        let http_client = reqwest::Client::builder()
            .proxy(reqwest::Proxy::all(format!("http://{}", proxy_addr)).unwrap())
            .build()
            .unwrap();
        let request = request_builder(http_client.get(url));

        self.device
            .spawn(move |_device| request.send())
            .unwrap()
            .await
            .unwrap()
    }
}
