use env_logger::Env;
use std::sync::atomic::{AtomicBool, Ordering};

static INITIALIZED: AtomicBool = AtomicBool::new(false);

pub fn init_idempotent() {
    if INITIALIZED.fetch_or(true, Ordering::Relaxed) {
        return;
    }

    env_logger::Builder::from_env(Env::default().default_filter_or("info"))
        .format_timestamp(None)
        .init();
}
