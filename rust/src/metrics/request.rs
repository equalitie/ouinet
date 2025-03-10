use serde::{ser::SerializeMap, Serialize, Serializer};
use std::collections::HashMap;

#[derive(Clone, Copy, Hash, Eq, PartialEq)]
pub struct RequestId(u64);

#[derive(Clone, Copy, Hash, Eq, PartialEq)]
pub enum RequestType {
    Origin,
    InjectorPrivate,
    InjectorPublic,
    Cache,
}

pub struct Requests {
    next_id: u64,
    active: HashMap<RequestId, RequestState>,
    summary: HashMap<RequestType, Summary>,
}

impl Requests {
    pub fn new() -> Self {
        Self {
            next_id: 0,
            active: Default::default(),
            summary: Default::default(),
        }
    }

    pub fn add_request(&mut self, request_type: RequestType) -> RequestId {
        let id = RequestId(self.next_id);
        self.next_id += 1;
        self.active.insert(id, RequestState::Exists(request_type));
        id
    }

    pub fn mark_request_started(&mut self, id: RequestId) {
        let Some(state) = self.active.get_mut(&id) else {
            log::error!("Attempted to start a non active request");
            return;
        };

        match state {
            RequestState::Exists(request_type) => *state = RequestState::Started(*request_type),
            RequestState::Started(_) => {
                log::error!("Attempted to start an already started request");
            }
        }
    }

    pub fn remove_request(&mut self, id: RequestId, reason: RemoveReason) {
        let Some(state) = self.active.remove(&id) else {
            log::error!("Attempted to remove a non active request");
            return;
        };

        match state {
            RequestState::Exists(_) => {}
            RequestState::Started(request_type) => {
                let summary = self
                    .summary
                    .entry(request_type)
                    .or_insert_with(Default::default);
                match reason {
                    RemoveReason::Success => summary.success_count += 1,
                    RemoveReason::Failure => summary.failure_count += 1,
                    RemoveReason::Cancelled => (),
                }
            }
        }
    }
}

impl Serialize for Requests {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut map = serializer.serialize_map(Some(self.summary.len()))?;
        for (request_type, summary) in &self.summary {
            let key = match request_type {
                RequestType::Origin => "origin",
                RequestType::InjectorPrivate => "injector_private",
                RequestType::InjectorPublic => "injector_public",
                RequestType::Cache => "cache",
            };
            map.serialize_entry(key, summary)?;
        }
        map.end()
    }
}

#[derive(Default, Serialize)]
struct Summary {
    success_count: u64,
    failure_count: u64,
}

pub enum RemoveReason {
    Success,
    Failure,
    Cancelled,
}

enum RequestState {
    Exists(RequestType),
    Started(RequestType),
}
