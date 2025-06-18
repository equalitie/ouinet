use tokio::time::Duration;

pub const RECORD_VERSION: u32 = 0;
pub const RECORD_FILE_EXTENSION: &str = "record";
/// Records older than this duration will be deleted.
pub const DELETE_RECORDS_AFTER: Duration = Duration::from_secs(60 * 60 * 24 * 7);

pub const ROTATE_DEVICE_ID_AFTER: Duration = Duration::from_secs(60 * 60 * 24 * 7);

pub const RECORD_WRITE_CONSTANT_BACKOFF: Duration = Duration::from_secs(5);
