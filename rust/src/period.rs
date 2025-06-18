use chrono::{DateTime, Datelike, Days, TimeDelta, TimeZone, Timelike, Utc};

#[derive(Debug, Clone, Copy)]
struct WholeWeek {
    year: i32,
    // Week of the year, counting from zero, the zeroth week may start in previous year
    week0: u32,
    // How many days from the last year we've counted into the zeroth week
    offset: u32,
}

impl WholeWeek {
    fn start(&self) -> DateTime<Utc> {
        // Unwrap: the fields are constructed using a valid DateTime
        Utc.with_ymd_and_hms(self.year, 1, 1, 0, 0, 0)
            .unwrap()
            .checked_add_days(Days::new((self.week0 * 7) as u64))
            .unwrap()
            .checked_sub_days(Days::new(self.offset as u64))
            .unwrap()
    }

    // Returns None when out of range
    fn end(&self) -> Option<DateTime<Utc>> {
        self.start().checked_add_days(Days::new(7))
    }

    // TODO: Unwrap
    fn next(&self) -> Self {
        Self::from(self.end().unwrap())
    }
}

impl From<DateTime<Utc>> for WholeWeek {
    fn from(mut date: DateTime<Utc>) -> Self {
        if week_ends_next_year(date) {
            date = Utc
                .with_ymd_and_hms(date.year() + 1, 1, 1, 0, 0, 0)
                .unwrap();
        }
        let year_start = Utc.with_ymd_and_hms(date.year(), 1, 1, 0, 0, 0).unwrap();
        let offset = year_start.weekday().num_days_from_monday();
        let days = days_up_to(date) + offset;
        Self {
            year: date.year(),
            week0: days / 7,
            offset,
        }
    }
}

// Count number of days up to `date` (not including).
fn days_up_to(date: DateTime<Utc>) -> u32 {
    use chrono::NaiveDate;
    let mut count: u32 = 0;
    for month in 1..date.month() {
        count += NaiveDate::from_ymd_opt(date.year(), month, 1)
            .unwrap()
            .num_days_in_month() as u32;
    }
    count + date.day0()
}

fn week_ends_next_year(date: DateTime<Utc>) -> bool {
    let days_left = 7 - date.weekday().num_days_from_monday();
    let end = Utc
        .with_ymd_and_hms(date.year(), date.month(), date.day(), 0, 0, 0)
        .unwrap()
        .checked_add_days(Days::new(days_left as u64))
        .unwrap();
    date.year() != end.year()
}

#[derive(Debug)]
pub struct WholeHour {
    year: i32,
    month0: u32,
    day0: u32,
    hour: u32,
}

impl WholeHour {
    pub fn start(&self) -> DateTime<Utc> {
        // Unwrap: the fields are constructed using a valid DateTime
        Utc.with_ymd_and_hms(self.year, self.month0 + 1, self.day0 + 1, self.hour, 0, 0)
            .unwrap()
    }

    // Returns None when out of range
    pub fn end(&self) -> Option<DateTime<Utc>> {
        self.start().checked_add_signed(TimeDelta::hours(1))
    }

    // TODO: Unwrap
    pub fn next(&self) -> Self {
        Self::from(self.end().unwrap())
    }
}

impl From<DateTime<Utc>> for WholeHour {
    fn from(date: DateTime<Utc>) -> Self {
        Self {
            year: date.year(),
            month0: date.month0(),
            day0: date.day0(),
            hour: date.hour(),
        }
    }
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
        let week = WholeWeek::from(date);
        assert_eq!(week.year, 2025);
        assert_eq!(week.week0, 24);
        assert_eq!(week.start(), make_date(2025, 6, 16));

        let date = make_date(2024, 12, 31);
        let week = WholeWeek::from(date);
        assert_eq!(week.year, 2025);
        assert_eq!(week.week0, 0);
        assert_eq!(week.start(), make_date(2024, 12, 30));

        let date = make_date(2025, 1, 1);
        let week = WholeWeek::from(date);
        assert_eq!(week.year, 2025);
        assert_eq!(week.week0, 0);
        assert_eq!(week.start(), make_date(2024, 12, 30));
    }

    #[test]
    fn test_days_up_to() {
        let date = make_date(2025, 1, 1);
        assert_eq!(days_up_to(date), 0);
        let date = make_date(2025, 1, 2);
        assert_eq!(days_up_to(date), 1);
        let date = make_date(2025, 2, 1);
        assert_eq!(days_up_to(date), 31);
    }

    #[test]
    fn test_week_ends_next_year() {
        assert!(!week_ends_next_year(make_date(2025, 12, 27)));
        assert!(!week_ends_next_year(make_date(2025, 12, 28)));
        assert!(week_ends_next_year(make_date(2025, 12, 29)));
        assert!(week_ends_next_year(make_date(2025, 12, 30)));
        assert!(week_ends_next_year(make_date(2025, 12, 31)));
    }

    #[test]
    fn next_week() {
        let week = WholeWeek::from(Utc.with_ymd_and_hms(2026, 6, 15, 12, 20, 0).unwrap());
        let next_week = week.next();
        assert_eq!(week.week0 + 1, next_week.week0);
    }

    #[test]
    fn next_hour() {
        let date = Utc.with_ymd_and_hms(2026, 6, 17, 12, 20, 0).unwrap();
        let hour = WholeHour::from(date);
        let next_hour = hour.next();
        assert_eq!(hour.hour + 1, next_hour.hour);
    }
}
