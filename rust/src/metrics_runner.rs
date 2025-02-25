use crate::{
    backoff::Backoff,
    metrics::Metrics,
    record_processor::{RecordProcessor, RecordProcessorError},
    store::Store,
};
use std::{
    io,
    path::PathBuf,
    sync::Arc,
    time::{Duration, Instant},
};
use thiserror::Error;
use tokio::{select, sync::watch, time};
use uuid::Uuid;

const CHANGE_PART_AFTER_DURATION: Duration = Duration::from_secs(60 * 60 * 24);

enum Event {
    ProcessOneRecord,
    MetricsModified,
    ChangePart,
    Exit,
}

async fn on_event(
    on_metrics_modified_rx: &mut watch::Receiver<()>,
    backoff: &Backoff,
    change_part_at: Instant,
) -> Event {
    let change_part_sleep_duration = match change_part_at.checked_duration_since(Instant::now()) {
        Some(duration) => duration,
        None => return Event::ChangePart,
    };

    select! {
        () = backoff.sleep() => Event::ProcessOneRecord,
        result = on_metrics_modified_rx.changed() => {
            match result {
                Ok(()) => Event::MetricsModified,
                Err(_) => Event::Exit,
            }
        }
        () = time::sleep(change_part_sleep_duration) => Event::ChangePart,
    }
}

pub async fn metrics_runner(
    metrics: Arc<Metrics>,
    store_path: PathBuf,
    processor: RecordProcessor,
) -> Result<(), MetricsRunnerError> {
    let runtime_id = Uuid::new_v4();
    let mut part = 0;

    let mut store = Store::new(store_path, runtime_id).await?;

    let mut on_metrics_modified_rx = metrics.subscribe();

    let mut backoff = store.backoff().await?;
    let mut oldest_record = None;
    let mut change_part_at = Instant::now() + CHANGE_PART_AFTER_DURATION;

    loop {
        let event = on_event(&mut on_metrics_modified_rx, &backoff, change_part_at).await;

        match event {
            Event::ProcessOneRecord => {
                log::debug!("metrics_runner::ProcessOneRecord");
                if oldest_record.is_none() {
                    let records = store.load_stored_records().await?;
                    oldest_record = records
                        .into_iter()
                        .filter(|record| !record.is_current(runtime_id, part))
                        .min_by(|l, r| l.created.cmp(&r.created));
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
                log::debug!("metrics_runner::MetricsModified");
                let record = metrics.make_record_data();
                store.store_record(record, part).await?;
                backoff.resume();
            }
            Event::ChangePart => {
                log::debug!("metrics_runner::ChangePart");
                let record = metrics.make_record_data();
                store.store_record(record, part).await?;
                backoff.resume();
                change_part_at = Instant::now() + CHANGE_PART_AFTER_DURATION;
                part += 1;
            }
            Event::Exit => break Ok(()),
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
