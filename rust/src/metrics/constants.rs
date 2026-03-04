use tokio::time::Duration;

pub const RECORD_VERSION: u32 = 0;
pub const RECORD_FILE_EXTENSION: &str = "record";
/// Records older than this duration will be deleted.
pub const RECORD_WRITE_CONSTANT_BACKOFF: Duration = Duration::from_secs(5);
