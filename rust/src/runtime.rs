use std::sync::{Arc, Mutex, Weak};
use tokio::runtime::{Builder, Runtime};

static RUNTIME: Mutex<Weak<Runtime>> = Mutex::new(Weak::new());

pub fn get_runtime() -> Arc<Runtime> {
    let mut lock = RUNTIME.lock().unwrap();

    if let Some(runtime) = lock.upgrade() {
        return runtime;
    }

    // TODO: Unwrap.
    // TODO: Be explicit about the number of threads to use, the default is the number of CPU
    // cores. Which is likely an overkill for the current use.
    let runtime = Builder::new_multi_thread().enable_all().build().unwrap();

    let runtime = Arc::new(runtime);

    *lock = Arc::downgrade(&runtime);

    runtime
}
