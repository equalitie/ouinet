use crate::{period::WholeWeek, store::Store};
use chrono::{DateTime, Utc};
use serde_json::json;
use std::{
    io,
    path::{Path, PathBuf},
    time::Duration,
};
use tokio::fs;
use uuid::Uuid;

pub struct DeviceId {
    current: Uuid,
    interval: WholeWeek,
    file_path: PathBuf,
}

impl DeviceId {
    pub async fn new(file_path: PathBuf) -> io::Result<Self> {
        let (value, created) = match Store::read(&file_path).await? {
            Some(data) => data,
            None => Self::create_and_store(&file_path).await?,
        };

        let interval = match WholeWeek::try_from(created) {
            Ok(interval) => interval,
            Err(error) => {
                return Err(io::Error::other(format!(
                    "DeviceId::new failed to construct WholeWeek from {created:?}: {error:?}"
                )))
            }
        };

        let mut this = Self {
            current: value,
            interval,
            file_path,
        };

        if this.rotate_after() == Duration::ZERO {
            this.rotate().await?;
        }

        Ok(this)
    }

    pub fn get(&self) -> Uuid {
        self.current
    }

    pub(super) async fn rotate(&mut self) -> io::Result<()> {
        let (new_value, created) = Self::create_and_store(&self.file_path).await?;

        self.current = new_value;
        self.interval = match WholeWeek::try_from(created) {
            Ok(interval) => interval,
            Err(error) => {
                return Err(io::Error::other(format!(
                    "DeviceId::rotate failed to construct WholeWeek from {created:?}: {error:?}"
                )))
            }
        };

        Ok(())
    }

    pub fn rotate_after(&self) -> Duration {
        crate::period::duration_to_end(Utc::now(), self.interval.start(), self.interval.end())
    }

    async fn create_and_store(file_path: &Path) -> io::Result<(Uuid, DateTime<Utc>)> {
        let uuid = Uuid::new_v4();
        let now = Utc::now();

        fs::write(file_path, json!((uuid, now)).to_string()).await?;

        Ok((uuid, now))
    }
}
