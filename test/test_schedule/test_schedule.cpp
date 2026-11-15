// Host test: see test/CMakeLists.txt
// Dose schedule (schedule.h): windows, their edges, extra and off-schedule
// doses, missed moments, reminders, midnight wrap, clock arriving late.
#include <unity.h>
#include <vector>
#include "schedule.h"

void setUp() {}
void tearDown() {}

static const int64_t DAY0 = 20000 * Schedule::DAY;  // some local midnight
static int64_t at(int h, int m, int day = 0) { return DAY0 + day * Schedule::DAY + h * 60 + m; }

// Tick minute by minute from a to b inclusive, collecting the events.
static std::vector<Schedule::Event> run(Schedule &s, int64_t a, int64_t b) {
    std::vector<Schedule::Event> all;
    Schedule::Event ev[8];
    for (int64_t t = a; t <= b; t++) {
        int n = s.tick(t, ev, 8);
        for (int i = 0; i < n; i++) all.push_back(ev[i]);
    }
    return all;
}

static int count(const std::vector<Schedule::Event> &v, Schedule::Action a, int slot) {
    int n = 0;
    for (const Schedule::Event &e : v) if (e.action == a && e.slot == slot) n++;
    return n;
}

static void test_empty_schedule() {
    Schedule s;
    TEST_ASSERT_EQUAL_INT(Schedule::NO_SCHEDULE, s.dose(at(8, 0)).status);
    TEST_ASSERT_EQUAL_INT(0, run(s, at(0, 0), at(23, 59)).size());
}

static void test_add_sorted_unique_full() {
    Schedule s;
    TEST_ASSERT_TRUE(s.add(20 * 60));
    TEST_ASSERT_TRUE(s.add(8 * 60));
    TEST_ASSERT_FALSE(s.add(8 * 60));   // duplicate
    TEST_ASSERT_FALSE(s.add(1440));     // out of range
    TEST_ASSERT_TRUE(s.add(14 * 60));
    TEST_ASSERT_TRUE(s.add(12 * 60));
    TEST_ASSERT_FALSE(s.add(10 * 60));  // full
    const Schedule::State &st = s.state();
    TEST_ASSERT_EQUAL_INT(4, st.n);
    TEST_ASSERT_EQUAL_INT(8 * 60, st.minute[0]);
    TEST_ASSERT_EQUAL_INT(12 * 60, st.minute[1]);
    TEST_ASSERT_EQUAL_INT(14 * 60, st.minute[2]);
    TEST_ASSERT_EQUAL_INT(20 * 60, st.minute[3]);
    TEST_ASSERT_TRUE(s.remove(2));
    TEST_ASSERT_EQUAL_INT(3, s.state().n);
    TEST_ASSERT_EQUAL_INT(14 * 60, s.state().minute[1]);
    TEST_ASSERT_FALSE(s.remove(4));
    TEST_ASSERT_FALSE(s.set_window(30, 721));
    TEST_ASSERT_TRUE(s.set_window(15, 90));
    TEST_ASSERT_EQUAL_INT(15, s.state().before);
}

static void test_window_edges() {
    Schedule s;
    s.add(8 * 60);  // window 07:30..09:00
    TEST_ASSERT_EQUAL_INT(Schedule::OFF_SCHEDULE, s.dose(at(7, 29)).status);
    Schedule::Dose d = s.dose(at(7, 30));
    TEST_ASSERT_EQUAL_INT(Schedule::ON_TIME, d.status);
    TEST_ASSERT_EQUAL_INT(1, d.slot);

    Schedule s2;
    s2.add(8 * 60);
    TEST_ASSERT_EQUAL_INT(Schedule::ON_TIME, s2.dose(at(9, 0)).status);
    Schedule s3;
    s3.add(8 * 60);
    TEST_ASSERT_EQUAL_INT(Schedule::OFF_SCHEDULE, s3.dose(at(9, 1)).status);
}

static void test_extra_dose_same_window() {
    Schedule s;
    s.add(8 * 60);
    TEST_ASSERT_EQUAL_INT(Schedule::ON_TIME, s.dose(at(8, 5)).status);
    Schedule::Dose d = s.dose(at(8, 20));
    TEST_ASSERT_EQUAL_INT(Schedule::EXTRA, d.status);
    TEST_ASSERT_EQUAL_INT(1, d.slot);
    // the next day the same moment is a fresh window
    TEST_ASSERT_EQUAL_INT(Schedule::ON_TIME, s.dose(at(8, 5, 1)).status);
}

