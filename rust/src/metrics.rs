use chrono::{offset::Utc, DateTime};
use serde::Serialize;
use serde_json::json;
use std::{
    collections::{BTreeMap, HashMap},
    time::SystemTime,
};
use tokio::{
    sync::watch,
    time::{Duration, Instant},
};

const DAY_TIME_FORMAT: &'static str = "%FT%T%.3f";

#[derive(Clone, Copy, Debug)]
pub enum IpVersion {
    V4,
    V6,
}

#[derive(Clone, Copy)]
pub struct BootstrapId {
    ipv: IpVersion,
    id: usize,
}

pub struct Metrics {
    start: DateTime<Utc>,
    restart: Option<DateTime<Utc>>,
    on_modify_tx: watch::Sender<()>,
    bootstraps: Bootstraps,
}

impl Metrics {
    pub fn new() -> Self {
        Self {
            start: SystemTime::now().into(),
            restart: None,
            on_modify_tx: watch::channel(()).0,
            bootstraps: Bootstraps::new(),
        }
    }

    pub fn bootstrap_start(&mut self, ipv: IpVersion) -> BootstrapId {
        let id = self.bootstraps.start(ipv);
        self.mark_modified();
        id
    }

    pub fn bootstrap_finish(&mut self, id: BootstrapId, success: bool) {
        self.bootstraps.finish(id, success);
        self.mark_modified()
    }

    pub fn make_record_data(&self) -> String {
        let bv4 = &self.bootstraps.v4;
        let bv6 = &self.bootstraps.v6;

        json!({
            // For format see: https://docs.rs/chrono/0.4.0/chrono/format/strftime/index.html
            "start": format!("{}", self.start.format(DAY_TIME_FORMAT)),
            "restart": self.restart.map(|dt| format!("{}", dt.format(DAY_TIME_FORMAT))),
            "bootstrap_ipv4": {
                "histogram": ExponentialHistogram::new(200, 10, bv4.values().filter_map(|s| s.success_duration_ms())),
                "unfinished": bv4.values().filter(|s| s.is_started()).count(),
                "min": bv4.values().filter_map(|s| s.success_duration_ms()).min(),
                "max": bv4.values().filter_map(|s| s.success_duration_ms()).max(),
            },
            "bootstrap_ipv6": {
                "histogram": ExponentialHistogram::new(200, 10, bv6.values().filter_map(|s| s.success_duration_ms())),
                "unfinished": bv6.values().filter(|s| s.is_started()).count(),
                "min": bv6.values().filter_map(|s| s.success_duration_ms()).min(),
                "max": bv6.values().filter_map(|s| s.success_duration_ms()).max(),
            }
        })
        .to_string()
    }

    pub fn restart(&mut self) {
        self.bootstraps.clear_finished();
        self.bootstraps.clear_finished();
        self.restart = Some(SystemTime::now().into());

        self.mark_modified();
    }

    pub fn subscribe(&self) -> watch::Receiver<()> {
        self.on_modify_tx.subscribe()
    }

    fn mark_modified(&self) {
        self.on_modify_tx.send_modify(|_| {});
    }
}

#[derive(Debug, Clone)]
enum BootstrapState {
    Started { at: Instant },
    Finished { took: Duration, success: bool },
}

impl BootstrapState {
    fn success_duration_ms(&self) -> Option<u128> {
        match self {
            BootstrapState::Started { .. } => None,
            BootstrapState::Finished { took, success } => {
                if *success {
                    Some(took.as_millis())
                } else {
                    None
                }
            }
        }
    }

    fn is_started(&self) -> bool {
        match self {
            BootstrapState::Started { .. } => true,
            BootstrapState::Finished { .. } => false,
        }
    }

    fn is_finished(&self) -> bool {
        match self {
            BootstrapState::Started { .. } => false,
            BootstrapState::Finished { .. } => true,
        }
    }
}

struct Bootstraps {
    next_id_v4: usize,
    next_id_v6: usize,
    v4: HashMap<usize, BootstrapState>,
    v6: HashMap<usize, BootstrapState>,
}

impl Bootstraps {
    fn new() -> Self {
        Self {
            next_id_v4: 0,
            next_id_v6: 0,
            v4: HashMap::new(),
            v6: HashMap::new(),
        }
    }

    fn start(&mut self, ipv: IpVersion) -> BootstrapId {
        let (states, next_id) = match ipv {
            IpVersion::V4 => (&mut self.v4, &mut self.next_id_v4),
            IpVersion::V6 => (&mut self.v6, &mut self.next_id_v6),
        };

        let id = *next_id;
        *next_id += 1;

        states.insert(id, BootstrapState::Started { at: Instant::now() });

        BootstrapId { ipv, id }
    }

    fn finish(&mut self, id: BootstrapId, success: bool) {
        let bootstrap = match id.ipv {
            IpVersion::V4 => self.v4.get_mut(&id.id),
            IpVersion::V6 => self.v6.get_mut(&id.id),
        };

        let Some(bootstrap) = bootstrap else {
            log::error!("Failed invariant: bootstrap entries must persist until finished");
            return;
        };

        match bootstrap {
            BootstrapState::Started { at } => {
                *bootstrap = BootstrapState::Finished {
                    took: Instant::now().duration_since(*at),
                    success,
                }
            }
            BootstrapState::Finished { .. } => {
                log::error!("Bootstrap finished more than once");
            }
        }
    }

    fn clear_finished(&mut self) {
        self.v4.retain(|_, state| !state.is_finished());
        self.v6.retain(|_, state| !state.is_finished());
    }
}

#[derive(Debug, Serialize)]
struct ExponentialHistogram {
    scale: u32,
    buckets: BTreeMap<usize, u32>,
    // Counter for those values that don't fit into `buckets`
    more: u32,
}

impl ExponentialHistogram {
    fn new(scale: u32, bucket_count: usize, values: impl Iterator<Item = u128>) -> Self {
        let mut buckets = BTreeMap::new();

        let mut more = 0;

        for value in values {
            let mut bucket_min: u128 = 0;
            let mut bucket_max: u128 = scale as u128;
            let mut assigned = false;

            for bucket in 0..bucket_count {
                if bucket_min <= value && value < bucket_max {
                    assigned = true;
                    buckets.entry(bucket).and_modify(|b| *b += 1).or_insert(1);
                    break;
                }

                bucket_min = bucket_max;
                bucket_max *= 2;
            }

            if !assigned {
                more += 1;
            }
        }

        Self {
            scale,
            buckets,
            more,
        }
    }
}
