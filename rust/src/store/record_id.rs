use crate::{constants, store::device_id::DeviceId, store::record_number::RecordNumber};
use std::{io, path::PathBuf};

pub struct RecordIdStore {
    device_id: DeviceId,
    sequence_number: RecordNumber,
}

impl RecordIdStore {
    pub async fn new(device_id_path: PathBuf, sequence_number_path: PathBuf) -> io::Result<Self> {
        let device_id = DeviceId::new(device_id_path, constants::ROTATE_DEVICE_ID_AFTER).await?;
        let sequence_number = RecordNumber::load(sequence_number_path).await?;
        Ok(Self {
            device_id,
            sequence_number,
        })
    }

    pub fn device_id(&self) -> &DeviceId {
        &self.device_id
    }

    pub fn sequence_number(&self) -> &RecordNumber {
        &self.sequence_number
    }

    pub async fn sequence_number_increment(&mut self) -> io::Result<()> {
        self.sequence_number.increment().await
    }

    pub async fn sequence_number_reset(&mut self) -> io::Result<()> {
        self.sequence_number.reset().await
    }

    pub async fn device_id_rotate(&mut self) -> io::Result<()> {
        self.device_id.rotate().await
    }
}