// Overlapping windows are refused: with 17:46 and 17:50 a dose after 17:50
// could only ever go to one of them and the other beeped until its window
// closed. Window 30/60 → moments must be more than 90 minutes apart.
static void test_overlap_refused() {
    Schedule s;
    TEST_ASSERT_TRUE(s.add(17 * 60 + 46));
    TEST_ASSERT_FALSE(s.add(17 * 60 + 50));           // the real case
    TEST_ASSERT_FALSE(s.add(8 * 60 + 30 + 9 * 60));   // 17:30 — before, 16 min apart
    TEST_ASSERT_FALSE(s.add(19 * 60 + 16));           // exactly 90 min: windows touch at 18:46
    TEST_ASSERT_TRUE(s.add(19 * 60 + 17));            // 91 min — fine
    TEST_ASSERT_EQUAL_INT(2, s.state().n);
    TEST_ASSERT_FALSE(s.set_window(30, 61));          // 91 min gap no longer enough
    TEST_ASSERT_TRUE(s.set_window(30, 60));
    TEST_ASSERT_EQUAL_INT(60, s.state().after);

    // across midnight: 23:30 and 00:59 are 89 minutes apart
    Schedule m;
    TEST_ASSERT_TRUE(m.add(23 * 60 + 30));
    TEST_ASSERT_FALSE(m.add(59));
    TEST_ASSERT_TRUE(m.add(61));
    // a single moment overlaps itself the next day only with a 24 h window
    Schedule one;
    one.add(8 * 60);
    TEST_ASSERT_FALSE(one.set_window(720, 720));
    TEST_ASSERT_TRUE(one.set_window(720, 719));

    // a whole schedule from the web form or NVS is checked too
    Schedule::State form = {};
    form.n = 2;
    form.minute[0] = 17 * 60 + 50; form.minute[1] = 17 * 60 + 46;
    form.before = 30; form.after = 60;
    TEST_ASSERT_FALSE(s.restore(form));
    TEST_ASSERT_EQUAL_INT(2, s.state().n);  // the old schedule stays
    Schedule::State sorted = {};
    Schedule::insert(sorted, 17 * 60 + 50);
    Schedule::insert(sorted, 17 * 60 + 46);
    sorted.before = 30; sorted.after = 60;
    TEST_ASSERT_EQUAL_INT(1, Schedule::overlap(sorted));  // slot 1 (17:46) runs into 17:50
}

static void test_missed_once_then_next_day() {
    Schedule s;
    s.add(8 * 60);
    std::vector<Schedule::Event> ev = run(s, at(7, 0), at(23, 59));
    TEST_ASSERT_EQUAL_INT(1, count(ev, Schedule::MISSED, 1));
    // reminders at 08:00, 08:05, ... 09:00 — 13 of them, none after the window
    TEST_ASSERT_EQUAL_INT(13, count(ev, Schedule::REMIND, 1));
    TEST_ASSERT_EQUAL_INT(Schedule::MISSED_ST, s.view(0, at(12, 0)).state);
    // next day: missed again, exactly once
    ev = run(s, at(0, 0, 1), at(23, 59, 1));
    TEST_ASSERT_EQUAL_INT(1, count(ev, Schedule::MISSED, 1));
}

static void test_reminders_stop_on_dose_and_lift_postpones() {
    Schedule s;
    s.add(8 * 60);
    std::vector<Schedule::Event> ev = run(s, at(7, 0), at(8, 6));
    TEST_ASSERT_EQUAL_INT(2, count(ev, Schedule::REMIND, 1));  // 08:00, 08:05
    TEST_ASSERT_TRUE(s.due(at(8, 6)));
    TEST_ASSERT_EQUAL_INT(Schedule::DUE, s.view(0, at(8, 6)).state);
    s.lift(at(8, 7));                                            // the box is in the hand
    ev = run(s, at(8, 7), at(8, 11));
    TEST_ASSERT_EQUAL_INT(0, count(ev, Schedule::REMIND, 1));   // 08:10 skipped
    ev = run(s, at(8, 12), at(8, 12));
    TEST_ASSERT_EQUAL_INT(1, count(ev, Schedule::REMIND, 1));   // 08:12 = lift + 5
    s.dose(at(8, 13));
    TEST_ASSERT_FALSE(s.due(at(8, 13)));
    ev = run(s, at(8, 13), at(23, 59));
    TEST_ASSERT_EQUAL_INT(0, ev.size());                         // no more reminders, no missed
    Schedule::View v = s.view(0, at(12, 0));
    TEST_ASSERT_EQUAL_INT(Schedule::TAKEN, v.state);
    TEST_ASSERT_EQUAL_INT(8 * 60 + 13, Schedule::minute_of_day(v.at));
}

// Before the moment the window is open but silent; the page shows it as due.
static void test_before_moment_no_reminder() {
    Schedule s;
    s.add(8 * 60);
    std::vector<Schedule::Event> ev = run(s, at(7, 0), at(7, 59));
    TEST_ASSERT_EQUAL_INT(0, ev.size());
    TEST_ASSERT_FALSE(s.due(at(7, 45)));
    TEST_ASSERT_EQUAL_INT(Schedule::DUE, s.view(0, at(7, 45)).state);
    TEST_ASSERT_EQUAL_INT(Schedule::UPCOMING, s.view(0, at(7, 0)).state);
    // taken before the moment: on time, and no reminder at 08:00
    TEST_ASSERT_EQUAL_INT(Schedule::ON_TIME, s.dose(at(7, 50)).status);
    ev = run(s, at(8, 0), at(9, 30));
    TEST_ASSERT_EQUAL_INT(0, ev.size());
}

