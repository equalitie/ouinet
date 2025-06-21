use crate::clock::Clock;
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
    clock: Clock,
}

impl Backoff {
    pub async fn new(file_path: PathBuf) -> io::Result<Self> {
        Self::new_with(file_path, Clock::new()).await
    }

    pub async fn new_with(file_path: PathBuf, clock: Clock) -> io::Result<Self> {
        let state = State::read(&file_path).await?;

        Ok(Self {
            file_path,
            stopped: false,
            state,
            clock,
        })
    }

    pub async fn sleep(&self) {
        match self.sleep_for() {
            SleepFor::Forever => std::future::pending().await,
            SleepFor::Duration(duration) => time::sleep(duration).await,
        }
    }

    fn sleep_for(&self) -> SleepFor {
        if self.stopped {
            return SleepFor::Forever;
        }

        match &self.state {
            State::Success => SleepFor::Duration(Duration::ZERO),
            State::Failure(fail) => {
                let now = self.clock.now();

                let duration_since_failure = match now.duration_since(fail.at) {
                    Ok(duration) => duration,
                    // System time has gone backward, assume no duration passed.
                    Err(_) => Duration::ZERO,
                };

                let sleep_for = match fail.duration_to_retry().checked_sub(duration_since_failure) {
                    Some(sleep_for) => sleep_for,
                    // We've waited for more than we had to.
                    None => Duration::ZERO,
                };

                SleepFor::Duration(sleep_for)
            }
        }
    }

    pub fn stop(&mut self) {
        self.stopped = true;
    }

    pub fn resume(&mut self) {
        self.stopped = false;
    }

    #[cfg(test)]
    pub fn is_stopped(&self) -> bool {
        self.stopped
    }

    pub async fn succeeded(&mut self) -> io::Result<()> {
        self.set_state(State::Success).await
    }

    pub async fn failed(&mut self) -> io::Result<()> {
        let new_state = match &self.state {
            State::Success => State::Failure(Failure {
                at: self.clock.now(),
                prev_failure_count: 0,
            }),
            State::Failure(fail) => {
                let now = self.clock.now();

                State::Failure(Failure {
                    at: now,
                    prev_failure_count: fail.prev_failure_count + 1,
                })
            }
        };

        self.set_state(new_state).await
    }

    async fn set_state(&mut self, new_state: State) -> io::Result<()> {
        self.state = new_state;
        self.state.write(&self.file_path).await
    }
}

enum State {
    Success,
    Failure(Failure),
}

impl State {
    async fn read(file_path: &PathBuf) -> io::Result<Self> {
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

        match state {
            Some(string) => match serde_json::from_str(&string) {
                Ok(state) => Ok(State::Failure(state)),
                Err(_) => {
                    fs::remove_file(&file_path).await?;
                    Ok(State::Success)
                }
            },
            None => Ok(State::Success),
        }
    }

    async fn write(&self, file_path: &PathBuf) -> io::Result<()> {
        match &self {
            State::Success => match fs::remove_file(&file_path).await {
                Ok(()) => Ok(()),
                Err(error) => {
                    if error.kind() == io::ErrorKind::NotFound {
                        Ok(())
                    } else {
                        Err(error)
                    }
                }
            },
            State::Failure(failed) => fs::write(&file_path, json!(&failed).to_string()).await,
        }
    }
}

#[derive(Serialize, Deserialize)]
struct Failure {
    at: SystemTime,
    prev_failure_count: u32,
}

impl Failure {
    fn duration_to_retry(&self) -> Duration {
        INITIAL_WAIT_AFTER_FAILURE
            .checked_mul(
                2_u32
                    .checked_pow(self.prev_failure_count)
                    .unwrap_or(u32::MAX),
            )
            .unwrap_or(MAX_WAIT_AFTER_FAILURE)
            .min(MAX_WAIT_AFTER_FAILURE)
    }
}

enum SleepFor {
    Forever,
    Duration(Duration),
}

#[cfg(test)]
impl SleepFor {
    fn unwrap_duration(&self) -> Duration {
        match self {
            SleepFor::Forever => panic!(),
            SleepFor::Duration(duration) => *duration,
        }
    }

    fn is_forever(&self) -> bool {
        std::matches!(self, SleepFor::Forever)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tmpdir::TmpDir;

    #[tokio::test]
    async fn backoff_stop() {
        let (_tmpdir, _clock, mut backoff) = init().await;
        backoff.stop();
        assert!(backoff.sleep_for().is_forever());
        backoff.resume();
        assert_eq!(backoff.sleep_for().unwrap_duration(), Duration::ZERO);
    }

    #[tokio::test]
    async fn backoff() {
        let (_tmpdir, clock, mut backoff) = init().await;

        assert_eq!(backoff.sleep_for().unwrap_duration(), Duration::ZERO);

        // Mark as failed
        backoff.failed().await.unwrap();

        // Check that we need to wait
        assert_eq!(
            backoff.sleep_for().unwrap_duration(),
            INITIAL_WAIT_AFTER_FAILURE
        );

        // Wait only half of the time and then check how much longer we need to wait
        let (half1, half2) = duration_div(INITIAL_WAIT_AFTER_FAILURE, 2.0);

        clock.add(half1);

        assert_eq!(backoff.sleep_for().unwrap_duration(), half2);

        // Wait the other half of the time
        clock.add(half2);

        // Check that we no longer need to wait
        assert_eq!(backoff.sleep_for().unwrap_duration(), Duration::ZERO);

        // Fail again, this time we'll wait longer than needed by half a second
        backoff.failed().await.unwrap();

        let sleep_for = backoff.sleep_for().unwrap_duration();

        let half_sec = Duration::from_millis(500);

        clock.add(sleep_for + half_sec);

        assert_eq!(backoff.sleep_for().unwrap_duration(), Duration::ZERO);
    }

    async fn init() -> (TmpDir, Clock, Backoff) {
        let tmpdir = TmpDir::new("backoff").await.unwrap();
        let file_path = tmpdir.as_ref().join("backoff.json");
        let clock = Clock::new_for_test();
        let backoff = Backoff::new_with(file_path, clock.clone()).await.unwrap();
        (tmpdir, clock, backoff)
    }

    fn duration_div(duration: Duration, by: f32) -> (Duration, Duration) {
        let half_1 = duration.div_f32(by);
        let half_2 = duration.checked_sub(half_1).unwrap();
        (half_1, half_2)
    }
}
