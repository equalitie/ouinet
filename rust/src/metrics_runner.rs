use crate::{
    metrics::Metrics,
    record_processor::{RecordProcessor, RecordProcessorError},
    store::Store,
};
use std::{io, path::PathBuf, sync::Arc};
use thiserror::Error;
use tokio::{select, sync::watch, time};

enum Event {
    ProcessOneRecord,
    MetricsModified,
    IncrementRecordNumber,
    Exit,
}

async fn on_event(on_metrics_modified_rx: &mut watch::Receiver<()>, store: &Store) -> Event {
    select! {
        () = store.backoff.sleep() => Event::ProcessOneRecord,
        result = on_metrics_modified_rx.changed() => {
            match result {
                Ok(()) => Event::MetricsModified,
                Err(_) => Event::Exit,
            }
        }
        () = time::sleep_until(store.record_number.increment_at()) => Event::IncrementRecordNumber,
    }
}

pub async fn metrics_runner(
    metrics: Arc<Metrics>,
    store_path: PathBuf,
    processor: RecordProcessor,
) -> Result<(), MetricsRunnerError> {
    let mut store = Store::new(store_path).await?;

    let mut on_metrics_modified_rx = metrics.subscribe();

    let mut oldest_record = None;

    loop {
        let event = on_event(&mut on_metrics_modified_rx, &store).await;

        match event {
            Event::ProcessOneRecord => {
                log::debug!("metrics_runner::ProcessOneRecord");

                let device_id = store.current_device_id().await?;

                if oldest_record.is_none() {
                    oldest_record = store
                        .load_stored_records()
                        .await?
                        .into_iter()
                        .filter(|record| !record.is_current(device_id, *store.record_number))
                        .min_by(|l, r| l.created.cmp(&r.created));
                }

                let Some(record) = &oldest_record else {
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
                // TODO: Don't write on every change. Add some constant backoff.
                log::debug!("metrics_runner::MetricsModified");
                let record = metrics.make_record_data();
                store.store_record(record).await?;
                store.backoff.resume();
            }
            Event::IncrementRecordNumber => {
                log::debug!("metrics_runner::IncrementRecordNumber");
                let record = metrics.make_record_data();
                store.store_record(record).await?;
                store.backoff.resume();
                store.record_number.increment().await?;
            }
            Event::Exit => {
                // TODO: Store
                break Ok(());
            }
        }
    }
}

#[derive(Error, Debug)]
pub enum MetricsRunnerError {
    #[error("RecordProcessor error {0}")]
    RecordProcessor(#[from] RecordProcessorError),
    #[error("IO error {0}")]
    Io(#[from] io::Error),
}
