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
    RotateDeviceId,
    Exit,
}

async fn on_event(
    on_metrics_modified_rx: &mut watch::Receiver<()>,
    store: &Store,
    finish_rx: &mut watch::Receiver<()>,
) -> Event {
    select! {
        () = store.backoff.sleep() => Event::ProcessOneRecord,
        result = on_metrics_modified_rx.changed() => {
            match result {
                Ok(()) => Event::MetricsModified,
                Err(_) => Event::Exit,
            }
        }
        _result = finish_rx.changed() => Event::Exit,
        () = time::sleep_until(store.record_number.increment_at()) => Event::IncrementRecordNumber,
        () = time::sleep(store.device_id.rotate_after()) => Event::RotateDeviceId,
    }
}

pub async fn metrics_runner(
    metrics: Arc<Metrics>,
    store_path: PathBuf,
    processor: RecordProcessor,
    mut finish_rx: watch::Receiver<()>,
) -> Result<(), MetricsRunnerError> {
    let mut store = Store::new(store_path).await?;

    let mut on_metrics_modified_rx = metrics.subscribe();

    let mut oldest_record = None;

    loop {
        let event = on_event(&mut on_metrics_modified_rx, &store, &mut finish_rx).await;

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
                    log::debug!("Nothing to process");
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
                log::debug!("Event::MetricsModified");
                let record = metrics.make_record_data();
                store.store_record(record).await?;
                store.backoff.resume();
            }
            Event::RotateDeviceId => {
                log::debug!("Event::RotateDeviceId");
                store.record_number.reset().await?;
                store.device_id.rotate().await?;
            }
            Event::IncrementRecordNumber => {
                log::debug!("Event::IncrementRecordNumber");
                let record = metrics.make_record_data();
                store.store_record(record).await?;
                store.backoff.resume();
                store.record_number.increment().await?;
            }
            Event::Exit => {
                log::debug!("Event::Exit");
                let record = metrics.make_record_data();
                store.store_record(record).await?;
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
