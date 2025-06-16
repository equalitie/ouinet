use crate::{
    backoff_watch::ConstantBackoffWatchReceiver,
    metrics::Metrics,
    record_processor::{RecordProcessor, RecordProcessorError},
    store::{Store, StoredRecord},
};
use std::{
    fmt, io,
    sync::{Arc, Mutex},
};
use thiserror::Error;
use tokio::{select, sync::mpsc, time};

enum Event {
    // TODO: Use lifetime instead of an Arc?
    ProcessOneRecord(Arc<RecordProcessor>),
    MetricsModified { metrics_enabled: bool },
    IncrementRecordNumber { metrics_enabled: bool },
    RotateDeviceId { metrics_enabled: bool },
    Purge,
    Exit { metrics_enabled: bool },
}

impl fmt::Debug for Event {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::ProcessOneRecord(_) => write!(f, "Event::ProcessOneRecord"),
            Self::MetricsModified { metrics_enabled } => write!(
                f,
                "Event::MetricsModified{{ metrics_enabled: {:?} }}",
                metrics_enabled
            ),
            Self::IncrementRecordNumber { metrics_enabled } => write!(
                f,
                "Event::IncrementRecordNumber{{ metrics_enabled: {:?} }}",
                metrics_enabled
            ),
            Self::RotateDeviceId { metrics_enabled } => write!(
                f,
                "Event::RotateDeviceId{{ metrics_enabled: {:?} }}",
                metrics_enabled
            ),
            Self::Purge => write!(f, "Event::Purge",),
            Self::Exit { metrics_enabled } => {
                write!(f, "Event::Exit{{ metrics_enabled: {:?} }}", metrics_enabled)
            }
        }
    }
}

pub async fn metrics_runner(
    metrics: Arc<Mutex<Metrics>>,
    store: Store,
    record_processor_rx: mpsc::UnboundedReceiver<Option<RecordProcessor>>,
) {
    log::debug!("Metrics runner started with initial state:");

    log::debug!(
        "  Current record ID: {}",
        store.current_record_id().to_string()
    );
    match store.load_stored_records().await {
        Ok(stored_records) => {
            log::debug!(
                "  Stored records: {:?}",
                stored_records.iter().map(|r| r.name()).collect::<Vec<_>>()
            );
        }
        Err(error) => {
            log::error!("Failed to read stored records: {error:?}, Metrics runner finished");
            return;
        }
    }

    match metrics_runner_(metrics, store, record_processor_rx).await {
        Ok(()) => {
            log::debug!("Metrics runner finished with Ok")
        }
        Err(MetricsRunnerError::RecordProcessor(RecordProcessorError::CxxDisconnected)) => {
            log::debug!("Metrics runner finished with CxxDisconnected")
        }
        Err(MetricsRunnerError::Io(error)) => {
            log::error!("Metrics runner finished with an error: {error:?}")
        }
    }
}

pub async fn metrics_runner_(
    metrics: Arc<Mutex<Metrics>>,
    mut store: Store,
    record_processor_rx: mpsc::UnboundedReceiver<Option<RecordProcessor>>,
) -> Result<(), MetricsRunnerError> {
    let mut event_listener =
        EventListener::new(metrics.lock().unwrap().subscribe(), record_processor_rx);
    let mut event_handler = EventHandler::new();

    loop {
        let event = event_listener.on_event(&store).await;

        match event_handler
            .process_event(event, &mut store, &metrics)
            .await?
        {
            EventResult::Continue => (),
            EventResult::Break => break,
        }
    }

    Ok(())
}

struct EventHandler {
    oldest_record: Option<StoredRecord>,
}

impl EventHandler {
    fn new() -> Self {
        Self {
            oldest_record: None,
        }
    }

    async fn process_event(
        &mut self,
        event: Event,
        store: &mut Store,
        metrics: &Mutex<Metrics>,
    ) -> Result<EventResult, MetricsRunnerError> {
        log::debug!("{event:?}");

        match event {
            Event::ProcessOneRecord(record_processor) => {
                if self.oldest_record.is_none() {
                    let current_record_id = store.current_record_id();
                    let stored_records = store.load_stored_records().await?;

                    log::debug!(
                        "  Records on disk: {:?}, current: {:?}",
                        stored_records.iter().map(|r| r.name()).collect::<Vec<_>>(),
                        Store::build_record_name(
                            crate::constants::RECORD_VERSION,
                            current_record_id
                        )
                    );

                    self.oldest_record = stored_records
                        .into_iter()
                        .filter(|record| record.id != current_record_id)
                        .min_by(|l, r| l.created.cmp(&r.created));
                }

                let Some(record) = &self.oldest_record else {
                    log::debug!("  Nothing to process");
                    store.backoff.stop();
                    return Ok(EventResult::Continue);
                };

                if record_processor.as_ref().process(record).await? {
                    log::debug!("  Sucess {:?}", record.name());
                    store.backoff.succeeded().await?;
                    record.discard().await?;
                    self.oldest_record = None;
                } else {
                    log::debug!("  Failure {:?}", record.name());
                    store.backoff.failed().await?;
                }
            }
            Event::MetricsModified { metrics_enabled } => {
                if metrics_enabled {
                    store_record(store, metrics).await?;
                }
            }
            Event::RotateDeviceId { metrics_enabled } => {
                if metrics_enabled {
                    store_record(store, metrics).await?;
                }

                store.record_id.rotate().await?;
                metrics.lock().unwrap().on_device_id_changed();
            }
            Event::IncrementRecordNumber { metrics_enabled } => {
                if metrics_enabled {
                    store_record(store, metrics).await?;
                }

                store.record_id.increment().await?;
                metrics.lock().unwrap().on_record_sequence_number_changed();
            }
            Event::Purge => {
                store.delete_stored_records().await?;
                return Ok(EventResult::Continue);
            }
            Event::Exit { metrics_enabled } => {
                if metrics_enabled {
                    store_record(store, metrics).await?;
                }
                return Ok(EventResult::Break);
            }
        }

        Ok(EventResult::Continue)
    }
}

