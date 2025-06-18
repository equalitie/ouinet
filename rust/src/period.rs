use chrono::{DateTime, Datelike, Days, MappedLocalTime, TimeDelta, TimeZone, Timelike, Utc};
use std::time::Duration;

#[derive(Debug, Clone, Copy)]
pub struct WholeWeek {
    // `start.year` may be one before the one in the date `self` was derived from
    start: DateTime<Utc>,
    end: DateTime<Utc>,
    // days since `start`
    days: u32,
}

impl WholeWeek {
    pub fn start(&self) -> DateTime<Utc> {
        self.start
    }

    pub fn end(&self) -> DateTime<Utc> {
        self.end
    }

    pub fn year(&self) -> i32 {
        self.end.year()
    }

    pub fn week0(&self) -> u32 {
        self.days / 7
    }
}

impl TryFrom<DateTime<Utc>> for WholeWeek {
    type Error = &'static str;

    fn try_from(mut date: DateTime<Utc>) -> Result<Self, Self::Error> {
        if week_ends_next_year(date)? {
            date = match Utc.with_ymd_and_hms(date.year() + 1, 1, 1, 0, 0, 0) {
                MappedLocalTime::Single(date) => date,
                MappedLocalTime::Ambiguous(_, _) => return Err("next year ambiguous"),
                MappedLocalTime::None => return Err("next year is out of range"),
            }
        }

        let year_start = match Utc.with_ymd_and_hms(date.year(), 1, 1, 0, 0, 0) {
            MappedLocalTime::Single(date) => date,
            MappedLocalTime::Ambiguous(_, _) => return Err("start year ambiguous"),
            MappedLocalTime::None => return Err("start year is out of range"),
        };

        let offset = year_start.weekday().num_days_from_monday();
        let days = days_up_to(date)? + offset;

        let week0 = days / 7;
        let start = year_start
            .checked_add_days(Days::new((week0 * 7) as u64))
            .and_then(|date| date.checked_sub_days(Days::new(offset as u64)))
            .ok_or("failed to calculate start")?;

        let end = start
            .checked_add_days(Days::new(7))
            .ok_or("failed to calculate end")?;

        Ok(Self { start, end, days })
    }
}

// Count number of days up to `date` (not including).
fn days_up_to(date: DateTime<Utc>) -> Result<u32, &'static str> {
    use chrono::NaiveDate;
    let mut count: u32 = 0;
    for month in 1..date.month() {
        count += NaiveDate::from_ymd_opt(date.year(), month, 1)
            .ok_or("days_up_to: invalid date")?
            .num_days_in_month() as u32;
    }
    Ok(count + date.day0())
}

fn week_ends_next_year(date: DateTime<Utc>) -> Result<bool, &'static str> {
    let days_left = 7 - date.weekday().num_days_from_monday();
    let end = match Utc.with_ymd_and_hms(date.year(), date.month(), date.day(), 0, 0, 0) {
        MappedLocalTime::Single(date) => date
            .checked_add_days(Days::new(days_left as u64))
            .ok_or("week_ends_next_year: can't add")?,
        MappedLocalTime::Ambiguous(_, _) => return Err("week_ends_next_year: ambiguous"),
        MappedLocalTime::None => return Err("week_ends_next_year: none"),
    };
    Ok(date.year() != end.year())
}

#[derive(Debug)]
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

    fn make_date(year: i32, month: u32, day: u32) -> DateTime<Utc> {
        Utc.with_ymd_and_hms(year, month, day, 0, 0, 0).unwrap()
    }

    #[test]
    fn construct_whole_week() {
        let date = make_date(2025, 6, 17);
        let week = WholeWeek::try_from(date).unwrap();
        assert_eq!(week.year(), 2025);
        assert_eq!(week.week0(), 24);
        assert_eq!(week.start(), make_date(2025, 6, 16));

        let date = make_date(2024, 12, 31);
        let week = WholeWeek::try_from(date).unwrap();
        assert_eq!(week.year(), 2025);
        assert_eq!(week.week0(), 0);
        assert_eq!(week.start(), make_date(2024, 12, 30));

        let date = make_date(2025, 1, 1);
        let week = WholeWeek::try_from(date).unwrap();
        assert_eq!(week.year(), 2025);
        assert_eq!(week.week0(), 0);
        assert_eq!(week.start(), make_date(2024, 12, 30));
    }

    #[test]
    fn test_days_up_to() {
        let date = make_date(2025, 1, 1);
        assert_eq!(days_up_to(date).unwrap(), 0);
        let date = make_date(2025, 1, 2);
        assert_eq!(days_up_to(date).unwrap(), 1);
        let date = make_date(2025, 2, 1);
        assert_eq!(days_up_to(date).unwrap(), 31);
    }

    #[test]
    fn test_week_ends_next_year() {
        assert!(!week_ends_next_year(make_date(2025, 12, 27)).unwrap());
        assert!(!week_ends_next_year(make_date(2025, 12, 28)).unwrap());
        assert!(week_ends_next_year(make_date(2025, 12, 29)).unwrap());
        assert!(week_ends_next_year(make_date(2025, 12, 30)).unwrap());
        assert!(week_ends_next_year(make_date(2025, 12, 31)).unwrap());
    }

    #[test]
    fn next_week() {
        let interval =
            WholeWeek::try_from(Utc.with_ymd_and_hms(2026, 6, 15, 12, 20, 0).unwrap()).unwrap();
        let next_interval = WholeWeek::try_from(interval.end()).unwrap();
        assert_eq!(interval.week0() + 1, next_interval.week0());
    }

    #[test]
    fn next_hour() {
        let date = Utc.with_ymd_and_hms(2026, 6, 17, 12, 20, 0).unwrap();
        let interval = WholeHour::try_from(date).unwrap();
        let next_interval = WholeHour::try_from(interval.end()).unwrap();
        assert_eq!(interval.start().hour() + 1, next_interval.start().hour());
    }
}
