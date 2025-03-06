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
use tokio::{select, sync::watch, time};

enum Event {
    ProcessOneRecord,
    MetricsModified,
    IncrementRecordNumber,
    RotateDeviceId,
    Exit,
}

pub async fn metrics_runner(
    metrics: Arc<Mutex<Metrics>>,
    store_path: PathBuf,
    record_processor: RecordProcessor,
    finish_rx: watch::Receiver<()>,
) -> Result<(), MetricsRunnerError> {
    let mut store = Store::new(store_path).await?;

    let mut event_listener = EventListener::new(metrics.lock().unwrap().subscribe(), finish_rx);

    let mut event_processor = EventProcessor::new(record_processor);

    loop {
        let event = event_listener.on_event(&store).await;

        if event_processor
            .process_event(event, &mut store, &*metrics)
            .await?
        {
            break;
        }
    }

    Ok(())
}

struct EventProcessor {
    oldest_record: Option<StoredRecord>,
    record_processor: RecordProcessor,
}

impl EventProcessor {
    fn new(record_processor: RecordProcessor) -> Self {
        Self {
            oldest_record: None,
            record_processor,
        }
    }

    async fn process_event(
        &mut self,
        event: Event,
        store: &mut Store,
        metrics: &Mutex<Metrics>,
    ) -> Result<bool, MetricsRunnerError> {
        match event {
            Event::ProcessOneRecord => {
                log::debug!("Event:ProcessOneRecord");

                if self.oldest_record.is_none() {
                    self.oldest_record = store.oldest_non_current_record().await?;
                }

                let Some(record) = &self.oldest_record else {
                    log::debug!("  Nothing to process");
                    store.backoff.stop();
                    return Ok(false);
                };

                if self.record_processor.process(&record).await? {
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
                return Ok(true);
            }
        }

        Ok(false)
    }
}

pub struct EventListener {
    on_metrics_modified_rx: ConstantBackoffWatchReceiver,
    finish_rx: watch::Receiver<()>,
}

impl EventListener {
    fn new(
        on_metrics_modified_rx: ConstantBackoffWatchReceiver,
        finish_rx: watch::Receiver<()>,
    ) -> Self {
        Self {
            on_metrics_modified_rx,
            finish_rx,
        }
    }

    async fn on_event(&mut self, store: &Store) -> Event {
        select! {
            () = store.backoff.sleep() => Event::ProcessOneRecord,
            result = self.on_metrics_modified_rx.changed() => {
                match result {
                    Ok(()) => Event::MetricsModified,
                    Err(_) => Event::Exit,
                }
            }
            _result = self.finish_rx.changed() => Event::Exit,
            () = time::sleep_until(store.record_number.increment_at()) => Event::IncrementRecordNumber,
            () = time::sleep(store.device_id.rotate_after()) => Event::RotateDeviceId,
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
    use tmpdir::TmpDir;

    struct Setup {
        tmpdir: TmpDir,
        event_processor: EventProcessor,
        store: Store,
        metrics: Mutex<Metrics>,
    }

    impl Setup {
        async fn new() -> Self {
            let tmpdir = TmpDir::new("metrics_runner_store").await.unwrap();
            let store = Store::new(tmpdir.as_ref().into()).await.unwrap();
            let event_processor = EventProcessor::new(RecordProcessor::new());
            let metrics = Mutex::new(Metrics::new());

            Self {
                tmpdir,
                event_processor,
                store,
                metrics,
            }
        }

        async fn process(&mut self, event: Event) {
            self.event_processor
                .process_event(event, &mut self.store, &self.metrics)
                .await
                .unwrap();
        }
    }

    #[tokio::test]
    async fn test() {
        let mut setup = Setup::new().await;

        setup.process(Event::ProcessOneRecord).await;

        //assert!(setup.store.load_stored_records().await.unwrap().is_empty())
    }
}
