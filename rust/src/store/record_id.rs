use crate::{
    constants, record_id::RecordId, store::device_id::DeviceId, store::record_number::RecordNumber,
};
use std::{io, path::PathBuf};
use tokio::sync::watch;

pub struct RecordIdStore {
    changed_tx: watch::Sender<RecordId>,
    device_id: DeviceId,
    sequence_number: RecordNumber,
}

impl RecordIdStore {
    pub async fn new(device_id_path: PathBuf, sequence_number_path: PathBuf) -> io::Result<Self> {
        let device_id = DeviceId::new(device_id_path, constants::ROTATE_DEVICE_ID_AFTER).await?;
        let sequence_number = RecordNumber::load(sequence_number_path).await?;

        let changed_tx = watch::Sender::new(RecordId {
            device_id: device_id.get(),
            sequence_number: sequence_number.get(),
        });

        Ok(Self {
            changed_tx,
            device_id,
            sequence_number,
        })
    }

    pub fn subscribe(&self) -> watch::Receiver<RecordId> {
        self.changed_tx.subscribe()
    }

    pub fn device_id(&self) -> &DeviceId {
        &self.device_id
    }

    pub fn sequence_number(&self) -> &RecordNumber {
        &self.sequence_number
    }

    pub async fn increment(&mut self) -> io::Result<()> {
        self.sequence_number.increment().await?;
        self.update_current();
        Ok(())
    }

    pub async fn rotate(&mut self) -> io::Result<()> {
        self.device_id.rotate().await?;
        self.sequence_number.reset().await?;
        self.update_current();
        Ok(())
    }

    fn update_current(&mut self) {
        self.changed_tx.send_replace(RecordId {
            device_id: self.device_id.get(),
            sequence_number: self.sequence_number.get(),
        });
    }
}
