mod backoff;
mod metrics;
mod metrics_runner;
mod record_processor;
mod runtime;
mod store;

use crate::{
    ffi::CxxRecordProcessor, metrics_runner::MetricsRunnerError,
    record_processor::RecordProcessorError,
};
use cxx::UniquePtr;
use metrics::{IpVersion, Metrics};
use metrics_runner::metrics_runner;
use std::{
    path::PathBuf,
    sync::{Arc, Mutex},
};
use tokio::{runtime::Runtime, sync::oneshot, task::JoinHandle};

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

// TODO: Don't create new client if one already exists
fn new_client(store_path: String, processor: UniquePtr<CxxRecordProcessor>) -> Box<Client> {
    let processor = record_processor::RecordProcessor::new(processor);
    let runtime = runtime::get_runtime();
    let metrics = Arc::new(Metrics::new());

    let job_handle = runtime.spawn({
        let metrics = metrics.clone();
        let store_path = PathBuf::from(store_path);

        async move {
            if let Err(error) = metrics_runner(metrics, store_path, processor).await {
                match error {
                    MetricsRunnerError::RecordProcessor(RecordProcessorError::CxxDisconnected) => {
                        ()
                    }
                    MetricsRunnerError::Io(error) => {
                        log::error!("Metrics runner finished with an error: {error:?}")
                    }
                }
            }
        }
    });

    Box::new(Client {
        _runtime: runtime,
        metrics,
        job_handle,
    })
}

pub struct Client {
    _runtime: Arc<Runtime>,
    metrics: Arc<Metrics>,
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
            metrics: self.metrics.clone(),
        })
    }
}

pub struct MainlineDht {
    metrics: Arc<Metrics>,
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

pub struct DhtNode {
    ipv: IpVersion,
    metrics: Arc<Metrics>,
}

impl DhtNode {
    fn new_bootstrap(&self) -> Box<Bootstrap> {
        let metrics = self.metrics.clone();
        let bootstrap_id = metrics.bootstrap_start(self.ipv);
        Box::new(Bootstrap {
            bootstrap_id,
            ipv: self.ipv,
            metrics: metrics.clone(),
        })
    }
}

pub struct Bootstrap {
    bootstrap_id: usize,
    ipv: IpVersion,
    metrics: Arc<Metrics>,
}

impl Bootstrap {
    fn mark_success(&self, _wan_endpoint: String) {
        self.metrics
            .bootstrap_finish(self.bootstrap_id, self.ipv, true);
    }
}

impl Drop for Bootstrap {
    fn drop(&mut self) {
        self.metrics
            .bootstrap_finish(self.bootstrap_id, self.ipv, false);
    }
}
