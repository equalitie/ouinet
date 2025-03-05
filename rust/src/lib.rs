mod backoff;
mod backoff_watch;
mod clock;
mod constants;
mod device_id;
mod logger;
mod metrics;
mod metrics_runner;
mod record_number;
mod record_processor;
mod runtime;
mod store;

use crate::{
    ffi::CxxRecordProcessor, metrics_runner::MetricsRunnerError,
    record_processor::RecordProcessorError,
};
use cxx::UniquePtr;
use metrics::{BootstrapId, IpVersion, Metrics};
use metrics_runner::metrics_runner;
use std::{
    path::PathBuf,
    sync::{Arc, Mutex, Weak},
};
use tokio::{
    runtime::Runtime,
    select,
    sync::{oneshot, watch},
    task::{self, JoinHandle},
    time::{sleep, Duration},
};

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
    runtime: Arc<Runtime>,
    finish_tx: Option<watch::Sender<()>>,
    job_handle: Option<JoinHandle<()>>,
}

impl Drop for Session {
    fn drop(&mut self) {
        // Signal to metrics_runner that we are exiting so it can save the most recent metrics.

        // TODO: The C++ code itself does some deinitialization with a timeout and the code in this
        // function happens only after that is finished. Consider doing this session
        // deinitialization concurrently with the C++ code.
        self.finish_tx.take();

        if let Some(mut job_handle) = self.job_handle.take() {
            self.runtime.block_on(async move {
                select! {
                    () = sleep(Duration::from_secs(5)) => {
                        log::warn!("Metrics runner failed to finish within 5 seconds");
                        job_handle.abort();
                    }
                    _ = &mut job_handle => (),
                }
            });
        }
    }
}

static SESSION: Mutex<Option<Session>> = Mutex::new(None);

// Create a new Client, if there were any clients created but not destroyed beforehand, all their
// operations will be no-ops.
fn new_client(store_path: String, processor: UniquePtr<CxxRecordProcessor>) -> Box<Client> {
    logger::init_idempotent();

    let runtime = runtime::get_runtime();

    let _runtime_guard = runtime.enter();

    let mut session_lock = SESSION.lock().unwrap();

    // Stop existing session if there is one, this also ensures there is always at most one client
    // that writes into the store.
    session_lock.take();

    let (finish_tx, finish_rx) = watch::channel(());
    let metrics = Arc::new(Mutex::new(Metrics::new()));
    let weak_metrics = Arc::downgrade(&metrics);

    let processor = record_processor::RecordProcessor::new(processor);

    let job_handle = task::spawn(async move {
        let store_path = PathBuf::from(store_path);

        if let Err(error) = metrics_runner(metrics, store_path, processor, finish_rx).await {
            match error {
                MetricsRunnerError::RecordProcessor(RecordProcessorError::CxxDisconnected) => (),
                MetricsRunnerError::Io(error) => {
                    log::error!("Metrics runner finished with an error: {error:?}")
                }
            }
        }
    });

    *session_lock = Some(Session {
        runtime,
        finish_tx: Some(finish_tx),
        job_handle: Some(job_handle),
    });

    Box::new(Client {
        metrics: weak_metrics,
    })
}

fn new_noop_client() -> Box<Client> {
    SESSION.lock().unwrap().take();

    Box::new(Client {
        metrics: Weak::new(),
    })
}

// -------------------------------------------------------------------

pub struct Client {
    metrics: Weak<Mutex<Metrics>>,
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
    metrics: Weak<Mutex<Metrics>>,
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
    metrics: Weak<Mutex<Metrics>>,
}

impl DhtNode {
    fn new_bootstrap(&self) -> Box<Bootstrap> {
        Box::new(Bootstrap::new(self.ipv, self.metrics.clone()))
    }
}

// -------------------------------------------------------------------

pub struct Bootstrap {
    inner: Option<BootstrapInner>,
}

struct BootstrapInner {
    bootstrap_id: BootstrapId,
    success: Mutex<bool>,
    metrics: Weak<Mutex<Metrics>>,
}

impl Bootstrap {
    fn new(ipv: IpVersion, metrics_weak: Weak<Mutex<Metrics>>) -> Self {
        let Some(metrics_strong) = metrics_weak.upgrade() else {
            return Bootstrap { inner: None };
        };

        let mut metrics_lock = metrics_strong.lock().unwrap();

        Bootstrap {
            inner: Some(BootstrapInner {
                bootstrap_id: metrics_lock.bootstrap_start(ipv),
                success: Mutex::new(false),
                metrics: metrics_weak,
            }),
        }
    }

    fn mark_success(&self, _wan_endpoint: String) {
        let Some(inner) = &self.inner else {
            return;
        };

        *inner.success.lock().unwrap() = true;

        let Some(metrics) = inner.metrics.upgrade() else {
            return;
        };

        metrics
            .lock()
            .unwrap()
            .bootstrap_finish(inner.bootstrap_id, true);
    }
}

impl Drop for Bootstrap {
    fn drop(&mut self) {
        let Some(inner) = &self.inner else {
            return;
        };

        if *inner.success.lock().unwrap() {
            // Don't report false if we already reported true.
            return;
        }

        let Some(metrics) = inner.metrics.upgrade() else {
            return;
        };

        metrics
            .lock()
            .unwrap()
            .bootstrap_finish(inner.bootstrap_id, false);
    }
}
