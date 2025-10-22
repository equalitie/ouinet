mod auxiliary;
mod bootstrap;
pub mod request;

use super::{
    backoff_watch::{ConstantBackoffWatchReceiver, ConstantBackoffWatchSender},
    constants,
    period::{WholeHour, WholeWeek},
};
use auxiliary::Auxiliary;
pub use bootstrap::{BootstrapId, Bootstraps};
use chrono::{Datelike, Timelike};
pub use request::Requests;
use serde_json::json;
use std::sync::Arc;

#[derive(Clone, Copy, Debug)]
pub enum IpVersion {
    V4,
    V6,
}

pub struct Collector {
    on_modify_tx: Arc<ConstantBackoffWatchSender>,
    bootstraps: Bootstraps,
    bridge: Bridge,
    pub requests: Requests,
    aux: Auxiliary,
    has_new_data: bool,
}

impl Collector {
    pub fn new() -> Self {
        let on_modify_tx = Arc::new(ConstantBackoffWatchSender::new(
            constants::RECORD_WRITE_CONSTANT_BACKOFF,
        ));

        Self {
            on_modify_tx: on_modify_tx.clone(),
            bootstraps: Bootstraps::new(),
            bridge: Default::default(),
            requests: Requests::new(on_modify_tx),
            aux: Auxiliary::new(),
            has_new_data: false,
        }
    }

    pub fn set_aux_key_value(&mut self, key: String, value: String) {
        if self.aux.set(key, value) {
            self.mark_modified(true);
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

    pub fn bridge_transfer_i2c(&mut self, byte_count: usize) {
        self.bridge.transfer_injector_to_client += byte_count as u64;
        self.mark_modified(true);
    }

    pub fn bridge_transfer_c2i(&mut self, byte_count: usize) {
        self.bridge.transfer_client_to_injector += byte_count as u64;
        self.mark_modified(true);
    }

    pub fn collect(&mut self, id_interval: WholeWeek, sq_interval: WholeHour) -> Option<String> {
        if !self.has_new_data() {
            return None;
        }

        let year = id_interval.start().iso_week().year();
        let week = id_interval.start().iso_week().week();
        let day = sq_interval.start().weekday().number_from_monday();
        let hour = sq_interval.start().hour();

        let data = json!({
            "os": std::env::consts::OS,
            "interval": format!("{:0>4}:{:0>2}:{:0>2}:{:0>2}", year, week, day, hour),
            "bridge_i2c": self.bridge.transfer_injector_to_client,
            "bridge_c2i": self.bridge.transfer_injector_to_client,
            "bootstraps": self.bootstraps,
            "requests": self.requests,
            "aux": self.aux,
        })
        .to_string();

        Some(data)
    }

    // Called when the device_id changes to not leak more data into the next record
    pub fn on_device_id_changed(&mut self) {
        self.bootstraps.on_device_id_changed();
        self.requests.on_device_id_changed();
        self.bridge.on_device_id_changed();
        self.aux.on_device_id_changed();
        self.mark_modified(false);
    }

    // Clear whatever metrics have started and finished, leave the unfinished records.
    pub fn on_record_sequence_number_changed(&mut self) {
        self.bootstraps.on_record_sequence_number_changed();
        self.requests.on_record_sequence_number_changed();
        self.bridge.on_record_sequence_number_changed();
        self.aux.on_record_sequence_number_changed();
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
        self.has_new_data || self.requests.has_new_data() || self.aux.has_new_data()
    }
}

#[derive(Default)]
struct Bridge {
    transfer_injector_to_client: u64,
    transfer_client_to_injector: u64,
}

impl Bridge {
    fn on_device_id_changed(&mut self) {
        self.clear();
    }

    fn on_record_sequence_number_changed(&mut self) {
        self.clear();
    }

    fn clear(&mut self) {
        self.transfer_injector_to_client = 0;
        self.transfer_client_to_injector = 0;
    }
}
