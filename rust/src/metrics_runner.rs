use crate::{metrics::Metrics, record_processor::RecordProcessor};
use std::sync::Arc;
use tokio::{fs, time, time::Duration};
use uuid::Uuid;

pub async fn metrics_runner(metrics: Arc<Metrics>, store_path: String, processor: RecordProcessor) {
    let uuid = Uuid::new_v4();
    let record_name = format!("v0_{uuid}.record");
    let record_path = format!("{store_path}/{record_name}.record");

    fs::create_dir_all(&store_path).await.unwrap();

    loop {
        time::sleep(Duration::from_secs(10)).await;
        let json = metrics.to_json().to_string();
        tokio::fs::write(&record_path, json.clone()).await.unwrap();

        let Some(success) = processor.process(record_name.clone(), json).await else {
            break;
        };
    }
}
