use crate::{
    metrics::Metrics,
    record_processor::{RecordProcessor, RecordProcessorError},
    store::Store,
};
use std::{io, path::PathBuf, sync::Arc};
use thiserror::Error;
use tokio::{time, time::Duration};

const ROTATE_UUID_AFTER: Duration = Duration::from_secs(60 * 60 * 24 * 7); // One week

pub async fn metrics_runner(
    metrics: Arc<Metrics>,
    store_path: PathBuf,
    processor: RecordProcessor,
) -> Result<(), MetricsRunnerError> {
    let store = Store::new(store_path).await?;

    while let Some(record) = store.get_next_pre_existing_record().await? {
        let success = processor.process(&record).await?;

        if success {
            record.discard().await?;
        }
    }

    let mut uuid_rotator = store.new_uuid_rotator(ROTATE_UUID_AFTER).await?;

    loop {
        time::sleep(Duration::from_secs(5)).await;

        let uuid = uuid_rotator.update().await?;

        let record = metrics.make_record();

        let record_name = format!("v0_{uuid}");
        let record = store.store_record(&record_name, record).await?;

        let success = processor.process(&record).await?;

        if success {
            record.discard().await?;
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
