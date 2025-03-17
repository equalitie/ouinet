use crate::store::StoredRecord;
use thiserror::Error;

#[cfg(not(test))]
mod normal_impl {
    use super::*;
    use crate::bridge::{CxxOneShotSender, CxxRecordProcessor};
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
        pub async fn process(&self, record: &StoredRecord) -> Result<bool, RecordProcessorError> {
            let (tx, rx) = oneshot::channel();
            self.cxx_processor.as_ref().unwrap().execute(
                record.name(),
                record.content.clone(),
                Box::new(CxxOneShotSender::new(tx)),
            );
            match rx.await {
                Ok(recort_sent) => Ok(recort_sent),
                Err(_) => Err(RecordProcessorError::CxxDisconnected),
            }
        }
    }
}

#[cfg(test)]
mod test_impl {
    use super::*;

    pub struct RecordProcessor {}

    impl RecordProcessor {
        pub fn new() -> Self {
            Self {}
        }

        pub async fn process(&self, record: &StoredRecord) -> Result<bool, RecordProcessorError> {
            // Just use these so the compiler doesn't spit out warnings about not being used or
            // constructed.
            let _name = record.name();
            let _data = record.content.clone();
            let _err = RecordProcessorError::CxxDisconnected;
            Ok(true)
        }
    }
}

#[cfg(not(test))]
pub use normal_impl::*;

#[cfg(test)]
pub use test_impl::*;

#[derive(Error, Debug)]
pub enum RecordProcessorError {
    #[error("The C++ end destroyed the handler before completion")]
    CxxDisconnected,
}
