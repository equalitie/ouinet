use serde_json::json;
use std::{
    io,
    path::{Path, PathBuf},
    time::{Duration, SystemTime},
};
use tokio::fs;
use uuid::Uuid;

const ROTATE_UUID_AFTER: Duration = Duration::from_secs(60 * 60 * 24 * 7);

pub struct UuidRotator {
    current_uuid: Uuid,
    created: SystemTime,
    file_path: PathBuf,
}

impl UuidRotator {
    pub async fn new(file_path: PathBuf) -> io::Result<Self> {
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

    pub async fn update(&mut self) -> io::Result<Uuid> {
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

        fs::write(file_path, json!((uuid, now)).to_string()).await?;

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
