use serde::Serialize;
use serde_json::json;
use std::sync::Mutex;
use tokio::sync::watch;

#[derive(Clone, Copy)]
pub enum IpVersion {
    V4,
    V6,
}

pub struct Metrics {
    on_modify_tx: watch::Sender<()>,
    bootstraps_v4: Mutex<Vec<BootstrapState>>,
    bootstraps_v6: Mutex<Vec<BootstrapState>>,
}

impl Metrics {
    pub fn new() -> Self {
        Self {
            on_modify_tx: watch::channel(()).0,
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
        let id = bootstraps.len() - 1;

        self.mark_modified();

        id
    }

    pub fn bootstrap_finish(&self, id: usize, ipv: IpVersion, success: bool) {
        let mut bootstraps = match ipv {
            IpVersion::V4 => self.bootstraps_v4.lock().unwrap(),
            IpVersion::V6 => self.bootstraps_v6.lock().unwrap(),
        };

        match bootstraps[id] {
            BootstrapState::Started => bootstraps[id] = BootstrapState::Finished { success },
            BootstrapState::Finished { .. } => (),
        }

        self.mark_modified()
    }

    pub fn make_record_data(&self) -> String {
        let bv4 = self.bootstraps_v4.lock().unwrap();
        let bv6 = self.bootstraps_v6.lock().unwrap();

        json!({
            "bootstraps_v4": bv4.clone(),
            "bootstraps_v6": bv6.clone(),
        })
        .to_string()
    }

    pub fn subscribe(&self) -> watch::Receiver<()> {
        self.on_modify_tx.subscribe()
    }

    fn mark_modified(&self) {
        self.on_modify_tx.send_modify(|_| {});
    }
}

#[derive(Debug, Serialize, Clone)]
enum BootstrapState {
    Started,
    Finished { success: bool },
}
