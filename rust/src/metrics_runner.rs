use crate::{
    metrics::Metrics,
    record_processor::{RecordProcessor, RecordProcessorError},
    store::{Store, StoredRecord},
};
use std::{io, path::PathBuf, sync::Arc};
use thiserror::Error;
use tokio::{time, time::Duration};

const DAY: Duration = Duration::from_secs(60 * 60 * 24);
const ROTATE_UUID_AFTER: Duration = Duration::from_secs(60 * 60 * 24 * 7);

pub async fn metrics_runner(
    metrics: Arc<Metrics>,
    store_path: PathBuf,
    processor: RecordProcessor,
) -> Result<(), MetricsRunnerError> {
    let store = Store::new(store_path).await?;

    let processor = PersistentRecordProcessor { processor };

    while let Some(record) = store.get_next_pre_existing_record().await? {
        processor.process(record).await?;
    }

    let mut uuid_rotator = store.new_uuid_rotator(ROTATE_UUID_AFTER).await?;

    loop {
        time::sleep(Duration::from_secs(5)).await;

        let uuid = uuid_rotator.update().await?;

        let record = metrics.make_record();

        let record_name = format!("v0_{uuid}");
        let record = store.store_record(&record_name, record).await?;

        processor.process(record).await?;
    }
}

// Retries processing of a record with an exponential backoff untill succeeds. Discards the record
// on success.
struct PersistentRecordProcessor {
    processor: RecordProcessor,
}

impl PersistentRecordProcessor {
    async fn process(&self, record: StoredRecord) -> Result<(), MetricsRunnerError> {
        let mut delay_after_failure = Duration::from_secs(10);
        let max_delay = 2 * DAY;

        loop {
            if !self.processor.process(&record).await? {
                time::sleep(delay_after_failure).await;

                delay_after_failure += Self::random_up_to(2 * delay_after_failure);

                if delay_after_failure > max_delay {
                    delay_after_failure = max_delay + Self::random_up_to(DAY);
                }

                continue;
            }

            record.discard().await?;
            break Ok(());
        }
    }

    fn random_up_to(duration: Duration) -> Duration {
        use rand::Rng;
        Duration::from_secs(rand::rng().random_range(..duration.as_secs()))
    }
}

#[derive(Error, Debug)]
pub enum MetricsRunnerError {
    #[error("RecordProcessor error {0}")]
    RecordProcessor(#[from] RecordProcessorError),
    #[error("IO error {0}")]
    Io(#[from] io::Error),
}
