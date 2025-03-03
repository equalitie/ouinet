use tokio::time::Duration;

pub const RECORD_VERSION: u32 = 0;
pub const RECORD_FILE_EXTENSION: &str = "record";
pub const DISCARD_RECORDS_AFTER: Duration = Duration::from_secs(60 * 60 * 24 * 7);
pub const INCREMENT_RECORD_VERSION_AFTER_DURATION: Duration = Duration::from_secs(60 * 60 * 24);
