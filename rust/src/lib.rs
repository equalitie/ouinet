mod backoff;
mod clock;
mod constants;
mod metrics;
mod metrics_runner;
mod record_number;
mod record_processor;
mod runtime;
mod store;
mod uuid_rotator;

use crate::{
    ffi::CxxRecordProcessor, metrics_runner::MetricsRunnerError,
    record_processor::RecordProcessorError,
};
use cxx::UniquePtr;
use metrics::{IpVersion, Metrics};
use metrics_runner::metrics_runner;
use std::{
    path::PathBuf,
    sync::{Arc, Mutex, Weak},
};
use tokio::{runtime::Runtime, sync::oneshot, task::JoinHandle};

#[cxx::bridge]
mod ffi {
    #[namespace = "ouinet::metrics::bridge"]
    extern "Rust" {
        //------------------------------------------------------------
        type Client;

        fn new_client(store_path: String, processor: UniquePtr<CxxRecordProcessor>) -> Box<Client>;
        fn new_noop_client() -> Box<Client>;
        fn new_mainline_dht(self: &Client) -> Box<MainlineDht>;

        //------------------------------------------------------------
        type MainlineDht;

        fn new_dht_node(self: &MainlineDht, is_ipv4: bool) -> Box<DhtNode>;

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

struct Session {
    job_handle: JoinHandle<()>,
}

impl Drop for Session {
    fn drop(&mut self) {
        // TODO: Send signal to metrics to store the metrics on disk and give it a little timeout
        // to finish.
        self.job_handle.abort();
    }
}

static SESSION: Mutex<Option<Session>> = Mutex::new(None);

// Create a new Client, if there were any clients created but not destroyed beforehand, all their
// operations will be no-ops.
fn new_client(store_path: String, processor: UniquePtr<CxxRecordProcessor>) -> Box<Client> {
    let runtime = runtime::get_runtime();

    let mut session_lock = SESSION.lock().unwrap();
    session_lock.take();

    let metrics = Arc::new(Metrics::new());
    let weak_metrics = Arc::downgrade(&metrics);

    let processor = record_processor::RecordProcessor::new(processor);

    let job_handle = runtime.spawn(async move {
        let store_path = PathBuf::from(store_path);

        if let Err(error) = metrics_runner(metrics, store_path, processor).await {
            match error {
                MetricsRunnerError::RecordProcessor(RecordProcessorError::CxxDisconnected) => (),
                MetricsRunnerError::Io(error) => {
                    log::error!("Metrics runner finished with an error: {error:?}")
                }
            }
        }
    });

    *session_lock = Some(Session { job_handle });

    Box::new(Client {
        _runtime: Some(runtime),
        metrics: weak_metrics,
    })
}

fn new_noop_client() -> Box<Client> {
    SESSION.lock().unwrap().take();

    Box::new(Client {
        _runtime: None,
        metrics: Weak::new(),
    })
}

// -------------------------------------------------------------------

pub struct Client {
    _runtime: Option<Arc<Runtime>>,
    metrics: Weak<Metrics>,
}

impl Drop for Client {
    fn drop(&mut self) {
        SESSION.lock().unwrap().take();
    }
}

impl Client {
    fn new_mainline_dht(&self) -> Box<MainlineDht> {
        Box::new(MainlineDht {
            metrics: self.metrics.clone(),
        })
    }
}

// -------------------------------------------------------------------

pub struct MainlineDht {
    metrics: Weak<Metrics>,
}

impl MainlineDht {
    fn new_dht_node(&self, is_ipv4: bool) -> Box<DhtNode> {
        let ipv = if is_ipv4 {
            IpVersion::V4
        } else {
            IpVersion::V6
        };

        Box::new(DhtNode {
            ipv,
            metrics: self.metrics.clone(),
        })
    }
}

// -------------------------------------------------------------------

pub struct DhtNode {
    ipv: IpVersion,
    metrics: Weak<Metrics>,
}

impl DhtNode {
    fn new_bootstrap(&self) -> Box<Bootstrap> {
        Box::new(Bootstrap::new(self.ipv, self.metrics.clone()))
    }
}

// -------------------------------------------------------------------

pub struct Bootstrap {
    ipv: IpVersion,
    inner: Option<BootstrapInner>,
}

struct BootstrapInner {
    bootstrap_id: usize,
    metrics: Weak<Metrics>,
}

impl Bootstrap {
    fn new(ipv: IpVersion, metrics_weak: Weak<Metrics>) -> Self {
        let Some(metrics) = metrics_weak.upgrade() else {
            return Bootstrap { ipv, inner: None };
        };

        Bootstrap {
            ipv,
            inner: Some(BootstrapInner {
                bootstrap_id: metrics.bootstrap_start(ipv),
                metrics: metrics_weak,
            }),
        }
    }

    fn mark_success(&self, _wan_endpoint: String) {
        let Some(inner) = &self.inner else {
            return;
        };

        let Some(metrics) = inner.metrics.upgrade() else {
            return;
        };

        metrics.bootstrap_finish(inner.bootstrap_id, self.ipv, true);
    }
}

impl Drop for Bootstrap {
    fn drop(&mut self) {
        let Some(inner) = &self.inner else {
            return;
        };

        let Some(metrics) = inner.metrics.upgrade() else {
            return;
        };

        metrics.bootstrap_finish(inner.bootstrap_id, self.ipv, false);
    }
}
