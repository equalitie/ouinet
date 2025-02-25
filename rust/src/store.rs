use crate::{backoff::Backoff, uuid_rotator::UuidRotator};
use serde::{Deserialize, Serialize};
use serde_json::json;
use std::{
    ffi::OsStr,
    io,
    path::PathBuf,
    time::{Duration, SystemTime},
};
use tokio::fs;
use uuid::Uuid;

const RECORD_VERSION: u32 = 0;
const RECORD_FILE_EXTENSION: &str = "record";
const DISCARD_RECORDS_AFTER: Duration = Duration::from_secs(60 * 60 * 24 * 7);

pub struct Store {
    runtime_id: Uuid,
    root_path: PathBuf,
    records_dir_path: PathBuf,
    uuid_rotator: UuidRotator,
}

impl Store {
    pub async fn new(root_path: PathBuf, runtime_id: Uuid) -> io::Result<Self> {
        let records_dir_path = root_path.join("records");
        let uuid_file_path = root_path.join("uuid.json");

        fs::create_dir_all(&records_dir_path).await?;

        let uuid_rotator = UuidRotator::new(uuid_file_path).await?;

        Ok(Self {
            runtime_id,
            root_path,
            records_dir_path,
            uuid_rotator,
        })
    }

    // TODO: Would be nice to ensure Backoff is created only once.
    pub async fn backoff(&self) -> io::Result<Backoff> {
        Backoff::new(self.root_path.join("backoff.json")).await
    }

    pub async fn store_record(&mut self, record_data: String, part: u32) -> io::Result<()> {
        // TODO: Store into '.tmp' file first and then rename?
        let device_id = self.current_device_id().await?;

        let record_name = Self::build_record_name(RECORD_VERSION, device_id, self.runtime_id, part);
        let record_path = self
            .records_dir_path
            .join(format!("{record_name}.{RECORD_FILE_EXTENSION}"));

        let content = StoredRecordContent {
            created: SystemTime::now(),
            data: record_data,
        };

        tokio::fs::write(&record_path, &json!(content).to_string()).await?;

        Ok(())
    }

    pub async fn load_stored_records(&self) -> io::Result<Vec<StoredRecord>> {
        let mut entries = fs::read_dir(&self.records_dir_path).await?;

        let mut records = Vec::new();

        // TODO: Discard old records?
        while let Some(entry) = entries.next_entry().await? {
            if !entry.file_type().await?.is_file() {
                continue;
            }

            let path = entry.path();

            if path.extension() != Some(OsStr::new(RECORD_FILE_EXTENSION)) {
                continue;
            }

            let Some(name) = path
                .file_stem()
                .and_then(|s| s.to_str())
                .map(str::to_string)
            else {
                continue;
            };

            let Some((version, device_id, runtime_id, part)) = Self::parse_record_name(&name)
            else {
                continue;
            };

            if version != RECORD_VERSION {
                fs::remove_file(&path).await?;
                continue;
            }

            let Ok(content) =
                serde_json::from_str::<StoredRecordContent>(&fs::read_to_string(&path).await?)
            else {
                // Possibly incomplete write before the app terminated.
                fs::remove_file(&path).await?;
                continue;
            };

            let discard = match SystemTime::now().duration_since(content.created) {
                Ok(duration) => duration >= DISCARD_RECORDS_AFTER,
                // System has moved time to prior to creating the record.
                Err(_) => true,
            };

            if discard {
                fs::remove_file(&path).await?;
                continue;
            }

            records.push(StoredRecord {
                device_id,
                runtime_id,
                part,
                path,
                created: content.created,
                data: content.data,
            });
        }

        Ok(records)
    }

    pub async fn current_device_id(&mut self) -> io::Result<Uuid> {
        self.uuid_rotator.update().await
    }

    fn build_record_name(version: u32, device_id: Uuid, runtime_id: Uuid, part: u32) -> String {
        format!("v{version}_{device_id}_{runtime_id}_{part}")
    }

    fn parse_record_name(name: &str) -> Option<(u32, Uuid, Uuid, u32)> {
        let mut parts = name.split('_');

        let version_part = parts.next()?;
        let device_id_part = parts.next()?;
        let runtime_id_part = parts.next()?;
        let part_part = parts.next()?;

        if !version_part.starts_with('v') {
            return None;
        }
        let version: u32 = version_part[1..].parse().ok()?;

        let device_id = Uuid::parse_str(device_id_part).ok()?;
        let runtime_id = Uuid::parse_str(runtime_id_part).ok()?;
        let part = part_part.parse().ok()?;

        Some((version, device_id, runtime_id, part))
    }
}

#[derive(Debug)]
pub struct StoredRecord {
    pub device_id: Uuid,
    pub runtime_id: Uuid,
    pub part: u32,
    pub path: PathBuf,
    pub created: SystemTime,
    pub data: String,
}

impl StoredRecord {
    pub async fn discard(&self) -> io::Result<()> {
        fs::remove_file(&self.path).await
    }

    pub fn name(&self) -> String {
        Store::build_record_name(RECORD_VERSION, self.device_id, self.runtime_id, self.part)
    }

    pub fn is_current(&self, runtime_id: Uuid, part: u32) -> bool {
        self.runtime_id == runtime_id && self.part == part
    }
}

#[derive(Serialize, Deserialize)]
struct StoredRecordContent {
    // TODO: Should we get this from file meta-data?
    created: SystemTime,
    data: String,
}