pub struct EventListener {
    on_metrics_modified_rx: ConstantBackoffWatchReceiver,
    record_processor_rx: mpsc::UnboundedReceiver<Option<RecordProcessor>>,
    record_processor: Option<Arc<RecordProcessor>>,
}

impl EventListener {
    fn new(
        on_metrics_modified_rx: ConstantBackoffWatchReceiver,
        record_processor_rx: mpsc::UnboundedReceiver<Option<RecordProcessor>>,
    ) -> Self {
        Self {
            on_metrics_modified_rx,
            record_processor_rx,
            record_processor: None,
        }
    }

    async fn on_event(&mut self, store: &Store) -> Event {
        loop {
            let processing_backoff = async {
                store.backoff.sleep().await;
                match self.record_processor.as_ref() {
                    Some(record_processor) => record_processor.clone(),
                    None => std::future::pending().await,
                }
            };

            let metrics_enabled = self.record_processor.is_some();

            select! {
                record_processor = processing_backoff =>
                    break Event::ProcessOneRecord(record_processor),
                result = self.on_metrics_modified_rx.changed() => {
                    match result {
                        Ok(()) => break Event::MetricsModified { metrics_enabled },
                        Err(_) => break Event::Exit { metrics_enabled },
                    }
                }
                result = self.record_processor_rx.recv() => {
                    match result {
                        Some(Some(record_processor)) => {
                            self.record_processor = Some(Arc::new(record_processor));
                            continue;
                        }
                        Some(None) => {
                            self.record_processor = None;
                            break Event::Purge;
                        }
                        None => break Event::Exit { metrics_enabled },
                    }
                }
                () = time::sleep_until(store.record_id.sequence_number().increment_at()) => break Event::IncrementRecordNumber { metrics_enabled },
                () = time::sleep(store.record_id.device_id().rotate_after()) => break Event::RotateDeviceId { metrics_enabled },
            }
        }
    }
}

async fn store_record(store: &mut Store, metrics: &Mutex<Metrics>) -> io::Result<()> {
    let record = metrics.lock().unwrap().collect();

    // Backoff may have stopped due to there being no records *or* because the one record that was
    // there was "current". So resume it.
    store.backoff.resume();

    if let Some(record) = record {
        log::debug!(
            "  Storing record {:?}",
            store.current_record_id().to_string()
        );
        store.store_record(record).await?;
        log::debug!("  Done");
    } else {
        log::debug!("  No new data to store");
    }

    Ok(())
}

enum EventResult {
    Continue,
    Break,
}

