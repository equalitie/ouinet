use serde::{ser::SerializeMap, Serialize, Serializer};
/// Key/value metrics provided by exernal code
use std::collections::{hash_map::Entry, HashMap};

pub struct Auxiliary {
    map: HashMap<String, String>,
    has_new_data: bool,
}

impl Auxiliary {
    pub fn new() -> Self {
        Self {
            map: Default::default(),
            has_new_data: false,
        }
    }

    // Returns true when modified
    pub fn set(&mut self, key: String, value: String) -> bool {
        let mut modified = false;

        match self.map.entry(key) {
            Entry::Occupied(mut entry) => {
                if entry.get() != &value {
                    entry.insert(value);
                    modified = true;
                }
            }
            Entry::Vacant(entry) => {
                entry.insert(value);
                modified = true;
            }
        }

        self.has_new_data |= modified;
        modified
    }

    pub fn has_new_data(&self) -> bool {
        self.has_new_data
    }

    pub fn on_device_id_changed(&mut self) {
        self.clear();
    }

    pub fn on_record_sequence_number_changed(&mut self) {
        self.clear();
    }

    fn clear(&mut self) {
        self.has_new_data = false;
        self.map.clear();
    }
}

impl Serialize for Auxiliary {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut map = serializer.serialize_map(Some(self.map.len()))?;
        for (key, value) in &self.map {
            map.serialize_entry(key, value)?;
        }
        map.end()
    }
}
