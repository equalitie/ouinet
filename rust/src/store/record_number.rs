use crate::{period::WholeHour, store::Store};
use chrono::Utc;
use std::{io, path::PathBuf};
use tokio::time::Duration;

/// We can have multiple records with the same DeviceId, and RecordNumber is used to disambiguate
/// between them. RecordNumber is incremented on app restart or after
/// INCREMENT_RECORD_VERSION_AFTER duration elapses. It is reset back to zero when DeviceId
/// changes.
pub struct RecordNumber {
    number: u32,
    interval: WholeHour,
    file_path: PathBuf,
}

impl RecordNumber {
    pub async fn load(file_path: PathBuf) -> io::Result<Self> {
        let number = match Store::read::<u32>(&file_path).await? {
            Some(number) => number + 1,
            None => 0,
        };

        Store::write(&file_path, &number).await?;

        Ok(Self {
            number,
            interval: WholeHour::from(Utc::now()),
            file_path,
        })
    }

    pub fn get(&self) -> u32 {
        self.number
    }

    pub(super) async fn increment(&mut self) -> io::Result<()> {
        self.number += 1;
        self.interval = self.interval.next();
        self.store().await
    }

    pub(super) async fn reset(&mut self) -> io::Result<()> {
        self.number = 0;
        self.interval = WholeHour::from(Utc::now());
        self.store().await
    }

    pub fn increment_after(&self) -> Duration {
        let now = Utc::now();
        let start = self.interval.start();
        let Some(end) = self.interval.end() else {
            log::warn!("RecordID's interval has overflown");
            return Duration::MAX;
        };
        if now < start {
            log::warn!("RecordID's interval starts before `now`");
            return Duration::ZERO;
        }
        let Ok(delta_ms) = end.signed_duration_since(now).num_milliseconds().try_into() else {
            // `now > end`
            return Duration::ZERO;
        };
        Duration::from_millis(delta_ms)
    }

    async fn store(&self) -> io::Result<()> {
        Store::write(&self.file_path, &self.number).await
    }
}
