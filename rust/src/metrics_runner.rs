use crate::{metrics::Metrics, record_processor::RecordProcessor, store::Store};
use std::{io, path::PathBuf, sync::Arc};
use tokio::{fs, time, time::Duration};

const ROTATE_UUID_AFTER: Duration = Duration::from_secs(60 * 60 * 24 * 7); // One week

pub async fn metrics_runner(
    metrics: Arc<Metrics>,
    store_path: PathBuf,
    processor: RecordProcessor,
) -> io::Result<()> {
    let store = Store::new(store_path).await?;
    let mut uuid_rotator = store.new_uuid_rotator(ROTATE_UUID_AFTER).await?;

    loop {
        time::sleep(Duration::from_secs(5)).await;

        let uuid = uuid_rotator.update().await?;

        let record = metrics.make_record();

        let record_name = format!("v0_{uuid}");
        let record = store.store_record(&record_name, record).await?;

        let Some(success) = processor.process(&record).await else {
            // The C++ callback has been destroyed, could theoretically happen when the
            // asio::io_context is destroyed before the callback is finished.
            return Ok(());
        };

        if success {
            record.discard().await?;
        }
    }
}
