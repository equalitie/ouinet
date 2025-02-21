use crate::{store::StoredRecord, CxxOneShotSender, CxxRecordProcessor};
use cxx::UniquePtr;
use tokio::sync::oneshot;

pub struct RecordProcessor {
    cxx_processor: UniquePtr<CxxRecordProcessor>,
}

impl RecordProcessor {
    pub fn new(cxx_processor: UniquePtr<CxxRecordProcessor>) -> Self {
        Self { cxx_processor }
    }

    // Sends the record for processing in C++, awaits until done.
    //
    // Returns:
    //   `Some(true)` if the record was processed (sent) successfully
    //   `Some(false)` if processing failed. We should wait and retry
    //   `None` when the C++ executor was destroyed
    pub async fn process(&self, record: &StoredRecord) -> Option<bool> {
        let (tx, rx) = oneshot::channel();
        self.cxx_processor.as_ref().unwrap().execute(
            record.name.clone(),
            record.content.clone(),
            Box::new(CxxOneShotSender::new(tx)),
        );
        match rx.await {
            Ok(recort_sent) => Some(recort_sent),
            Err(_) => None,
        }
    }
}
