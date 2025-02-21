use crate::metrics::Record;
use serde_json::json;
use std::{
    io,
    path::{Path, PathBuf},
    time::SystemTime,
};
use tokio::{fs, time::Duration};
use uuid::Uuid;

pub struct Store {
    records_dir_path: PathBuf,
    uuid_file_path: PathBuf,
}

impl Store {
    pub async fn new(root_path: PathBuf) -> io::Result<Self> {
        let records_dir_path = root_path.join("records");
        let uuid_file_path = root_path.join("uuid.json");

        fs::create_dir_all(&records_dir_path).await?;

        Ok(Self {
            records_dir_path,
            uuid_file_path,
        })
    }
}

impl Store {
    pub async fn new_uuid_rotator(&self, rotate_after: Duration) -> io::Result<UuidRotator> {
        UuidRotator::new(self.uuid_file_path.clone(), rotate_after).await
    }

    pub async fn store_record(
        &self,
        record_name: &str,
        record: Record,
    ) -> io::Result<StoredRecord> {
        let record_path = self.records_dir_path.join(format!("{record_name}.record"));
        tokio::fs::write(&record_path, &json!(record).to_string()).await?;
        Ok(StoredRecord {
            name: record_name.into(),
            path: record_path,
            record,
        })
    }

    pub async fn get_next_pre_existing_record(&self) -> io::Result<Option<StoredRecord>> {
        let mut entries = fs::read_dir(&self.records_dir_path).await?;

        // TODO: Remove records which can't be parsed?
        // TODO: Discard old records?
        while let Some(entry) = entries.next_entry().await? {
            if !entry.file_type().await?.is_file() {
                continue;
            }

            let path = entry.path();

            let Some(name) = path
                .file_stem()
                .and_then(|s| s.to_str())
                .map(str::to_string)
            else {
                continue;
            };

            let Ok(record) = serde_json::from_str(&fs::read_to_string(&path).await?) else {
                continue;
            };

            return Ok(Some(StoredRecord { name, path, record }));
        }

        Ok(None)
    }
}

#[derive(Debug)]
pub struct StoredRecord {
    pub name: String,
    pub path: PathBuf,
    pub record: Record,
}

impl StoredRecord {
    pub async fn discard(&self) -> io::Result<()> {
        fs::remove_file(&self.path).await
    }
}

pub struct UuidRotator {
    current_uuid: Uuid,
    created: SystemTime,
    file_path: PathBuf,
    rotate_after: Duration,
}

impl UuidRotator {
    async fn new(file_path: PathBuf, rotate_after: Duration) -> io::Result<Self> {
        let (uuid, created) = match Self::load(&file_path, rotate_after).await {
            Some(pair) => pair,
            None => Self::create_and_store(&file_path).await?,
        };

        Ok(Self {
            current_uuid: uuid,
            created,
            file_path,
            rotate_after,
        })
    }

    pub async fn update(&mut self) -> io::Result<Uuid> {
        if !Self::requires_rotation(self.created, self.rotate_after) {
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

        fs::write(file_path, json!((uuid, now)).to_string()).await?;

        Ok((uuid, now))
    }

    async fn load(file_path: &Path, rotate_after: Duration) -> Option<(Uuid, SystemTime)> {
        let (uuid, created): (Uuid, SystemTime) = match fs::read_to_string(&file_path).await {
            Ok(string) => match serde_json::from_str(&string) {
                Ok(entry) => entry,
                Err(_) => return None,
            },
            Err(_) => return None,
        };

        if Self::requires_rotation(created, rotate_after) {
            return None;
        }

        Some((uuid, created))
    }

    fn requires_rotation(created: SystemTime, rotate_after: Duration) -> bool {
        let now = SystemTime::now();

        if let Ok(elapsed) = now.duration_since(created) {
            if elapsed < rotate_after {
                return false;
            }
        } else {
            log::warn!("Uuid created in the future");
        }

        true
    }
}
