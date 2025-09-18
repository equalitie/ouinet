use env_logger::Env;

pub fn init_idempotent() {
    // TODO: Pass log level from C++ code
    env_logger::Builder::from_env(Env::default().default_filter_or("debug"))
        .format_timestamp(None)
        .try_init()
        .ok(); // ignore if already initialized
}
