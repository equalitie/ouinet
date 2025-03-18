use env_logger::Env;

pub fn init_idempotent() {
    env_logger::Builder::from_env(Env::default().default_filter_or("info"))
        .format_timestamp(None)
        .try_init()
        .ok(); // ignore if already initialized
}
