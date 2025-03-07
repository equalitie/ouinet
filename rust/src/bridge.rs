use crate::{
    logger,
    metrics::{BootstrapId, IpVersion, Metrics},
    metrics_runner::metrics_runner,
    metrics_runner::MetricsRunnerError,
    record_processor::{RecordProcessor, RecordProcessorError},
    runtime,
};
use cxx::UniquePtr;
use std::{
    path::PathBuf,
    sync::{Arc, Mutex, Weak},
};
use tokio::{
    runtime::Runtime,
    select,
    sync::{mpsc, oneshot},
    task::{self, JoinHandle},
    time::{sleep, Duration},
};

#[cxx::bridge]
mod ffi {
    #[namespace = "ouinet::metrics::bridge"]
    extern "Rust" {
        //------------------------------------------------------------
        type Client;

        fn new_client(store_path: String) -> Box<Client>;
        fn new_noop_client() -> Box<Client>;
        fn new_mainline_dht(self: &Client) -> Box<MainlineDht>;

        // Until the processor is set, no metrics will be stored on the disk nor sent. The (non
        // no-oop) client will, however collect metrics in memory so that once once (and if) the
        // processor is set eventually, the metrics from this runtime can be collected.
        fn set_processor(self: &Client, processor: UniquePtr<CxxRecordProcessor>);

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

        #[allow(dead_code)] // Not used in tests
        fn execute(
            self: &CxxRecordProcessor,
            record_name: String,
            record_data: String,
            on_finish: Box<CxxOneShotSender>,
        );
    }
}

pub use ffi::CxxRecordProcessor;
unsafe impl Send for CxxRecordProcessor {}
unsafe impl Sync for CxxRecordProcessor {}

pub struct CxxOneShotSender {
    tx: Mutex<Option<oneshot::Sender<bool>>>,
}

impl CxxOneShotSender {
    #[cfg(not(test))]
    pub fn new(tx: oneshot::Sender<bool>) -> Self {
        Self {
            tx: Mutex::new(Some(tx)),
        }
    }

    fn send(&self, success: bool) {
        self.tx
            .lock()
            .unwrap()
            .take()
            .expect("The CxxOneShot::send function must be used at most once")
            .send(success)
            .unwrap_or(());
    }
}

// -------------------------------------------------------------------

static CURRENT_CLIENT_INNER: Mutex<Weak<ClientInner>> = Mutex::new(Weak::new());

// Stop existing client if there is one, this also ensures there is always at most one client
// that writes into the store.
// TODO: Alternative is to have multiple c++ clients to point to the same rust client, both
// approaches have pros and cons.
fn stop_current_client() -> Option<Arc<Runtime>> {
    let client_inner_lock = CURRENT_CLIENT_INNER.lock().unwrap();

    if let Some(client_inner) = client_inner_lock.upgrade() {
        client_inner.stop()
    } else {
        None
    }
}

// Create a new Client, if there were any clients created but not destroyed beforehand, all their
// operations will be no-ops.
fn new_client(store_path: String) -> Box<Client> {
    logger::init_idempotent();
    let runtime = match stop_current_client() {
        Some(runtime) => runtime,
        None => runtime::get_runtime(),
    };
    Box::new(Client::new(PathBuf::from(store_path), runtime))
}

fn new_noop_client() -> Box<Client> {
    logger::init_idempotent();
    stop_current_client();
    Box::new(Client::new_noop())
}

// -------------------------------------------------------------------

pub struct Client {
    inner: Arc<ClientInner>,
}

impl Client {
    fn new(store_path: PathBuf, runtime: Arc<Runtime>) -> Self {
        let _runtime_guard = runtime.enter();

        let (processor_tx, processor_rx) = mpsc::unbounded_channel();

        let metrics = Arc::new(Mutex::new(Metrics::new()));

        let job_handle = task::spawn({
            let metrics = metrics.clone();

            async move {
                let store_path = PathBuf::from(store_path);

                if let Err(error) = metrics_runner(metrics, store_path, processor_rx).await {
                    match error {
                        MetricsRunnerError::RecordProcessor(
                            RecordProcessorError::CxxDisconnected,
                        ) => (),
                        MetricsRunnerError::Io(error) => {
                            log::error!("Metrics runner finished with an error: {error:?}")
                        }
                    }
                }
            }
        });

        Self {
            inner: Arc::new(ClientInner {
                runner: Mutex::new(Some(Runner {
                    runtime,
                    processor_tx,
                    job_handle,
                })),
                metrics: Arc::downgrade(&metrics),
            }),
        }
    }

    fn new_noop() -> Self {
        Self {
            inner: Arc::new(ClientInner {
                runner: Mutex::new(None),
                metrics: Weak::new(),
            }),
        }
    }

    fn set_processor(&self, processor: UniquePtr<CxxRecordProcessor>) {
        self.inner.set_processor(processor)
    }

    fn new_mainline_dht(&self) -> Box<MainlineDht> {
        self.inner.new_mainline_dht()
    }
}

struct ClientInner {
    runner: Mutex<Option<Runner>>,
    metrics: Weak<Mutex<Metrics>>,
}

struct Runner {
    runtime: Arc<Runtime>,
    processor_tx: mpsc::UnboundedSender<Option<RecordProcessor>>,
    job_handle: JoinHandle<()>,
}

impl ClientInner {
    fn set_processor(&self, processor: UniquePtr<CxxRecordProcessor>) {
        let processor = if !processor.is_null() {
            #[cfg(not(test))]
            {
                Some(RecordProcessor::new(processor))
            }
            #[cfg(test)]
            {
                let _unused = processor;
                Some(RecordProcessor::new())
            }
        } else {
            None
        };

        let runner_lock = self.runner.lock().unwrap();

        let Some(runner) = runner_lock.as_ref() else {
            log::warn!("Passing record processor to a noop client");
            return;
        };

        runner
            .processor_tx
            .send(processor)
            .expect("While the client exist the mpsc channel should not close");
    }

    fn new_mainline_dht(&self) -> Box<MainlineDht> {
        Box::new(MainlineDht {
            metrics: self.metrics.clone(),
        })
    }

    fn stop(&self) -> Option<Arc<Runtime>> {
        let mut lock = self.runner.lock().unwrap();

        let Some(Runner {
            runtime,
            processor_tx,
            mut job_handle,
        }) = lock.take()
        else {
            return None;
        };

        // Signal to metrics_runner that we are exiting so it can save the most recent metrics.
        drop(processor_tx);

        // TODO: The C++ code itself does some deinitialization with a timeout and the code in this
        // function happens only after that is finished. Consider doing this session
        // deinitialization concurrently with the C++ code.
        runtime.block_on(async move {
            select! {
                () = sleep(Duration::from_secs(5)) => {
                    log::warn!("Metrics runner failed to finish within 5 seconds");
                    job_handle.abort();
                }
                _ = &mut job_handle => (),
            }
        });

        Some(runtime)
    }
}

impl Drop for ClientInner {
    fn drop(&mut self) {
        self.stop();
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
