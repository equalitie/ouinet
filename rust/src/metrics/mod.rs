mod bootstrap;
pub mod request;

use crate::{
    backoff_watch::{ConstantBackoffWatchReceiver, ConstantBackoffWatchSender},
    constants,
};
pub use bootstrap::{BootstrapId, Bootstraps};
use chrono::{offset::Utc, DateTime};
pub use request::Requests;
use serde_json::json;
use std::time::SystemTime;

const DAY_TIME_FORMAT: &'static str = "%FT%T%.3f";

#[derive(Clone, Copy, Debug)]
pub enum IpVersion {
    V4,
    V6,
}

pub struct Metrics {
    start: DateTime<Utc>,
    record_start: DateTime<Utc>,
    on_modify_tx: ConstantBackoffWatchSender,
    bootstraps: Bootstraps,
    pub requests: Requests,
    has_new_data: bool,
}

impl Metrics {
    pub fn new() -> Self {
        let now = SystemTime::now().into();

        Self {
            start: now,
            record_start: now,
            on_modify_tx: ConstantBackoffWatchSender::new(constants::RECORD_WRITE_CONSTANT_BACKOFF),
            bootstraps: Bootstraps::new(),
            requests: Requests::new(),
            has_new_data: false,
        }
    }

    pub fn bootstrap_start(&mut self, ipv: IpVersion) -> BootstrapId {
        log::debug!("Metrics::bootstrap_start ipv:{ipv:?}");

        let id = self.bootstraps.start(ipv);
        self.mark_modified(true);
        id
    }

    pub fn bootstrap_finish(&mut self, id: BootstrapId, success: bool) {
        log::debug!("Metrics::bootstrap_finish id:{id:?} success:{success:?}");

        self.bootstraps.finish(id, success);
        self.mark_modified(true)
    }

    pub fn collect(&mut self) -> Option<String> {
        if !self.has_new_data() {
            return None;
        }

        let data = json!({
            "start": format!("{}", self.start.format(DAY_TIME_FORMAT)),
            "record_start": format!("{}", self.record_start.format(DAY_TIME_FORMAT)),
            "bootstraps": self.bootstraps,
        })
        .to_string();

        self.clear_finished();

        Some(data)
    }

    // Called when the device_id changes to not leak more data into the next record
    pub fn clear(&mut self) {
        let now = SystemTime::now().into();
        self.start = now;
        self.record_start = now;
        self.bootstraps.clear();
        self.mark_modified(false);
    }

    // Clear whatever metrics have started and finished, leave the unfinished records.
    fn clear_finished(&mut self) {
        self.record_start = SystemTime::now().into();
        self.bootstraps.clear_finished();
        self.mark_modified(false);
    }

    pub fn subscribe(&self) -> ConstantBackoffWatchReceiver {
        self.on_modify_tx.subscribe()
    }

    fn mark_modified(&mut self, new_data: bool) {
        self.has_new_data = new_data;
        self.on_modify_tx.send_modify(|_| {});
    }

    #[cfg(test)]
    pub fn modify(&mut self) {
        self.mark_modified(true);
    }

    pub fn has_new_data(&self) -> bool {
        self.has_new_data
    }
}
