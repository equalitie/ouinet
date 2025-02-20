use serde::Serialize;
use serde_json::json;
use std::sync::Mutex;

#[derive(Clone, Copy)]
pub enum IpVersion {
    V4,
    V6,
}

pub struct Metrics {
    bootstraps_v4: Mutex<Vec<BootstrapState>>,
    bootstraps_v6: Mutex<Vec<BootstrapState>>,
}

impl Metrics {
    pub fn new() -> Self {
        Self {
            bootstraps_v4: Mutex::new(Vec::default()),
            bootstraps_v6: Mutex::new(Vec::default()),
        }
    }

    pub fn bootstrap_start(&self, ipv: IpVersion) -> usize {
        let mut bootstraps = match ipv {
            IpVersion::V4 => self.bootstraps_v4.lock().unwrap(),
            IpVersion::V6 => self.bootstraps_v6.lock().unwrap(),
        };

        bootstraps.push(BootstrapState::Started);
        bootstraps.len() - 1
    }

    pub fn bootstrap_finish(&self, id: usize, ipv: IpVersion, success: bool) {
        let mut bootstraps = match ipv {
            IpVersion::V4 => self.bootstraps_v4.lock().unwrap(),
            IpVersion::V6 => self.bootstraps_v6.lock().unwrap(),
        };

        bootstraps[id] = BootstrapState::Finished { success };
    }

    pub fn to_json(&self) -> serde_json::Value {
        let bv4 = self.bootstraps_v4.lock().unwrap();
        let bv6 = self.bootstraps_v6.lock().unwrap();

        json!({
            "bootstraps_v4": *bv4,
            "bootstraps_v6": *bv6,
        })
    }
}

#[derive(Debug, Serialize)]
enum BootstrapState {
    Started,
    Finished { success: bool },
}