static void test_midnight_wrap() {
    Schedule s;
    s.add(23 * 60 + 30);  // window 23:00..00:30 next day
    Schedule::Dose d = s.dose(at(0, 20, 1));
    TEST_ASSERT_EQUAL_INT(Schedule::ON_TIME, d.status);
    TEST_ASSERT_EQUAL_INT(1, d.slot);
    TEST_ASSERT_EQUAL_INT(Schedule::OFF_SCHEDULE, s.dose(at(0, 31, 1)).status);

    Schedule m;
    m.add(23 * 60 + 30);
    std::vector<Schedule::Event> ev = run(m, at(22, 0), at(0, 29, 1));
    TEST_ASSERT_EQUAL_INT(0, count(ev, Schedule::MISSED, 1));
    TEST_ASSERT_TRUE(m.due(at(0, 20, 1)));
    TEST_ASSERT_EQUAL_INT(Schedule::DUE, m.view(0, at(0, 20, 1)).state);  // still that moment's window
    ev = run(m, at(0, 30, 1), at(0, 31, 1));
    TEST_ASSERT_EQUAL_INT(1, count(ev, Schedule::MISSED, 1));
}

// The clock arrived at 10:00: the 08:00 moment was not watched, so it is not
// missed; the 20:00 moment is handled as usual.
static void test_clock_arrives_late() {
    Schedule s;
    s.add(8 * 60);
    s.add(20 * 60);
    std::vector<Schedule::Event> ev = run(s, at(10, 0), at(23, 59));
    TEST_ASSERT_EQUAL_INT(0, count(ev, Schedule::MISSED, 1));
    TEST_ASSERT_EQUAL_INT(Schedule::UPCOMING, s.view(0, at(10, 0)).state);
    TEST_ASSERT_EQUAL_INT(1, count(ev, Schedule::MISSED, 2));
    TEST_ASSERT_EQUAL_INT(13, count(ev, Schedule::REMIND, 2));
}

// Power was lost at 08:10 and the clock came back at 08:20. The journal has the
// 08:05 dose: replayed before the first tick, it silences the reminder.
static void test_replay_before_ticks() {
    Schedule s;
    s.add(8 * 60);
    TEST_ASSERT_EQUAL_INT(Schedule::ON_TIME, s.dose(at(8, 5)).status);
    std::vector<Schedule::Event> ev = run(s, at(8, 20), at(9, 30));
    TEST_ASSERT_EQUAL_INT(0, ev.size());
    TEST_ASSERT_EQUAL_INT(Schedule::TAKEN, s.view(0, at(8, 20)).state);
}

static void test_state_roundtrip_and_validation() {
    Schedule a;
    a.add(20 * 60);
    a.add(8 * 60);
    a.set_window(20, 45);
    Schedule::State st = a.state();  // goes to NVS and back as is

    Schedule b;
    TEST_ASSERT_TRUE(b.restore(st));
    TEST_ASSERT_EQUAL_INT(2, b.state().n);
    TEST_ASSERT_EQUAL_INT(45, b.state().after);
    TEST_ASSERT_EQUAL_INT(Schedule::ON_TIME, b.dose(at(8, 40)).status);
    TEST_ASSERT_EQUAL_INT(Schedule::OFF_SCHEDULE, b.dose(at(8, 50)).status);

    Schedule::State bad = st;
    bad.minute[0] = 1500;
    TEST_ASSERT_FALSE(b.restore(bad));
    bad = st;
    bad.n = 7;
    TEST_ASSERT_FALSE(b.restore(bad));
    // an unsorted form with a duplicate comes back sorted and unique
    Schedule::State form = {};
    form.n = 3;
    form.minute[0] = 20 * 60; form.minute[1] = 8 * 60; form.minute[2] = 20 * 60;
    form.before = 30; form.after = 60;
    TEST_ASSERT_TRUE(b.restore(form));
    TEST_ASSERT_EQUAL_INT(2, b.state().n);
    TEST_ASSERT_EQUAL_INT(8 * 60, b.state().minute[0]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_empty_schedule);
    RUN_TEST(test_add_sorted_unique_full);
    RUN_TEST(test_window_edges);
    RUN_TEST(test_extra_dose_same_window);
    RUN_TEST(test_overlap_refused);
    RUN_TEST(test_missed_once_then_next_day);
    RUN_TEST(test_reminders_stop_on_dose_and_lift_postpones);
    RUN_TEST(test_before_moment_no_reminder);
    RUN_TEST(test_midnight_wrap);
    RUN_TEST(test_clock_arrives_late);
    RUN_TEST(test_replay_before_ticks);
    RUN_TEST(test_state_roundtrip_and_validation);
    return UNITY_END();
}
