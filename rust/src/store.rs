use crate::{backoff::Backoff, constants, record_number::RecordNumber, uuid_rotator::UuidRotator};
use serde::{Deserialize, Serialize};
use serde_json::json;
use std::{
    ffi::OsStr,
    io,
    path::{Path, PathBuf},
    time::SystemTime,
};
use tokio::fs;
use uuid::Uuid;

pub struct Store {
    records_dir_path: PathBuf,
    pub record_number: RecordNumber,
    pub backoff: Backoff,
    device_id_rotator: UuidRotator,
}

impl Store {
    pub async fn new(root_path: PathBuf) -> io::Result<Self> {
        let records_dir_path = root_path.join("records");
        let device_id_path = root_path.join("device_id.json");
        let record_number_path = root_path.join("record_number.json");
        let backoff_path = root_path.join("backoff.json");

        fs::create_dir_all(&records_dir_path).await?;

        let record_number = RecordNumber::load(record_number_path).await?;
        let device_id_rotator =
            UuidRotator::new(device_id_path, constants::ROTATE_DEVICE_ID_AFTER).await?;
        let backoff = Backoff::new(backoff_path).await?;

        Ok(Self {
            records_dir_path,
            record_number,
            backoff,
            device_id_rotator,
        })
    }

    pub async fn write<D: Serialize>(path: &Path, data: &D) -> io::Result<()> {
        fs::write(path, &json!(data).to_string()).await
    }

    pub async fn read<D: for<'a> Deserialize<'a>>(path: &Path) -> io::Result<Option<D>> {
        match fs::read_to_string(path).await {
            Ok(string) => match serde_json::from_str::<D>(&string) {
                Ok(data) => Ok(Some(data)),
                Err(_) => {
                    log::warn!("Failed to parse file {path:?}, removing it");
                    fs::remove_file(path).await?;
                    Ok(None)
                }
            },
            Err(error) => {
                if error.kind() == io::ErrorKind::NotFound {
                    Ok(None)
                } else {
                    Err(error)
                }
            }
        }
    }

    pub async fn store_record(&mut self, record_data: String) -> io::Result<()> {
        // TODO: Store into '.tmp' file first and then rename?
        let device_id = self.current_device_id().await?;

        let record_number = *self.record_number;

        let record_name =
            Self::build_record_name(constants::RECORD_VERSION, device_id, record_number);
        let record_path = self.records_dir_path.join(format!(
            "{record_name}.{}",
            constants::RECORD_FILE_EXTENSION
        ));

        let content = StoredRecordContent {
            created: SystemTime::now(),
            data: record_data,
        };

        Self::write(&record_path, &content).await
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

            if path.extension() != Some(OsStr::new(constants::RECORD_FILE_EXTENSION)) {
                continue;
            }

            let Some(name) = path
                .file_stem()
                .and_then(|s| s.to_str())
                .map(str::to_string)
            else {
                continue;
            };

            let Some((version, device_id, record_number)) = Self::parse_record_name(&name) else {
                continue;
            };

            if version != constants::RECORD_VERSION {
                fs::remove_file(&path).await?;
                continue;
            }

            let content = match Self::read::<StoredRecordContent>(&path).await {
                Ok(None) => continue,
                Ok(Some(content)) => content,
                Err(error) => return Err(error),
            };

            let discard = match SystemTime::now().duration_since(content.created) {
                Ok(duration) => duration >= constants::DISCARD_RECORDS_AFTER,
                // System has moved time to prior to creating the record.
                Err(_) => true,
            };

            if discard {
                fs::remove_file(&path).await?;
                continue;
            }

            records.push(StoredRecord {
                device_id,
                record_number,
                path,
                created: content.created,
                data: content.data,
            });
        }

        Ok(records)
    }

    pub async fn current_device_id(&mut self) -> io::Result<Uuid> {
        self.device_id_rotator.update().await
    }

    fn build_record_name(version: u32, device_id: Uuid, record_number: u32) -> String {
        format!("v{version}_{device_id}_{record_number}")
    }

    fn parse_record_name(
        name: &str,
    ) -> Option<(
        u32,  /* version */
        Uuid, /* device_id */
        u32,  /* record_number */
    )> {
        let mut parts = name.split('_');

        let version_part = parts.next()?;
        let device_id_part = parts.next()?;
        let record_number_part = parts.next()?;

        if !version_part.starts_with('v') {
            return None;
        }
        let version: u32 = version_part[1..].parse().ok()?;

        let device_id = Uuid::parse_str(device_id_part).ok()?;
        let record_number = record_number_part.parse().ok()?;

        Some((version, device_id, record_number))
    }
}

#[derive(Debug)]
pub struct StoredRecord {
    pub device_id: Uuid,
    pub record_number: u32,
    pub path: PathBuf,
    pub created: SystemTime,
    pub data: String,
}

impl StoredRecord {
    pub async fn discard(&self) -> io::Result<()> {
        fs::remove_file(&self.path).await
    }

    pub fn name(&self) -> String {
        Store::build_record_name(
            constants::RECORD_VERSION,
            self.device_id,
            self.record_number,
        )
    }

    pub fn is_current(&self, device_id: Uuid, record_number: u32) -> bool {
        self.device_id == device_id && self.record_number == record_number
    }
}

#[derive(Serialize, Deserialize)]
struct StoredRecordContent {
    // TODO: Should we get this from file meta-data?
    created: SystemTime,
    data: String,
}
