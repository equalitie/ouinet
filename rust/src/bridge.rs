use crate::{
    crypto::EncryptionKey,
    logger,
    metrics::{
        request::{self, RequestId, RequestType},
        BootstrapId, IpVersion, Metrics,
    },
    metrics_runner::{metrics_runner, MetricsRunnerError},
    record_processor::{RecordProcessor, RecordProcessorError},
    runtime,
    store::Store,
};
use cxx::UniquePtr;
use std::{
    path::PathBuf,
    sync::{Arc, Mutex, Weak},
};
use tokio::{
    runtime::Runtime,
    select,
    sync::{mpsc, oneshot, watch},
    task::{self, JoinHandle},
    time::{sleep, Duration},
};
use uuid::Uuid;

#[cxx::bridge]
mod ffi {
    #[namespace = "ouinet::metrics::bridge"]
    extern "Rust" {
        //------------------------------------------------------------
        type Client;

        fn new_client(store_path: String, encryption_key: Box<EncryptionKey>) -> Box<Client>;
        fn new_mainline_dht(self: &Client) -> Box<MainlineDht>;
        fn new_origin_request(self: &Client) -> Box<Request>;
        fn new_private_injector_request(self: &Client) -> Box<Request>;
        fn new_public_injector_request(self: &Client) -> Box<Request>;
        fn new_cache_in_request(self: &Client) -> Box<Request>;
        fn new_cache_out_request(self: &Client) -> Box<Request>;

        // Until the processor is set, no metrics will be stored on the disk nor sent. The (non
        // no-oop) client will, however collect metrics in memory so that once once (and if) the
        // processor is set eventually, the metrics from this runtime can be collected.
        fn set_processor(self: &Client, processor: UniquePtr<CxxRecordProcessor>);

        fn device_id(self: &Client) -> String;

        //------------------------------------------------------------
        type MainlineDht;
        fn new_dht_node(self: &MainlineDht, is_ipv4: bool) -> Box<DhtNode>;

        type DhtNode;
        fn new_bootstrap(self: &DhtNode) -> Box<Bootstrap>;

        type Bootstrap;
        fn mark_success(self: &Bootstrap);

        //------------------------------------------------------------
        type Request;
        fn mark_success(self: &Request);
        fn mark_failure(self: &Request);
        fn increment_transfer_size(self: &Request, added: usize);

        //------------------------------------------------------------
        // Tells the rust code when record processing on the C++ side has finished.
        pub type CxxOneShotSender;
        fn send(self: &CxxOneShotSender, success: bool);

        //------------------------------------------------------------
        type EncryptionKey;
        fn validate_encryption_key(pem_str: String) -> Result<Box<EncryptionKey>>;
    }

    #[namespace = "ouinet::metrics::bridge"]
    unsafe extern "C++" {
        include!("cxx/record_processor.h");
        type CxxRecordProcessor;

        #[allow(dead_code)] // Not used in tests
        fn execute(
            self: &CxxRecordProcessor,
            record_name: String,
            record_data: Vec<u8>,
            on_finish: Box<CxxOneShotSender>,
        );
    }
}

