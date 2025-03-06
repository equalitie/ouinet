use crate::{
    backoff_watch::{ConstantBackoffWatchReceiver, ConstantBackoffWatchSender},
    constants,
};
use chrono::{offset::Utc, DateTime};
use serde::Serialize;
use serde_json::json;
use std::{
    collections::{BTreeMap, HashMap},
    time::SystemTime,
};
use tokio::time::{Duration, Instant};

const DAY_TIME_FORMAT: &'static str = "%FT%T%.3f";

#[derive(Clone, Copy, Debug)]
pub enum IpVersion {
    V4,
    V6,
}

#[derive(Clone, Copy, Debug)]
pub struct BootstrapId {
    ipv: IpVersion,
    id: usize,
}

pub struct Metrics {
    start: DateTime<Utc>,
    record_start: DateTime<Utc>,
    on_modify_tx: ConstantBackoffWatchSender,
    bootstraps: Bootstraps,
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

        let bv4 = &self.bootstraps.v4;
        let bv6 = &self.bootstraps.v6;

        let data = json!({
            "start": format!("{}", self.start.format(DAY_TIME_FORMAT)),
            "record_start": format!("{}", self.record_start.format(DAY_TIME_FORMAT)),
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
            // This could happen when the bootstrap started and the `clear` function was called
            // because the `device_id` has rotated.
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

    fn clear(&mut self) {
        self.v4.clear();
        self.v6.clear();
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
