use std::fmt;
use uuid::Uuid;

#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Copy, Clone)]
pub struct RecordId {
    pub device_id: Uuid,
    pub sequence_number: u32,
}

impl RecordId {
    pub fn nil() -> Self {
        Self {
            device_id: Uuid::nil(),
            sequence_number: 0,
        }
    }
}

impl fmt::Display for RecordId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}_{}", self.device_id, self.sequence_number)
    }
}
