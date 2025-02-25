use crate::{
    metrics::Metrics,
    record_processor::{RecordProcessor, RecordProcessorError},
    store::Store,
};
use std::{io, path::PathBuf, sync::Arc};
use thiserror::Error;
use tokio::select;

pub async fn metrics_runner(
    metrics: Arc<Metrics>,
    store_path: PathBuf,
    processor: RecordProcessor,
) -> Result<(), MetricsRunnerError> {
    let mut store = Store::new(store_path).await?;

    let mut on_metrics_modified_rx = metrics.subscribe();

    enum Event {
        ProcessOneRecord,
        MetricsModified,
    }

    let mut backoff = store.backoff().await?;
    let mut oldest_record = None;

    loop {
        let event = select! {
            () = backoff.sleep() => Event::ProcessOneRecord,
            result = on_metrics_modified_rx.changed() => {
                match result {
                    Ok(()) => Event::MetricsModified,
                    Err(_) => return Ok(()),
                }
            }
        };

        match event {
            Event::ProcessOneRecord => {
                println!(":::::::::: process one record");
                if oldest_record.is_none() {
                    let records = store.load_stored_records().await?;
                    oldest_record = records.into_iter().min_by(|l, r| l.created.cmp(&r.created));
                }

                let Some(record) = &oldest_record else {
                    backoff.stop();
                    continue;
                };

                if processor.process(&record).await? {
                    backoff.succeeded().await?;
                    record.discard().await?;
                    oldest_record = None;
                } else {
                    backoff.failed().await?;
                }
            }
            Event::MetricsModified => {
                println!(":::::::::: modified");
                let record = metrics.make_record_data();
                store.store_record(record).await?;
                backoff.resume();
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
