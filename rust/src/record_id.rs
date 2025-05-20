use uuid::Uuid;

#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Copy, Clone)]
pub struct RecordId {
    pub device_id: Uuid,
    pub sequence_number: u32,
}

impl RecordId {
    pub fn to_string(&self) -> String {
        format!("{}_{}", self.device_id, self.sequence_number)
    }
}
