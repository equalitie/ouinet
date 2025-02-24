use serde::{Deserialize, Serialize};
use serde_json::json;
use std::{
    io,
    path::PathBuf,
    time::{Duration, SystemTime},
};
use tokio::{fs, time};

const INITIAL_WAIT_AFTER_FAILURE: Duration = Duration::from_secs(1);
const MAX_WAIT_AFTER_FAILURE: Duration = Duration::from_secs(60 * 60 * 24);

pub struct Backoff {
    file_path: PathBuf,
    stopped: bool,
    state: State,
}

impl Backoff {
    pub async fn new(file_path: PathBuf) -> io::Result<Self> {
        let state = match fs::read_to_string(&file_path).await {
            Ok(content) => Some(content),
            Err(error) => {
                if error.kind() == io::ErrorKind::NotFound {
                    None
                } else {
                    return Err(error);
                }
            }
        };

        let state = match state {
            Some(string) => match serde_json::from_str(&string) {
                Ok(state) => State::Failure(state),
                Err(_) => {
                    fs::remove_file(&file_path).await?;
                    State::Success
                }
            },
            None => State::Success,
        };

        Ok(Self {
            file_path,
            stopped: false,
            state,
        })
    }

    pub async fn sleep(&self) {
        if self.stopped {
            std::future::pending::<()>().await;
            unreachable!();
        }

        match &self.state {
            State::Success => (),
            State::Failure(fail) => {
                let now = SystemTime::now();

                let duration_since_failure = match now.duration_since(fail.at) {
                    Ok(duration) => duration,
                    // System time has changed, assume no duration passed.
                    Err(_) => Duration::ZERO,
                };

                let sleep_for = match fail.duration_to_retry.checked_sub(duration_since_failure) {
                    Some(sleep_for) => sleep_for,
                    // We've waited for more than we had to.
                    None => Duration::ZERO,
                };

                time::sleep(sleep_for).await
            }
        }
    }

    pub fn stop(&mut self) {
        self.stopped = true;
    }

    pub fn resume(&mut self) {
        self.stopped = false;
    }

    pub async fn succeeded(&mut self) -> io::Result<()> {
        self.state = State::Success;
        self.store().await
    }

    pub async fn failed(&mut self) -> io::Result<()> {
        match &self.state {
            State::Success => {
                self.state = State::Failure(Failure {
                    at: SystemTime::now(),
                    duration_to_retry: INITIAL_WAIT_AFTER_FAILURE,
                });
            }
            State::Failure(fail) => {
                let now = SystemTime::now();

                self.state = State::Failure(Failure {
                    at: now,
                    duration_to_retry: (fail.duration_to_retry * 2).min(MAX_WAIT_AFTER_FAILURE),
                });
            }
        }

        self.store().await
    }

    async fn store(&self) -> io::Result<()> {
        match &self.state {
            State::Success => fs::remove_file(&self.file_path).await,
            State::Failure(failed) => fs::write(&self.file_path, json!(&failed).to_string()).await,
        }
    }
}

enum State {
    Success,
    Failure(Failure),
}

#[derive(Serialize, Deserialize)]
struct Failure {
    at: SystemTime,
    // Next re-try should happen no sooner than `at + duration_to_retry`.
    duration_to_retry: Duration,
}

#[test]
mod test {
    #[tokio::test]
    async fn backoff() {}
}
