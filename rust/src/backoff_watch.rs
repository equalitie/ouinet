use tokio::{
    sync::watch,
    task::{spawn, JoinHandle},
    time::{self, Duration, Instant},
};

pub struct ConstantBackoffWatchSender {
    runner_tx: watch::Sender<()>,
    // Keeping user_tx here just so the user can subscribe to it.
    user_tx: watch::Sender<()>,
    runner: JoinHandle<()>,
}

impl ConstantBackoffWatchSender {
    pub fn new(delay: Duration) -> Self {
        let (runner_tx, mut runner_rx) = watch::channel(());
        let (user_tx, _user_rx) = watch::channel(());

        let runner = spawn({
            let user_tx = user_tx.clone();

            async move {
                let mut last_sent: Option<Instant> = None;

                loop {
                    if runner_rx.changed().await.is_err() {
                        break;
                    }

                    if let Some(time_sent) = last_sent {
                        match delay.checked_sub(time_sent.elapsed()) {
                            Some(remaining) => time::sleep(remaining).await,
                            None => (),
                        }
                    }

                    last_sent = Some(Instant::now());
                    user_tx.send_modify(|_| {});

                    // Don't accumulate the send events while we were sleeping.
                    runner_rx.mark_unchanged();
                }
            }
        });

        Self {
            runner_tx,
            user_tx,
            runner,
        }
    }

    pub fn send_modify<F: FnOnce(&mut ())>(&self, modify: F) {
        self.runner_tx.send_modify(modify);
    }

    pub fn subscribe(&self) -> ConstantBackoffWatchReceiver {
        ConstantBackoffWatchReceiver {
            user_rx: self.user_tx.subscribe(),
        }
    }
}

impl Drop for ConstantBackoffWatchSender {
    fn drop(&mut self) {
        self.runner.abort();
    }
}

pub struct ConstantBackoffWatchReceiver {
    user_rx: watch::Receiver<()>,
}

impl ConstantBackoffWatchReceiver {
    pub async fn changed(&mut self) -> Result<(), watch::error::RecvError> {
        self.user_rx.changed().await
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[tokio::test]
    async fn sanity() {
        let delay = Duration::from_millis(500);
        let tx = ConstantBackoffWatchSender::new(delay);
        let mut rx = tx.subscribe();

        let start = Instant::now();

        // This should be received almost immediatelly.
        tx.send_modify(|_| ());
        rx.changed().await.unwrap();

        assert!(start.elapsed() < delay);

        let start = Instant::now();

        // This send should be received after `delay`.
        tx.send_modify(|_| ());
        rx.changed().await.unwrap();

        assert!(start.elapsed() >= delay);
    }

    #[tokio::test]
    async fn dont_accumulate_sends_during_timeout() {
        let delay = Duration::from_millis(200);
        let tx = ConstantBackoffWatchSender::new(delay);
        let mut rx = tx.subscribe();

        // The first send will be received immediatelly.
        tx.send_modify(|_| ());
        rx.changed().await.unwrap();

        // This will trigger the delay timeout inside the runner
        tx.send_modify(|_| ());

        // Give the runner time to receive
        time::sleep(Duration::from_millis(100)).await;

        // Send again while the runner is still waiting, this send should not accumulate
        tx.send_modify(|_| ());

        // Receive expected event
        rx.changed().await.unwrap();

        // The last send event should not accumulate so we expect this sleep to timeout
        assert!(
            tokio::time::timeout(Duration::from_millis(300), rx.changed())
                .await
                .is_err()
        );
    }
}
