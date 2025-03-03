use crate::{constants, store::Store};
use std::{io, ops::Deref, path::PathBuf};
use tokio::time::Instant;

pub struct RecordNumber {
    number: u32,
    time: Instant,
    file_path: PathBuf,
}

impl RecordNumber {
    pub async fn load(file_path: PathBuf) -> io::Result<Self> {
        let number = match Store::read::<u32>(&file_path).await? {
            Some(number) => number + 1,
            None => {
                let number = 0;
                Store::write(&file_path, &number).await?;
                number
            }
        };

        Ok(Self {
            number,
            time: Instant::now(),
            file_path,
        })
    }

    pub async fn increment(&mut self) -> io::Result<()> {
        self.number += 1;
        self.time = Instant::now();
        self.store().await
    }

    pub async fn reset(&mut self) -> io::Result<()> {
        self.number = 0;
        self.time = Instant::now();
        self.store().await
    }

    pub fn increment_at(&self) -> Instant {
        self.time + constants::INCREMENT_RECORD_VERSION_AFTER_DURATION
    }

    async fn store(&self) -> io::Result<()> {
        Store::write(&self.file_path, &self.number).await
    }
}

impl Deref for RecordNumber {
    type Target = u32;

    fn deref(&self) -> &Self::Target {
        &self.number
    }
}
