use crate::{metrics::Metrics, record_processor::RecordProcessor};
use serde::{Deserialize, Serialize};
use serde_json::json;
use std::{
    io,
    path::{Path, PathBuf},
    sync::Arc,
    time::{SystemTime, UNIX_EPOCH},
};
use tokio::{fs, time, time::Duration};
use uuid::Uuid;

const ROTATE_UUID_AFTER: Duration = Duration::from_secs(60 * 60 * 24 * 7); // One week

pub async fn metrics_runner(
    metrics: Arc<Metrics>,
    store_path: PathBuf,
    processor: RecordProcessor,
) -> io::Result<()> {
    let records_dir_path = store_path.join("records");
    fs::create_dir_all(&records_dir_path).await?;

    let store = Store::new(store_path).await?;
    let mut uuid_rotator = UuidRotator::new(&store).await?;

    loop {
        time::sleep(Duration::from_secs(5)).await;

        let uuid = uuid_rotator.update().await?;
        let record_name = format!("v0_{uuid}.record");
        let record_path = records_dir_path.join(format!("{record_name}.record"));

        let json = metrics.to_json().to_string();
        println!(">>>>>>>>>> {record_path:?}");
        tokio::fs::write(&record_path, json.clone()).await.unwrap();

        let Some(success) = processor.process(record_name.clone(), json).await else {
            return Ok(());
        };
    }
}

struct Store {
    path: PathBuf,
}

impl Store {
    async fn new(path: PathBuf) -> io::Result<Self> {
        fs::create_dir_all(&path).await?;
        Ok(Self { path })
    }
}

struct UuidRotator {
    current_uuid: Uuid,
    created: SystemTime,
    file_path: PathBuf,
}

impl UuidRotator {
    async fn new(store: &Store) -> io::Result<Self> {
        let file_path = store.path.join("uuid.json");

        let (uuid, created) = match Self::load(&file_path).await {
            Some(pair) => pair,
            None => Self::create_and_store(&file_path).await?,
        };

        Ok(Self {
            current_uuid: uuid,
            created,
            file_path,
        })
    }

    async fn update(&mut self) -> io::Result<Uuid> {
        if !Self::requires_rotation(self.created) {
            return Ok(self.current_uuid);
        }

        let (new_uuid, created) = Self::create_and_store(&self.file_path).await?;

        self.current_uuid = new_uuid;
        self.created = created;

        Ok(self.current_uuid)
    }

    async fn create_and_store(file_path: &Path) -> io::Result<(Uuid, SystemTime)> {
        let uuid = Uuid::new_v4();
        let now = SystemTime::now();

        fs::write(
            file_path,
            json!((
                uuid,
                // Unwrap: something is seriously wrong if UNIX_EPOCH > now.
                now.duration_since(UNIX_EPOCH).unwrap().as_secs(),
            ))
            .to_string(),
        )
        .await?;

        Ok((uuid, now))
    }

    async fn load(file_path: &Path) -> Option<(Uuid, SystemTime)> {
        let (uuid, created): (Uuid, SystemTime) = match fs::read_to_string(&file_path).await {
            Ok(string) => match serde_json::from_str(&string) {
                Ok(entry) => entry,
                Err(_) => return None,
            },
            Err(_) => return None,
        };

        if Self::requires_rotation(created) {
            return None;
        }

        Some((uuid, created))
    }

    fn requires_rotation(created: SystemTime) -> bool {
        let now = SystemTime::now();

        if let Ok(elapsed) = now.duration_since(created) {
            if elapsed < ROTATE_UUID_AFTER {
                return false;
            }
        } else {
            log::warn!("Uuid created in the future");
        }

        true
    }
}
