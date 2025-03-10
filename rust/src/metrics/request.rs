use std::collections::HashMap;

#[derive(Clone, Copy, Hash, Eq, PartialEq)]
pub struct RequestId(u64);

pub enum RequestType {
    Origin,
    Injector,
    Cache,
}

pub struct Requests {
    next_id: u64,
    active: HashMap<RequestId, RequestState>,
}

impl Requests {
    pub fn new() -> Self {
        Self {
            next_id: 0,
            active: Default::default(),
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
            RequestState::Exists(_) => *state = RequestState::Started,
            RequestState::Started => {
                log::error!("Attempted to start an already started request");
            }
        }
    }

    pub fn remove_request(&mut self, id: RequestId, reason: RemoveReason) {
        self.active.remove(&id);
    }
}

pub enum RemoveReason {
    Success,
    Failure,
    Cancelled,
}

enum RequestState {
    Exists(RequestType),
    Started,
}
