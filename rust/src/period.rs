use chrono::{DateTime, Datelike, Days, MappedLocalTime, TimeDelta, TimeZone, Timelike, Utc};
use std::time::Duration;

#[derive(Debug, Clone, Copy)]
pub struct WholeWeek {
    start: DateTime<Utc>,
    end: DateTime<Utc>,
}

impl WholeWeek {
    pub fn start(&self) -> DateTime<Utc> {
        self.start
    }

    pub fn end(&self) -> DateTime<Utc> {
        self.end
    }
}

impl TryFrom<DateTime<Utc>> for WholeWeek {
    type Error = &'static str;

    fn try_from(date: DateTime<Utc>) -> Result<Self, Self::Error> {
        let offset = date.weekday().num_days_from_monday();

        let start = date
            .checked_sub_days(Days::new(offset as u64))
            .ok_or("failed to calculate start")?;

        let end = start
            .checked_add_days(Days::new(7))
            .ok_or("failed to calculate end")?;

        Ok(Self { start, end })
    }
}

#[derive(Debug, Clone, Copy)]
pub struct WholeHour {
    start: DateTime<Utc>,
    end: DateTime<Utc>,
}

impl WholeHour {
    pub fn start(&self) -> DateTime<Utc> {
        self.start
    }

    pub fn end(&self) -> DateTime<Utc> {
        self.end
    }
}

impl TryFrom<DateTime<Utc>> for WholeHour {
    type Error = &'static str;

    fn try_from(date: DateTime<Utc>) -> Result<Self, Self::Error> {
        let start =
            match Utc.with_ymd_and_hms(date.year(), date.month(), date.day(), date.hour(), 0, 0) {
                MappedLocalTime::Single(date) => date,
                MappedLocalTime::Ambiguous(_, _) => return Err("start is ambiguous"),
                MappedLocalTime::None => return Err("start is none"),
            };

        let end = start
            .checked_add_signed(TimeDelta::hours(1))
            .ok_or("can't calculate end")?;

        Ok(Self { start, end })
    }
}

// A bit of a misnomer for this function because it returns ZERO when `from < start`, but that's
// generally how we want to use it: we rotate device_id and record sequence numbers right a way
// when the creation time no longer fits into their intervals.
pub fn duration_to_end(from: DateTime<Utc>, start: DateTime<Utc>, end: DateTime<Utc>) -> Duration {
    if from < start {
        log::warn!("RecordID's interval starts before `from`");
        return Duration::ZERO;
    }
    let Ok(delta_ms) = end
        .signed_duration_since(from)
        .num_milliseconds()
        .try_into()
    else {
        // `now > end`
        return Duration::ZERO;
    };
    Duration::from_millis(delta_ms)
}

#[cfg(test)]
mod test {
    use super::*;
    use chrono::Weekday;

    fn make_date(year: i32, month: u32, day: u32) -> DateTime<Utc> {
        Utc.with_ymd_and_hms(year, month, day, 0, 0, 0).unwrap()
    }

    #[test]
    fn construct_whole_week() {
        let date = make_date(2025, 6, 17); // Tuesday
        let week = WholeWeek::try_from(date).unwrap();
        assert_eq!(week.start().iso_week().year(), 2025);
        assert_eq!(week.start().iso_week().week0(), 24);
        assert_eq!(week.start().weekday(), Weekday::Mon);
        assert_eq!(week.end().weekday(), Weekday::Mon);
        assert_eq!(week.start(), make_date(2025, 6, 16));

        let date = make_date(2024, 12, 31);
        let week = WholeWeek::try_from(date).unwrap();
        assert_eq!(week.start().iso_week().year(), 2025);
        assert_eq!(week.start().iso_week().week0(), 0);
        assert_eq!(week.start().weekday(), Weekday::Mon);
        assert_eq!(week.end().weekday(), Weekday::Mon);
        assert_eq!(week.start(), make_date(2024, 12, 30));

        let date = make_date(2025, 1, 1);
        let week = WholeWeek::try_from(date).unwrap();
        assert_eq!(week.start().iso_week().year(), 2025);
        assert_eq!(week.start().iso_week().week0(), 0);
        assert_eq!(week.start().weekday(), Weekday::Mon);
        assert_eq!(week.end().weekday(), Weekday::Mon);
        assert_eq!(week.start(), make_date(2024, 12, 30));
    }

    #[test]
    fn next_week() {
        let interval =
            WholeWeek::try_from(Utc.with_ymd_and_hms(2026, 6, 15, 12, 20, 0).unwrap()).unwrap();
        let next_interval = WholeWeek::try_from(interval.end()).unwrap();
        assert_eq!(
            interval.start().iso_week().week0() + 1,
            next_interval.start().iso_week().week0()
        );
    }

    #[test]
    fn next_hour() {
        let date = Utc.with_ymd_and_hms(2026, 6, 17, 12, 20, 0).unwrap();
        let interval = WholeHour::try_from(date).unwrap();
        let next_interval = WholeHour::try_from(interval.end()).unwrap();
        assert_eq!(interval.start().hour() + 1, next_interval.start().hour());
    }
}
