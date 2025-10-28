//! Metrics API exposed to C++

use super::{
    collector::{
        request::{self, RequestId, RequestType},
        BootstrapId, Collector, IpVersion,
    },
    crypto::EncryptionKey,
    record_id::RecordId,
    record_processor::RecordProcessor,
    runner::metrics_runner,
    store::Store,
};
use crate::{logger, runtime};
use cxx::UniquePtr;
use std::{
    path::PathBuf,
    sync::{Arc, Mutex, Weak},
};
use tokio::{
    sync::{mpsc, oneshot, watch},
    task::JoinHandle,
    time::{self, Duration},
};

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
        fn bridge_transfer_i2c(self: &Client, byte_count: usize);
        fn bridge_transfer_c2i(self: &Client, byte_count: usize);
        fn set_aux_key_value(self: &Client, record_id: String, key: String, value: String) -> bool;

        // Until the processor is set, no metrics will be stored on the disk nor sent. The (non
        // no-oop) client will, however collect metrics in memory so that once once (and if) the
        // processor is set eventually, the metrics from this runtime can be collected.
        fn set_processor(self: &Client, processor: UniquePtr<CxxRecordProcessor>);

        fn current_device_id(self: &Client) -> String;
        fn current_record_id(self: &Client) -> String;

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

        // Not used in tests
        #[allow(dead_code)]
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
fn stop_current_client() {
    let client_inner_lock = CURRENT_CLIENT_INNER.lock().unwrap();

    if let Some(client_inner) = client_inner_lock.upgrade() {
        client_inner.stop()
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

    let runtime = runtime::handle().clone();

    stop_current_client();

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
    fn new(runtime: runtime::Handle, store_path: PathBuf, encryption_key: EncryptionKey) -> Self {
        let (processor_tx, processor_rx) = mpsc::unbounded_channel();

        let collector = Arc::new(Mutex::new(Collector::new(&runtime)));
        let store = runtime.block_on(Store::new(store_path, encryption_key));

        let (runner, record_id_rx) = match store {
            Ok(store) => {
                let collector = collector.clone();
                let record_id_rx = store.record_id.subscribe();
                let job_handle = runtime.spawn(async move {
                    metrics_runner(collector, store, processor_rx).await;
                });

                (
                    Some(Runner {
                        processor_tx,
                        job_handle,
                    }),
                    record_id_rx,
                )
            }
            Err(error) => {
                log::error!("Failed to initialize metrics store: {error:?}");

                // Dummy device id receiver that always returns `Uuid::nil()`
                (None, watch::channel(RecordId::nil()).1)
            }
        };

        Self {
            inner: Arc::new(ClientInner {
                runtime,
                runner: Mutex::new(runner),
                collector,
                record_id_rx,
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
        let collector = self.inner.collector.clone();

        let id = collector.lock().unwrap().requests.add_request(request_type);

        Box::new(Request {
            collector,
            request_type,
            id,
        })
    }

    fn current_device_id(&self) -> String {
        self.inner.record_id_rx.borrow().device_id.to_string()
    }

    fn current_record_id(&self) -> String {
        self.inner.record_id_rx.borrow().to_string()
    }

    fn bridge_transfer_i2c(&self, byte_count: usize) {
        let mut collector = self.inner.collector.lock().unwrap();
        collector.bridge_transfer_i2c(byte_count);
    }

    fn bridge_transfer_c2i(&self, byte_count: usize) {
        let mut collector = self.inner.collector.lock().unwrap();
        collector.bridge_transfer_c2i(byte_count);
    }

    fn set_aux_key_value(&self, record_id: String, key: String, value: String) -> bool {
        let mut collector = self.inner.collector.lock().unwrap();
        if record_id == self.current_record_id() {
            collector.set_aux_key_value(key, value);
            true
        } else {
            false
        }
    }
}

struct ClientInner {
    runtime: runtime::Handle,
    runner: Mutex<Option<Runner>>,
    collector: Arc<Mutex<Collector>>,
    record_id_rx: watch::Receiver<RecordId>,
}

struct Runner {
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
        let enabling = processor.is_some();

        let Some(runner) = runner_lock.as_ref() else {
            log::warn!("Passing record processor to a noop client");
            return;
        };

        let result = runner.processor_tx.send(processor);

        if enabling && result.is_err() {
            panic!("While the client exists the mpsc channel should not close");
        }
    }

    fn new_mainline_dht(&self) -> Box<MainlineDht> {
        Box::new(MainlineDht {
            collector: self.collector.clone(),
        })
    }

    fn stop(&self) {
        let mut lock = self.runner.lock().unwrap();

        let Some(Runner {
            processor_tx,
            mut job_handle,
        }) = lock.take()
        else {
            return;
        };

        // Signal to metrics_runner that we are exiting so it can save the most recent metrics.
        drop(processor_tx);

        // Enter the async runtime so we can use `tokio::time::timeout`.
        let _enter = self.runtime.enter();

        // TODO: The C++ code itself does some deinitialization with a timeout and the code in this
        // function happens only after that is finished. Consider doing this session
        // deinitialization concurrently with the C++ code.
        match self
            .runtime
            .block_on(time::timeout(Duration::from_secs(5), &mut job_handle))
        {
            Ok(_) => (),
            Err(_) => {
                log::warn!("Metrics runner failed to finish within 5 seconds");
                job_handle.abort();
            }
        }
    }
}

impl Drop for ClientInner {
    fn drop(&mut self) {
        self.stop();
    }
}

// -------------------------------------------------------------------

pub struct MainlineDht {
    collector: Arc<Mutex<Collector>>,
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
            collector: self.collector.clone(),
        })
    }
}

// -------------------------------------------------------------------

pub struct DhtNode {
    ipv: IpVersion,
    collector: Arc<Mutex<Collector>>,
}

impl DhtNode {
    fn new_bootstrap(&self) -> Box<Bootstrap> {
        Box::new(Bootstrap::new(self.ipv, self.collector.clone()))
    }
}

// -------------------------------------------------------------------

pub struct Bootstrap {
    bootstrap_id: BootstrapId,
    success: Mutex<bool>,
    collector: Arc<Mutex<Collector>>,
}

impl Bootstrap {
    fn new(ipv: IpVersion, collector: Arc<Mutex<Collector>>) -> Self {
        let bootstrap_id = collector.lock().unwrap().bootstrap_start(ipv);

        Bootstrap {
            bootstrap_id,
            success: Mutex::new(false),
            collector,
        }
    }

    fn mark_success(&self) {
        *self.success.lock().unwrap() = true;

        self.collector
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

        self.collector
            .lock()
            .unwrap()
            .bootstrap_finish(self.bootstrap_id, false);
    }
}

// -------------------------------------------------------------------

struct Request {
    collector: Arc<Mutex<Collector>>,
    request_type: RequestType,
    id: RequestId,
}

impl Request {
    fn increment_transfer_size(&self, added: usize) {
        self.collector
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
        self.collector
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
