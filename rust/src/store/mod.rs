mod backoff;
mod device_id;
mod record_id;
mod record_number;

use crate::{
    constants,
    crypto::{self, EncryptionKey},
    record_id::RecordId,
};
use backoff::Backoff;
use record_id::RecordIdStore;
use serde::{Deserialize, Serialize};
use std::{
    ffi::OsStr,
    io,
    path::{Path, PathBuf},
    time::{Duration, SystemTime},
};
use tokio::fs::{self, File};
use uuid::Uuid;

pub struct Store {
    records_dir_path: PathBuf,
    encryption_key: EncryptionKey,
    pub record_id: RecordIdStore,
    pub backoff: Backoff,
}

impl Store {
    pub async fn new(root_path: PathBuf, encryption_key: EncryptionKey) -> io::Result<Self> {
        let records_dir_path = root_path.join("records");
        let device_id_path = root_path.join("device_id.json");
        let record_number_path = root_path.join("record_number.json");
        let backoff_path = root_path.join("backoff.json");

        fs::create_dir_all(&records_dir_path).await?;

        let record_id = RecordIdStore::new(device_id_path, record_number_path).await?;
        let backoff = Backoff::new(backoff_path).await?;

        Ok(Self {
            records_dir_path,
            encryption_key,
            record_id,
            backoff,
        })
    }

    pub async fn write<D: Serialize>(path: &Path, data: &D) -> io::Result<()> {
        let content = serde_json::to_vec(data)?;

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

    pub fn current_record_id(&self) -> RecordId {
        RecordId {
            device_id: self.record_id.device_id().get(),
            sequence_number: self.record_id.sequence_number().get(),
        }
    }

    pub async fn store_record(&self, record_data: String) -> io::Result<()> {
        // TODO: Store into '.tmp' file first and then rename?
        let current_record_id = self.current_record_id();

        let record_name = Self::build_record_name(constants::RECORD_VERSION, current_record_id);
        let record_path = self.records_dir_path.join(format!(
            "{record_name}.{}",
            constants::RECORD_FILE_EXTENSION
        ));

        let encrypted_record_data = crypto::encrypt(&self.encryption_key, record_data.as_bytes())
            .map_err(io::Error::other)?;

        if let Some(parent) = record_path.parent() {
            fs::create_dir_all(parent).await?;
        }

        Self::write_record_content(&record_path, encrypted_record_data).await
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
                log::warn!("Failed to parse record name {name:?}, removing it");
                fs::remove_file(&path).await?;
                continue;
            };

            if version != constants::RECORD_VERSION {
                log::warn!(
                    "Record {name:?} has incompatible version {}, removing it",
                    constants::RECORD_VERSION
                );
                fs::remove_file(&path).await?;
                continue;
            }

            let Some((created, content)) = Self::read_record_content(&path).await? else {
                log::warn!("Failed to parse record {path:?}, removing it");
                fs::remove_file(&path).await?;
                continue;
            };

            match created.elapsed() {
                Ok(duration) => {
                    if duration >= constants::DELETE_RECORDS_AFTER {
                        log::warn!("Record {name:?} is too old ({duration:?}), removing it");
                        fs::remove_file(&path).await?;
                        continue;
                    }
                }
                // System has moved time to prior to creating the record.
                Err(_) => {
                    log::warn!(
                        "Record {name:?} has invalid creation time (created:{created:?}, now:{:?}), removing it",
                        SystemTime::now()
                    );
                    fs::remove_file(&path).await?;
                    continue;
                }
            };

            records.push(StoredRecord {
                id: RecordId {
                    device_id,
                    sequence_number: record_number,
                },
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

    pub fn build_record_name(version: u32, record_id: RecordId) -> String {
        format!("v{version}_{record_id}")
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

    async fn write_record_content(
        file_path: &Path,
        encrypted_record_data: Vec<u8>,
    ) -> io::Result<()> {
        use tokio::io::AsyncWriteExt;

        let mut file = File::create(file_path).await?;
        let created = SystemTime::now();
        let timestamp_ms = created
            .duration_since(SystemTime::UNIX_EPOCH)
            .map(|d| d.as_millis())
            .unwrap_or(0);
        file.write_all(format!("{timestamp_ms}\n").as_bytes())
            .await?;
        file.write_all(&encrypted_record_data).await?;
        file.flush().await
    }

    async fn read_record_content(file_path: &Path) -> io::Result<Option<(SystemTime, Vec<u8>)>> {
        use tokio::io::{AsyncBufReadExt, AsyncReadExt, BufReader};

        let file = File::open(file_path).await?;
        let mut reader = BufReader::new(file);
        let mut line = String::new();
        reader.read_line(&mut line).await?;
        line.pop(); // Remove trailing new line
        let Ok(timestamp_ms) = line.parse::<u64>() else {
            log::warn!("Failed to parse string {line:?} as u64");
            return Ok(None);
        };
        let Some(created) = SystemTime::UNIX_EPOCH.checked_add(Duration::from_millis(timestamp_ms))
        else {
            log::warn!("Failed add {timestamp_ms} to UNIX_EPOCH");
            return Ok(None);
        };
        let mut encrypted_record_data = Vec::new();
        reader.read_to_end(&mut encrypted_record_data).await?;
        Ok(Some((created, encrypted_record_data)))
    }
}

#[derive(Debug)]
pub struct StoredRecord {
    pub id: RecordId,
    pub path: PathBuf,
    pub created: SystemTime,
    pub content: Vec<u8>,
}

impl StoredRecord {
    pub async fn discard(&self) -> io::Result<()> {
        fs::remove_file(&self.path).await
    }

    pub fn name(&self) -> String {
        Store::build_record_name(constants::RECORD_VERSION, self.id)
    }
}
