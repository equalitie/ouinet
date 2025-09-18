#[cfg(test)]
use std::sync::{Arc, Mutex};
#[cfg(test)]
use std::time::Duration;
use std::time::SystemTime;

// Replacement for SystemTime used for testing.

#[derive(Clone)]
pub struct Clock {
    inner: ClockInner,
}

impl Clock {
    pub fn new() -> Self {
        Self {
            inner: ClockInner::System,
        }
    }

    #[cfg(test)]
    pub fn new_for_test() -> Self {
        Self {
            inner: ClockInner::Test(Arc::new(Mutex::new(ClockState {
                now: SystemTime::now(),
            }))),
        }
    }

    pub fn now(&self) -> SystemTime {
        match &self.inner {
            ClockInner::System => SystemTime::now(),
            #[cfg(test)]
            ClockInner::Test(state) => state.lock().unwrap().now,
        }
    }

    #[cfg(test)]
    pub fn add(&self, duration: Duration) {
        match &self.inner {
            ClockInner::System => panic!(),
            #[cfg(test)]
            ClockInner::Test(state) => {
                state.lock().unwrap().now += duration;
            }
        }
    }
}

#[derive(Clone)]
enum ClockInner {
    System,
    #[cfg(test)]
    Test(Arc<Mutex<ClockState>>),
}

#[cfg(test)]
#[derive(Clone)]
pub struct ClockState {
    now: SystemTime,
}
