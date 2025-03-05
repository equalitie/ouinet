use crate::{
    backoff_watch::ConstantBackoffWatchReceiver,
    metrics::Metrics,
    record_processor::{RecordProcessor, RecordProcessorError},
    store::Store,
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
    processor: RecordProcessor,
    finish_rx: watch::Receiver<()>,
) -> Result<(), MetricsRunnerError> {
    let mut store = Store::new(store_path).await?;

    let mut event_listener = EventListener::new(metrics.lock().unwrap().subscribe(), finish_rx);

    let mut oldest_record = None;

    loop {
        let event = event_listener.on_event(&store).await;

        match event {
            Event::ProcessOneRecord => {
                log::debug!("Event:ProcessOneRecord");

                let device_id = *store.device_id;

                if oldest_record.is_none() {
                    oldest_record = store
                        .load_stored_records()
                        .await?
                        .into_iter()
                        .filter(|record| !record.is_current(device_id, *store.record_number))
                        .min_by(|l, r| l.created.cmp(&r.created));
                }

                let Some(record) = &oldest_record else {
                    log::debug!("  Nothing to process");
                    store.backoff.stop();
                    continue;
                };

                if processor.process(&record).await? {
                    store.backoff.succeeded().await?;
                    record.discard().await?;
                    oldest_record = None;
                } else {
                    store.backoff.failed().await?;
                }
            }
            Event::MetricsModified => {
                log::debug!("Event::MetricsModified");
                if store_record(&mut store, &*metrics).await? {
                    log::debug!("  stored");
                } else {
                    log::debug!("  no new data");
                }
            }
            Event::RotateDeviceId => {
                log::debug!("Event::RotateDeviceId");

                store_record(&mut store, &*metrics).await?;

                metrics.lock().unwrap().clear();
                store.record_number.reset().await?;
                store.device_id.rotate().await?;
            }
            Event::IncrementRecordNumber => {
                log::debug!("Event::IncrementRecordNumber");

                store_record(&mut store, &*metrics).await?;

                store.record_number.increment().await?;
            }
            Event::Exit => {
                log::debug!("Event::Exit");
                store_record(&mut store, &*metrics).await?;
                break Ok(());
            }
        }
    }
}

struct EventListener {
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
