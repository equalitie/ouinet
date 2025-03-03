use crate::store::Store;
use serde_json::json;
use std::{
    io,
    ops::Deref,
    path::{Path, PathBuf},
    time::{Duration, SystemTime},
};
use tokio::fs;
use uuid::Uuid;

pub struct UuidRotator {
    current_uuid: Uuid,
    created: SystemTime,
    file_path: PathBuf,
    rotate_after: Duration,
}

impl UuidRotator {
    pub async fn new(file_path: PathBuf, rotate_after: Duration) -> io::Result<Self> {
        let (uuid, created) = match Store::read(&file_path).await? {
            Some(data) => data,
            None => Self::create_and_store(&file_path).await?,
        };

        Ok(Self {
            current_uuid: uuid,
            created,
            file_path,
            rotate_after,
        })
    }

    pub async fn rotate(&mut self) -> io::Result<()> {
        let (new_uuid, created) = Self::create_and_store(&self.file_path).await?;

        self.current_uuid = new_uuid;
        self.created = created;

        Ok(())
    }

    pub fn rotate_after(&self) -> Duration {
        match SystemTime::now().duration_since(self.created) {
            Ok(elapsed) => match self.rotate_after.checked_sub(elapsed) {
                Some(remaining) => remaining,
                None => Duration::ZERO,
            },
            Err(_) => Duration::ZERO,
        }
    }

    async fn create_and_store(file_path: &Path) -> io::Result<(Uuid, SystemTime)> {
        let uuid = Uuid::new_v4();
        let now = SystemTime::now();

        fs::write(file_path, json!((uuid, now)).to_string()).await?;

        Ok((uuid, now))
    }
}

impl Deref for UuidRotator {
    type Target = Uuid;

    fn deref(&self) -> &Self::Target {
        &self.current_uuid
    }
}