fn validate_encryption_key(pem_str: String) -> Result<Box<EncryptionKey>, String> {
    Ok(Box::new(
        EncryptionKey::from_pem(&pem_str).map_err(|error| error.to_string())?,
    ))
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

/// Create a new Client, if there were any clients created but not destroyed beforehand, all their
/// operations will be no-ops.
fn new_client(
    store_path: String,
    // False positive: we need to box the key because its opaque to C++
    #[expect(clippy::boxed_local)] encryption_key: Box<EncryptionKey>,
) -> Box<Client> {
    logger::init_idempotent();
    let runtime = match stop_current_client() {
        Some(runtime) => runtime,
        None => runtime::get_runtime(),
    };
    Box::new(Client::new(
        runtime,
        PathBuf::from(store_path),
        *encryption_key,
    ))
}

// -------------------------------------------------------------------

pub struct Client {
    inner: Arc<ClientInner>,
}

impl Client {
    fn new(runtime: Arc<Runtime>, store_path: PathBuf, encryption_key: EncryptionKey) -> Self {
        let _runtime_guard = runtime.enter();

        let (processor_tx, processor_rx) = mpsc::unbounded_channel();

        let metrics = Arc::new(Mutex::new(Metrics::new()));
        let store = runtime.block_on(Store::new(store_path, encryption_key));

        let (runner, device_id_rx) = match store {
            Ok(store) => {
                let metrics = metrics.clone();
                let device_id_rx = store.device_id.subscribe();
                let job_handle = task::spawn(async move {
                    if let Err(error) = metrics_runner(metrics, store, processor_rx).await {
                        match error {
                            MetricsRunnerError::RecordProcessor(
                                RecordProcessorError::CxxDisconnected,
                            ) => (),
                            MetricsRunnerError::Io(error) => {
                                log::error!("Metrics runner finished with an error: {error:?}")
                            }
                        }
                    }
                });

                (
                    Some(Runner {
                        runtime,
                        processor_tx,
                        job_handle,
                    }),
                    device_id_rx,
                )
            }
            Err(error) => {
                log::error!("Failed to initialize metrics store: {error:?}");

                // Dummy device id receiver that always returns `Uuid::nil()`
                (None, watch::channel(Uuid::nil()).1)
            }
        };

        Self {
            inner: Arc::new(ClientInner {
                runner: Mutex::new(runner),
                metrics,
                device_id_rx,
            }),
        }
    }

    fn set_processor(&self, processor: UniquePtr<CxxRecordProcessor>) {
        self.inner.set_processor(processor)
    }

    fn new_mainline_dht(&self) -> Box<MainlineDht> {
        self.inner.new_mainline_dht()
    }

    fn new_origin_request(&self) -> Box<Request> {
        self.new_request(RequestType::Origin)
    }

    fn new_public_injector_request(&self) -> Box<Request> {
        self.new_request(RequestType::InjectorPublic)
    }

    fn new_private_injector_request(&self) -> Box<Request> {
        self.new_request(RequestType::InjectorPrivate)
    }

    fn new_cache_in_request(&self) -> Box<Request> {
        self.new_request(RequestType::CacheIn)
    }

    fn new_cache_out_request(&self) -> Box<Request> {
        self.new_request(RequestType::CacheOut)
    }

    fn new_request(&self, request_type: RequestType) -> Box<Request> {
        let metrics = self.inner.metrics.clone();

        let id = metrics.lock().unwrap().requests.add_request(request_type);

        Box::new(Request {
            metrics,
            request_type,
            id,
        })
    }

    fn device_id(&self) -> String {
        self.inner.device_id_rx.borrow().to_string()
    }
}

struct ClientInner {
    runner: Mutex<Option<Runner>>,
    metrics: Arc<Mutex<Metrics>>,
    device_id_rx: watch::Receiver<Uuid>,
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

        let Runner {
            runtime,
            processor_tx,
            mut job_handle,
        } = lock.take()?;

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
    metrics: Arc<Mutex<Metrics>>,
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
    metrics: Arc<Mutex<Metrics>>,
}

impl DhtNode {
    fn new_bootstrap(&self) -> Box<Bootstrap> {
        Box::new(Bootstrap::new(self.ipv, self.metrics.clone()))
    }
}

// -------------------------------------------------------------------

pub struct Bootstrap {
    bootstrap_id: BootstrapId,
    success: Mutex<bool>,
    metrics: Arc<Mutex<Metrics>>,
}

impl Bootstrap {
    fn new(ipv: IpVersion, metrics: Arc<Mutex<Metrics>>) -> Self {
        let bootstrap_id = metrics.lock().unwrap().bootstrap_start(ipv);

        Bootstrap {
            bootstrap_id,
            success: Mutex::new(false),
            metrics,
        }
    }

    fn mark_success(&self) {
        *self.success.lock().unwrap() = true;

        self.metrics
            .lock()
            .unwrap()
            .bootstrap_finish(self.bootstrap_id, true);
    }
}

impl Drop for Bootstrap {
    fn drop(&mut self) {
        if *self.success.lock().unwrap() {
            // Don't report false if we already reported true.
            return;
        }

        self.metrics
            .lock()
            .unwrap()
            .bootstrap_finish(self.bootstrap_id, false);
    }
}

// -------------------------------------------------------------------

struct Request {
    metrics: Arc<Mutex<Metrics>>,
    request_type: RequestType,
    id: RequestId,
}

impl Request {
    fn increment_transfer_size(&self, added: usize) {
        self.metrics
            .lock()
            .unwrap()
            .requests
            .increment_transfer_size(self.request_type, added);
    }

    fn mark_success(&self) {
        self.remove_request(request::RemoveReason::Success);
    }

    fn mark_failure(&self) {
        self.remove_request(request::RemoveReason::Failure);
    }

    fn remove_request(&self, reason: request::RemoveReason) {
        self.metrics
            .lock()
            .unwrap()
            .requests
            .remove_request(self.id, reason);
    }
}

impl Drop for Request {
    fn drop(&mut self) {
        self.remove_request(request::RemoveReason::Cancelled);
    }
}
