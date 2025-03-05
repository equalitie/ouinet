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
