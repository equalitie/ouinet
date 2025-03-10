use super::IpVersion;
use serde::{ser::SerializeMap, Serialize, Serializer};
use std::collections::{BTreeMap, HashMap};
use tokio::time::{Duration, Instant};

#[derive(Clone, Copy, Debug)]
pub struct BootstrapId {
    ipv: IpVersion,
    id: usize,
}

#[derive(Debug, Clone)]
pub enum BootstrapState {
    Started { at: Instant },
    Finished { took: Duration, success: bool },
}

impl BootstrapState {
    pub fn success_duration_ms(&self) -> Option<u128> {
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

    pub fn is_started(&self) -> bool {
        match self {
            BootstrapState::Started { .. } => true,
            BootstrapState::Finished { .. } => false,
        }
    }

    pub fn is_finished(&self) -> bool {
        match self {
            BootstrapState::Started { .. } => false,
            BootstrapState::Finished { .. } => true,
        }
    }
}

pub struct Bootstraps {
    next_id_v4: usize,
    next_id_v6: usize,
    v4: HashMap<usize, BootstrapState>,
    v6: HashMap<usize, BootstrapState>,
}

impl Bootstraps {
    pub fn new() -> Self {
        Self {
            next_id_v4: 0,
            next_id_v6: 0,
            v4: HashMap::new(),
            v6: HashMap::new(),
        }
    }

    pub fn start(&mut self, ipv: IpVersion) -> BootstrapId {
        let (states, next_id) = match ipv {
            IpVersion::V4 => (&mut self.v4, &mut self.next_id_v4),
            IpVersion::V6 => (&mut self.v6, &mut self.next_id_v6),
        };

        let id = *next_id;
        *next_id += 1;

        states.insert(id, BootstrapState::Started { at: Instant::now() });

        BootstrapId { ipv, id }
    }

    pub fn finish(&mut self, id: BootstrapId, success: bool) {
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

    pub fn clear_finished(&mut self) {
        self.v4.retain(|_, state| !state.is_finished());
        self.v6.retain(|_, state| !state.is_finished());
    }

    pub fn clear(&mut self) {
        self.v4.clear();
        self.v6.clear();
    }
}

impl Serialize for Bootstraps {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut map = serializer.serialize_map(Some(2))?;
        map.serialize_entry("v4", &BootstrapSerializer { bs: &self.v4 })?;
        map.serialize_entry("v6", &BootstrapSerializer { bs: &self.v6 })?;
        map.end()
    }
}

struct BootstrapSerializer<'a> {
    bs: &'a HashMap<usize, BootstrapState>,
}

impl<'a> Serialize for BootstrapSerializer<'a> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut map = serializer.serialize_map(Some(4))?;
        map.serialize_entry(
            "histogram",
            &ExponentialHistogram::new(
                200,
                10,
                self.bs.values().filter_map(|s| s.success_duration_ms()),
            ),
        )?;
        map.serialize_entry(
            "unfinished",
            &self.bs.values().filter(|s| s.is_started()).count(),
        )?;
        map.serialize_entry(
            "min",
            &self
                .bs
                .values()
                .filter_map(|s| s.success_duration_ms())
                .min(),
        )?;
        map.serialize_entry(
            "max",
            &self
                .bs
                .values()
                .filter_map(|s| s.success_duration_ms())
                .max(),
        )?;
        map.end()
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
