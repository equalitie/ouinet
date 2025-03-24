use crate::{
    backoff::Backoff,
    constants,
    crypto::{self, EncryptionKey},
    device_id::DeviceId,
    record_number::RecordNumber,
};
use serde::{Deserialize, Serialize};
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
    encryption_key: EncryptionKey,
    pub record_number: RecordNumber,
    pub backoff: Backoff,
    pub device_id: DeviceId,
}

impl Store {
    pub async fn new(root_path: PathBuf, encryption_key: EncryptionKey) -> io::Result<Self> {
        let records_dir_path = root_path.join("records");
        let device_id_path = root_path.join("device_id.json");
        let record_number_path = root_path.join("record_number.json");
        let backoff_path = root_path.join("backoff.json");

        let record_number = RecordNumber::load(record_number_path).await?;
        let device_id = DeviceId::new(device_id_path, constants::ROTATE_DEVICE_ID_AFTER).await?;
        let backoff = Backoff::new(backoff_path).await?;

        Ok(Self {
            records_dir_path,
            encryption_key,
            record_number,
            backoff,
            device_id,
        })
    }

    pub async fn write<D: Serialize>(path: &Path, data: &D) -> io::Result<()> {
        let content = serde_json::to_vec(data)?;

        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).await?;
        }

        fs::write(path, &content).await
    }

    pub async fn read<D: for<'a> Deserialize<'a>>(path: &Path) -> io::Result<Option<D>> {
        match fs::read(path).await {
            Ok(content) => match serde_json::from_slice(&content) {
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

    pub async fn oldest_non_current_record(&self) -> io::Result<Option<StoredRecord>> {
        let current_device_id = self.device_id.get();
        let current_record_number = self.record_number.get();

        let record = self
            .load_stored_records()
            .await?
            .into_iter()
            .filter(|record| !record.is_current(current_device_id, current_record_number))
            .min_by(|l, r| l.created.cmp(&r.created));

        Ok(record)
    }

    pub async fn store_record(&self, record_data: String) -> io::Result<()> {
        // TODO: Store into '.tmp' file first and then rename?
        let device_id = self.device_id.get();
        let record_number = self.record_number.get();

        let record_name =
            Self::build_record_name(constants::RECORD_VERSION, device_id, record_number);
        let record_path = self.records_dir_path.join(format!(
            "{record_name}.{}",
            constants::RECORD_FILE_EXTENSION
        ));

        let content = crypto::encrypt(&self.encryption_key, record_data.as_bytes())
            .map_err(io::Error::other)?;

        if let Some(parent) = record_path.parent() {
            fs::create_dir_all(parent).await?;
        }

        fs::write(record_path, &content).await
    }

    pub async fn load_stored_records(&self) -> io::Result<Vec<StoredRecord>> {
        let mut entries = match fs::read_dir(&self.records_dir_path).await {
            Ok(entries) => entries,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(Vec::new()),
            Err(error) => return Err(error),
        };

        let mut records = Vec::new();

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

            let created = entry.metadata().await?.created()?;
            let content = fs::read(&path).await?;

            let delete = match created.elapsed() {
                Ok(duration) => duration >= constants::DELETE_RECORDS_AFTER,
                // System has moved time to prior to creating the record.
                Err(_) => true,
            };

            if delete {
                fs::remove_file(&path).await?;
                continue;
            }

            records.push(StoredRecord {
                device_id,
                record_number,
                path,
                created,
                content,
            });
        }

        Ok(records)
    }

    pub async fn delete_stored_records(&self) -> io::Result<()> {
        match fs::remove_dir_all(&self.records_dir_path).await {
            Ok(()) => Ok(()),
            Err(error) => {
                if error.kind() == io::ErrorKind::NotFound {
                    Ok(())
                } else {
                    Err(error)
                }
            }
        }
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
    pub content: Vec<u8>,
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
    #[serde(with = "serde_bytes")]
    data: Vec<u8>,
}
