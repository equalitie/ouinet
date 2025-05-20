use crate::backoff_watch::ConstantBackoffWatchSender;
use serde::{ser::SerializeMap, Serialize, Serializer};
use std::{collections::HashMap, sync::Arc};

#[derive(Clone, Copy, Hash, Eq, PartialEq)]
pub struct RequestId(u64);

#[derive(Clone, Copy, Hash, Eq, PartialEq, Debug)]
pub enum RequestType {
    Origin,
    InjectorPrivate,
    InjectorPublic,
    // Entries from cache we received
    CacheIn,
    // Entries from cache received served
    CacheOut,
}

pub struct Requests {
    next_id: u64,
    active: HashMap<RequestId, RequestType>,
    summary: HashMap<RequestType, Summary>,
    on_modify_tx: Arc<ConstantBackoffWatchSender>,
    has_new_data: bool,
}

impl Requests {
    pub fn new(on_modify_tx: Arc<ConstantBackoffWatchSender>) -> Self {
        Self {
            next_id: 0,
            active: Default::default(),
            summary: Default::default(),
            on_modify_tx,
            has_new_data: false,
        }
    }

    pub fn add_request(&mut self, request_type: RequestType) -> RequestId {
        let id = RequestId(self.next_id);
        self.next_id += 1;
        self.active.insert(id, request_type);
        self.mark_modified(true);
        id
    }

    pub fn remove_request(&mut self, id: RequestId, reason: RemoveReason) {
        let Some(request_type) = self.active.remove(&id) else {
            // Cancellation is done from the destructor which is always called.
            if reason != RemoveReason::Cancelled {
                log::warn!("Attempted to remove a non active request (might be a false positive if recently `clear`ed)");
            }
            return;
        };

        let summary = self.summary.entry(request_type).or_default();
        match reason {
            RemoveReason::Success => summary.success_count += 1,
            RemoveReason::Failure => summary.failure_count += 1,
            RemoveReason::Cancelled => (),
        }

        self.mark_modified(true);
    }

    pub fn clear(&mut self) {
        self.active.clear();
        self.summary.clear();
        self.mark_modified(false);
    }

    pub fn clear_finished(&mut self) {
        self.summary.clear();
        self.mark_modified(false);
    }

    pub fn has_new_data(&self) -> bool {
        self.has_new_data
    }

    pub fn increment_transfer_size(&mut self, request_type: RequestType, added: usize) {
        let summary = self.summary.entry(request_type).or_default();
        summary.transferred += added as u64;
    }

    fn mark_modified(&mut self, has_new_data: bool) {
        self.has_new_data = has_new_data;
        self.on_modify_tx.send_modify(|_| {})
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
                RequestType::CacheIn => "cache_in",
                RequestType::CacheOut => "cache_out",
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
    transferred: u64,
}

#[derive(PartialEq, Debug)]
pub enum RemoveReason {
    Success,
    Failure,
    Cancelled,
}
