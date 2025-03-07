use crate::{
    backoff_watch::ConstantBackoffWatchReceiver,
    metrics::Metrics,
    record_processor::{RecordProcessor, RecordProcessorError},
    store::{Store, StoredRecord},
};
use std::{
    io,
    path::PathBuf,
    sync::{Arc, Mutex},
};
use thiserror::Error;
use tokio::{select, sync::mpsc, time};

enum Event {
    // TODO: Use lifetime instead of an Arc?
    ProcessOneRecord(Arc<RecordProcessor>),
    MetricsModified,
    IncrementRecordNumber,
    RotateDeviceId,
    Exit,
}

pub async fn metrics_runner(
    metrics: Arc<Mutex<Metrics>>,
    store_path: PathBuf,
    record_processor_rx: mpsc::UnboundedReceiver<Option<RecordProcessor>>,
) -> Result<(), MetricsRunnerError> {
    let mut store = Store::new(store_path).await?;

    let mut event_listener =
        EventListener::new(metrics.lock().unwrap().subscribe(), record_processor_rx);
    let mut event_handler = EventHandler::new();

    loop {
        let event = event_listener.on_event(&store).await;

        match event_handler
            .process_event(event, &mut store, &*metrics)
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
        // TODO: We don't want to store records when there is no record_processor (i.e. when
        // metrics are disabled)

        match event {
            Event::ProcessOneRecord(record_processor) => {
                log::debug!("Event:ProcessOneRecord");

                if self.oldest_record.is_none() {
                    self.oldest_record = store.oldest_non_current_record().await?;
                }

                let Some(record) = &self.oldest_record else {
                    log::debug!("  Nothing to process");
                    store.backoff.stop();
                    return Ok(EventResult::Continue);
                };

                if record_processor.as_ref().process(&record).await? {
                    store.backoff.succeeded().await?;
                    record.discard().await?;
                    self.oldest_record = None;
                } else {
                    store.backoff.failed().await?;
                }
            }
            Event::MetricsModified => {
                log::debug!("Event::MetricsModified");
                if store_record(store, metrics).await? {
                    log::debug!("  stored");
                } else {
                    log::debug!("  no new data");
                }
            }
            Event::RotateDeviceId => {
                log::debug!("Event::RotateDeviceId");

                store_record(store, metrics).await?;

                metrics.lock().unwrap().clear();
                store.record_number.reset().await?;
                store.device_id.rotate().await?;
            }
            Event::IncrementRecordNumber => {
                log::debug!("Event::IncrementRecordNumber");

                store_record(store, metrics).await?;

                store.record_number.increment().await?;
            }
            Event::Exit => {
                log::debug!("Event::Exit");
                store_record(store, metrics).await?;
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

            select! {
                record_processor = processing_backoff =>
                    break Event::ProcessOneRecord(record_processor),
                result = self.on_metrics_modified_rx.changed() => {
                    match result {
                        Ok(()) => break Event::MetricsModified,
                        Err(_) => break Event::Exit,
                    }
                }
                result = self.record_processor_rx.recv() => {
                    match result {
                        Some(record_processor) => {
                            self.record_processor = record_processor.map(Arc::new);
                            continue;
                        }
                        None => break Event::Exit
                    }
                }
                () = time::sleep_until(store.record_number.increment_at()) => break Event::IncrementRecordNumber,
                () = time::sleep(store.device_id.rotate_after()) => break Event::RotateDeviceId,
            }
        }
    }
}

async fn store_record(store: &mut Store, metrics: &Mutex<Metrics>) -> io::Result<bool> {
    let record = metrics.lock().unwrap().collect();

    if let Some(record) = record {
        store.store_record(record).await?;
        store.backoff.resume();
        Ok(true)
    } else {
        Ok(false)
    }
}

enum EventResult {
    Continue,
    Break,
}

#[derive(Error, Debug)]
pub enum MetricsRunnerError {
    #[error("RecordProcessor error {0}")]
    RecordProcessor(#[from] RecordProcessorError),
    #[error("IO error {0}")]
    Io(#[from] io::Error),
}

#[cfg(test)]
mod test {
    use super::*;
    use std::collections::BTreeSet;
    use tmpdir::TmpDir;
    use uuid::Uuid;

    #[derive(Debug, Eq, PartialEq, Copy, Clone, Ord, PartialOrd)]
    struct RecordId {
        device_id: Uuid,
        record_no: u32,
    }

    struct Setup {
        _tmpdir: TmpDir,
        event_handler: EventHandler,
        store: Store,
        metrics: Mutex<Metrics>,
    }

    impl Setup {
        async fn new() -> Self {
            let tmpdir = TmpDir::new("metrics_runner_store").await.unwrap();
            let store = Store::new(tmpdir.as_ref().into()).await.unwrap();
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
                    device_id: rs.device_id,
                    record_no: rs.record_number,
                })
                .collect()
        }

        fn current_record_id(&self) -> RecordId {
            RecordId {
                device_id: *self.store.device_id,
                record_no: *self.store.record_number,
            }
        }

        async fn modify_metrics_and_process(&mut self) {
            // Modifying metrics normally sends an event through `tokio::watch` to the
            // event_listener. But we don't have event_listener here so we need to simulate it.
            self.metrics.lock().unwrap().modify();
            self.process(Event::MetricsModified).await;
        }
    }

    #[tokio::test]
    async fn event_processing() {
        let mut setup = Setup::new().await;

        let processor = Arc::new(RecordProcessor::new());

        setup
            .process(Event::ProcessOneRecord(processor.clone()))
            .await;

        assert!(setup.stored_record_ids().await.is_empty());

        let record0_id = setup.current_record_id();
        assert_eq!(record0_id.record_no, 0);

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
        setup.process(Event::IncrementRecordNumber).await;

        let record1_id = setup.current_record_id();

        assert_eq!(record0_id.device_id, record1_id.device_id);
        assert_ne!(record0_id.record_no, record1_id.record_no);

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
        setup.process(Event::RotateDeviceId).await;

        let record2_id = setup.current_record_id();

        assert_ne!(record1_id.device_id, record2_id.device_id);
        assert_eq!(record2_id.record_no, 0);

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
        setup.process(Event::RotateDeviceId).await;

        // Process again, this time it should be removed
        setup
            .process(Event::ProcessOneRecord(processor.clone()))
            .await;

        assert_eq!(setup.stored_record_ids().await, [].into_iter().collect());
    }
}
