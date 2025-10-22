use std::sync::LazyLock;
use tokio::runtime::{Builder, Handle, Runtime};

static RUNTIME: LazyLock<Runtime> = LazyLock::new(|| {
    // TODO: Handle failures more gracefully than panicking.
    // TODO: Be explicit about the number of threads to use, the default is the number of CPU
    // cores. Which is likely an overkill for the current use.
    Builder::new_multi_thread()
        .enable_all()
        .build()
        .expect("failed to initialize async runtime")
});

/// Returns a handle to the global async runtime. The runtime is lazily created the first time this
/// function is called.
pub fn handle() -> &'static Handle {
    RUNTIME.handle()
}
