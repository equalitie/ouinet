mod record_processor;
mod runtime;

use crate::ffi::CxxRecordProcessor;
use cxx::UniquePtr;
use record_processor::RecordProcessor;
use state_monitor::{MonitoredValue, StateMonitor};
use std::sync::{Arc, Mutex};
use tokio::{
    fs,
    runtime::Runtime,
    sync::oneshot,
    task::JoinHandle,
    time::{self, Duration},
};
use uuid::Uuid;

#[cxx::bridge]
mod ffi {
    #[namespace = "ouinet::metrics::bridge"]
    extern "Rust" {
        //------------------------------------------------------------
        type Client;

        fn new_client(store_path: String, processor: UniquePtr<CxxRecordProcessor>) -> Box<Client>;
        fn new_mainline_dht(self: &Client) -> Box<MainlineDht>;

        //------------------------------------------------------------
        type MainlineDht;

        fn new_dht_node(self: &MainlineDht, ip_version: &str) -> Box<DhtNode>;

        //------------------------------------------------------------
        type DhtNode;

        fn new_bootstrap(self: &DhtNode) -> Box<Bootstrap>;

        //------------------------------------------------------------
        type Bootstrap;

        fn mark_success(self: &Bootstrap, wan_endpoint: String);

        //------------------------------------------------------------
        // Tells the rust code when record processing on the C++ side has finished.
        pub type CxxOneShotSender;
        fn send(self: &CxxOneShotSender, success: bool);
    }

    #[namespace = "ouinet::metrics::bridge"]
    unsafe extern "C++" {
        include!("cxx/record_processor.h");
        type CxxRecordProcessor;

        fn execute(
            self: &CxxRecordProcessor,
            record_name: String,
            record_data: String,
            on_finish: Box<CxxOneShotSender>,
        );
    }
}

unsafe impl Send for CxxRecordProcessor {}
unsafe impl Sync for CxxRecordProcessor {}

pub struct CxxOneShotSender {
    tx: Mutex<Option<oneshot::Sender<bool>>>,
}

impl CxxOneShotSender {
    fn new(tx: oneshot::Sender<bool>) -> Self {
        Self {
            tx: Mutex::new(Some(tx)),
        }
    }
    fn send(&self, success: bool) {
        self.tx
            .lock()
            .unwrap()
            .take()
            // This `send` function must not be used more than once.
            .unwrap()
            .send(success)
            .unwrap_or(());
    }
}

fn new_client(store_path: String, processor: UniquePtr<CxxRecordProcessor>) -> Box<Client> {
    let processor = RecordProcessor::new(processor);
    let uuid = Uuid::new_v4();
    let runtime = runtime::get_runtime();
    let monitor = Arc::new(StateMonitor::make_root());

    let job_handle = runtime.spawn({
        let monitor = monitor.clone();
        let record_name = format!("v0_{uuid}.record");
        let record_path = format!("{store_path}/{record_name}.record");

        async move {
            fs::create_dir_all(&store_path).await.unwrap();

            loop {
                time::sleep(Duration::from_secs(1)).await;
                let json = serde_json::to_string(&monitor_to_json(&monitor)).unwrap();
                tokio::fs::write(&record_path, json.clone()).await.unwrap();

                let Some(success) = processor.process(record_name.clone(), json).await else {
                    break;
                };
            }
        }
    });

    Box::new(Client {
        _runtime: runtime,
        monitor,
        inner: ClientInner::new(),
        job_handle,
    })
}

pub struct Client {
    _runtime: Arc<Runtime>,
    monitor: Arc<StateMonitor>,
    inner: Arc<ClientInner>,
    job_handle: JoinHandle<()>,
}

impl Drop for Client {
    fn drop(&mut self) {
        self.job_handle.abort();
    }
}

impl Client {
    fn new_mainline_dht(&self) -> Box<MainlineDht> {
        Box::new(MainlineDht {
            monitor: self.monitor.make_child("MainlineDht"),
            client_inner: self.inner.clone(),
        })
    }
}

struct ClientInner {
    bootstrap_state: Mutex<Option<MonitoredValue<String>>>,
}

impl ClientInner {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            bootstrap_state: Mutex::new(None),
        })
    }
}

pub struct MainlineDht {
    monitor: StateMonitor,
    client_inner: Arc<ClientInner>,
}

impl MainlineDht {
    fn new_dht_node(&self, ip_version: &str) -> Box<DhtNode> {
        Box::new(DhtNode {
            monitor: self.monitor.make_child(format!("DhtNode_{ip_version}")),
            client_inner: self.client_inner.clone(),
        })
    }
}

pub struct DhtNode {
    monitor: StateMonitor,
    client_inner: Arc<ClientInner>,
}

impl DhtNode {
    fn new_bootstrap(&self) -> Box<Bootstrap> {
        let state = self.monitor.make_value("bootstrap_state", "started".into());
        *self.client_inner.bootstrap_state.lock().unwrap() = Some(state.clone());
        Box::new(Bootstrap { state })
    }
}

pub struct Bootstrap {
    state: MonitoredValue<String>,
}

impl Bootstrap {
    fn mark_success(&self, wan_endpoint: String) {
        *self.state.get() = format!("boostrap successfull {wan_endpoint}");
    }
}

fn monitor_to_json(monitor: &StateMonitor) -> serde_json::Value {
    use serde_json::{json, Value};

    let value_names = monitor.values();
    let mut values = Vec::new();

    for value_name in value_names {
        if let Ok(value) = monitor.get_value::<String>(&value_name) {
            values.push(Value::String(value));
        }
    }

    let children_ids = monitor.children();
    let mut children = Vec::new();

    for child_id in children_ids {
        if let Some(child) = monitor.locate(Some(child_id)) {
            children.push(monitor_to_json(&child));
        }
    }

    json!({
        "values": values,
        "children": children,
    })
}