#[derive(Error, Debug)]
pub enum MetricsRunnerError {
    #[error("record processor error")]
    RecordProcessor(#[from] RecordProcessorError),
    #[error("IO error")]
    Io(#[from] io::Error),
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        crypto::{DecryptionKey, EncryptionKey},
        logger,
        record_id::RecordId,
    };
    use std::collections::BTreeSet;
    use tmpdir::TmpDir;

    struct Setup {
        _tmpdir: TmpDir,
        event_handler: EventHandler,
        store: Store,
        metrics: Mutex<Metrics>,
    }

    impl Setup {
        async fn new() -> Self {
            logger::init_idempotent();

            let tmpdir = TmpDir::new("metrics_runner_store").await.unwrap();
            let dk = DecryptionKey::random(&mut rand::rng());
            let ek = EncryptionKey::from(&dk);
            let store = Store::new(tmpdir.as_ref().into(), ek).await.unwrap();
            let event_handler = EventHandler::new();
            let metrics = Mutex::new(Metrics::new());

            Self {
                _tmpdir: tmpdir,
                event_handler,
                store,
                metrics,
            }
        }

        async fn process(&mut self, event: Event) {
            self.event_handler
                .process_event(event, &mut self.store, &self.metrics)
                .await
                .unwrap();
        }

        async fn stored_record_ids(&self) -> BTreeSet<RecordId> {
            self.store
                .load_stored_records()
                .await
                .unwrap()
                .iter()
                .map(|rs| RecordId {
                    device_id: rs.id.device_id,
                    sequence_number: rs.id.sequence_number,
                })
                .collect()
        }

        fn current_record_id(&self) -> RecordId {
            self.store.current_record_id()
        }

        async fn modify_metrics_and_process(&mut self) {
            // Modifying metrics normally sends an event through `tokio::watch` to the
            // event_listener. But we don't have event_listener here so we need to simulate it.
            self.metrics.lock().unwrap().modify();
            self.process(Event::MetricsModified {
                metrics_enabled: true,
            })
            .await;
        }
    }

    #[tokio::test]
    async fn event_processing() {
        let mut setup = Setup::new().await;

        let processor = Arc::new(RecordProcessor::new());
        let metrics_enabled = true;

        setup
            .process(Event::ProcessOneRecord(processor.clone()))
            .await;

        assert!(setup.stored_record_ids().await.is_empty());

        let record0_id = setup.current_record_id();
        assert_eq!(record0_id.sequence_number, 0);

        setup.modify_metrics_and_process().await;

        // Check record is stored
        assert_eq!(
            setup.stored_record_ids().await,
            [record0_id].into_iter().collect()
        );

        setup.modify_metrics_and_process().await;

        // Check record is stored still with the same id
        assert_eq!(
            setup.stored_record_ids().await,
            [record0_id].into_iter().collect()
        );

        // Update record number within the record id
        setup
            .process(Event::IncrementRecordNumber { metrics_enabled })
            .await;

        let record1_id = setup.current_record_id();

        assert_eq!(record0_id.device_id, record1_id.device_id);
        assert_ne!(record0_id.sequence_number, record1_id.sequence_number);

        // Metrics hasn't been modified yet, so no new record has been created
        assert_eq!(
            setup.stored_record_ids().await,
            [record0_id].into_iter().collect()
        );

        setup.modify_metrics_and_process().await;

        // The new record is should now be created
        assert_eq!(
            setup.stored_record_ids().await,
            [record0_id, record1_id].into_iter().collect()
        );

        // Rotate device id
        setup
            .process(Event::RotateDeviceId { metrics_enabled })
            .await;

        let record2_id = setup.current_record_id();

        assert_ne!(record1_id.device_id, record2_id.device_id);
        assert_eq!(record2_id.sequence_number, 0);

        // Nothing changes yet
        assert_eq!(
            setup.stored_record_ids().await,
            [record0_id, record1_id].into_iter().collect()
        );

        setup.modify_metrics_and_process().await;

        // record2_id appears
        assert_eq!(
            setup.stored_record_ids().await,
            [record0_id, record1_id, record2_id].into_iter().collect()
        );

        // -------------------
        // Process the records
        // -------------------
        setup
            .process(Event::ProcessOneRecord(processor.clone()))
            .await;

        assert_eq!(
            setup.stored_record_ids().await,
            [record1_id, record2_id].into_iter().collect()
        );

        setup
            .process(Event::ProcessOneRecord(processor.clone()))
            .await;

        assert_eq!(
            setup.stored_record_ids().await,
            [record2_id].into_iter().collect()
        );

        setup
            .process(Event::ProcessOneRecord(processor.clone()))
            .await;

        // Not removed because record2_id is "current" (still being updated)
        assert_eq!(
            setup.stored_record_ids().await,
            [record2_id].into_iter().collect()
        );

        // Rotate the current record id
        setup
            .process(Event::RotateDeviceId { metrics_enabled })
            .await;

        // Process again, this time it should be removed
        setup
            .process(Event::ProcessOneRecord(processor.clone()))
            .await;

        assert_eq!(setup.stored_record_ids().await, [].into_iter().collect());
    }

    #[tokio::test]
    async fn resume_after_record_id_change_even_if_no_write() {
        let mut setup = Setup::new().await;

        let processor = Arc::new(RecordProcessor::new());

        // Modify metrics and store it on disk.
        setup.modify_metrics_and_process().await;

        // Try processing a record. This won't process anything because the one stored record is
        // "current". But because there is nothing to proces it should `stop` the processing
        // timeout.
        setup
            .process(Event::ProcessOneRecord(processor.clone()))
            .await;

        assert!(setup.store.backoff.is_stopped());

        // Nothing is written here because no modifications to metrics happened since the last
        // write, but we still want the backoff to be resumed.
        setup
            .process(Event::IncrementRecordNumber {
                metrics_enabled: true,
            })
            .await;

        assert!(!setup.store.backoff.is_stopped());
    }

    #[tokio::test]
    async fn delete_stored_records() {
        let mut setup = Setup::new().await;
        assert_eq!(setup.store.load_stored_records().await.unwrap().len(), 0);

        setup.modify_metrics_and_process().await;
        assert_eq!(setup.store.load_stored_records().await.unwrap().len(), 1);

        setup.store.delete_stored_records().await.unwrap();
        assert_eq!(setup.store.load_stored_records().await.unwrap().len(), 0);
    }
}
