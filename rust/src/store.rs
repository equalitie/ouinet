use crate::backoff::Backoff;
use serde::{Deserialize, Serialize};
use serde_json::json;
use std::{
    ffi::OsStr,
    io,
    path::{Path, PathBuf},
    time::SystemTime,
};
use tokio::{fs, time::Duration};
use uuid::Uuid;

const RECORD_VERSION: u32 = 0;
const RECORD_FILE_EXTENSION: &str = "record";
const ROTATE_UUID_AFTER: Duration = Duration::from_secs(60 * 60 * 24 * 7);

pub struct Store {
    root_path: PathBuf,
    records_dir_path: PathBuf,
    uuid_rotator: UuidRotator,
}

impl Store {
    pub async fn new(root_path: PathBuf) -> io::Result<Self> {
        let records_dir_path = root_path.join("records");
        let uuid_file_path = root_path.join("uuid.json");

        fs::create_dir_all(&records_dir_path).await?;

        let uuid_rotator = UuidRotator::new(uuid_file_path, ROTATE_UUID_AFTER).await?;

        Ok(Self {
            root_path,
            records_dir_path,
            uuid_rotator,
        })
    }

    // TODO: Would be nice to ensure Backoff is created only once.
    pub async fn backoff(&self) -> io::Result<Backoff> {
        Backoff::new(self.root_path.join("backoff.json")).await
    }

    pub async fn store_record(&mut self, record_data: String) -> io::Result<()> {
        // TODO: Store into '.tmp' file first and then rename?
        let uuid = self.current_uuid().await?;

        let record_name = Self::build_record_name(RECORD_VERSION, uuid);
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

            let Some((version, uuid)) = Self::parse_record_name(&name) else {
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

            records.push(StoredRecord {
                uuid,
                path,
                created: content.created,
                data: content.data,
            });
        }

        Ok(records)
    }

    pub async fn current_uuid(&mut self) -> io::Result<Uuid> {
        self.uuid_rotator.update().await
    }

    fn build_record_name(version: u32, uuid: Uuid) -> String {
        format!("v{version}_{uuid}")
    }

    fn parse_record_name(name: &str) -> Option<(u32, Uuid)> {
        let mut parts = name.split('_');

        let version_part = parts.next()?;
        let uuid_part = parts.next()?;

        if !version_part.starts_with('v') {
            return None;
        }
        let version: u32 = version_part[1..].parse().ok()?;

        let uuid = Uuid::parse_str(uuid_part).ok()?;

        Some((version, uuid))
    }
}

#[derive(Debug)]
pub struct StoredRecord {
    pub uuid: Uuid,
    pub path: PathBuf,
    pub created: SystemTime,
    pub data: String,
}

impl StoredRecord {
    pub async fn discard(&self) -> io::Result<()> {
        fs::remove_file(&self.path).await
    }

    pub fn name(&self) -> String {
        Store::build_record_name(RECORD_VERSION, self.uuid)
    }
}

#[derive(Serialize, Deserialize)]
struct StoredRecordContent {
    // TODO: Should we get this from file meta-data?
    created: SystemTime,
    data: String,
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
