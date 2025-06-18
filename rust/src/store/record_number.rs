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

        let now = Utc::now();

        let interval = match WholeHour::try_from(Utc::now()) {
            Ok(interval) => interval,
            Err(error) => {
                return Err(io::Error::other(format!(
                    "RecordNumber::new failed to construct WholeHour from {now:?}: {error:?}"
                )))
            }
        };

        Ok(Self {
            number,
            interval,
            file_path,
        })
    }

    pub fn get(&self) -> u32 {
        self.number
    }

    pub(super) async fn increment(&mut self) -> io::Result<()> {
        let now = Utc::now();
        self.number += 1;
        self.interval = match WholeHour::try_from(now) {
            Ok(interval) => interval,
            Err(error) => {
                return Err(io::Error::other(format!(
                    "RecordNumber::increment failed to construct WholeHour from {now:?}: {error:?}"
                )))
            }
        };
        self.store().await
    }

    pub(super) async fn reset(&mut self) -> io::Result<()> {
        let now = Utc::now();
        self.number = 0;
        self.interval = match WholeHour::try_from(now) {
            Ok(interval) => interval,
            Err(error) => {
                return Err(io::Error::other(format!(
                    "RecordNumber::reset failed to construct WholeHour from {now:?}: {error:?}"
                )))
            }
        };
        self.store().await
    }

    pub fn increment_after(&self) -> Duration {
        crate::period::duration_to_end(Utc::now(), self.interval.start(), self.interval.end())
    }

    async fn store(&self) -> io::Result<()> {
        Store::write(&self.file_path, &self.number).await
    }
}
