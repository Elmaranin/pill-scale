#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "board/usb_console.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cmd.h"
#include "logbook.h"
#include "sched.h"
#include "web.h"
#include "board/wifi.h"
#include "board/buzzer.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const int N_RAW_DEFAULT = 10;
static const int N_READ_DEFAULT = 10;
static const int N_TARE_DEFAULT = 20;
static const int N_MAX = 600;  // 60 s at 10 SPS — waiting longer inside a command is pointless

// Optional numeric argument n with range check.
static bool parse_n(const char *s, int def, int &out) {
    if (!s) { out = def; return true; }
    char *end;
    long v = strtol(s, &end, 10);
    if (*end != '\0' || v < 1 || v > N_MAX) return false;
    out = (int)v;
    return true;
}

void CommandLine::poll() {
    read_input();
    if (!scale_.cell().ready()) return;

    // The ADC sample is read once and handed to everyone who needs it.
    // The "Scale" tab always gets it, even without calibration (raw counts).
    int32_t raw = scale_.cell().read();
    live(raw);
    if (streaming_) stream(raw);
    if (detecting_) detect(raw);
    if (tracking_) track(raw);
}

// What the last lift meant: nothing, dose, double, refill, another box.
// Always journaled; printed to USB only when event on is enabled.
void CommandLine::judge(uint16_t off_s) {
    float g = tracker_.after();
    if (!tracker_.reliable()) {
        if (print_events_) puts("E unsure");
        logbook_add(Journal::UNSURE, 0, off_s, tracker_.delta(), g);
        return;
    }
    double d = tracker_.delta();
    DoseBook::Result r = doses_.classify(d, tracker_.before());
    uint8_t type = Journal::NONE;
    // Schedule verdict for a dose (and a double): with the clock unknown — none.
    Schedule::Dose sd = {Schedule::NO_SCHEDULE, 0};
    char tail[32] = "";
    if (r.v == DoseBook::DOSE || r.v == DoseBook::DOUBLE) {
        sd = sched_dose();
        if (sd.status != Schedule::NO_SCHEDULE)
            snprintf(tail, sizeof(tail), " slot=%d sched=%s", sd.slot,
                     Journal::sched_name(sched_to_journal(sd.status)));
    }
    switch (r.v) {
    case DoseBook::NONE:
        type = Journal::NONE;
        if (print_events_) printf("E none delta=%.3f\n", d);
        break;
    case DoseBook::REFILL:
        type = Journal::REFILL;
        if (print_events_) printf("E refill g=%.3f\n", d);
        break;
    case DoseBook::NEWBOX:
        type = Journal::NEWBOX;
        if (print_events_) printf("E newbox g=%.3f\n", d);
        break;
    case DoseBook::DOUBLE:
        type = Journal::DOUBLE;
        if (print_events_) printf("E double kind=%d g=%.3f%s\n", r.kind, -d, tail);
        break;
    case DoseBook::DOSE:
        type = Journal::DOSE;
        if (print_events_)
            printf("E dose kind=%d n=%d g=%.3f%s%s\n", r.kind, r.count, -d, r.fresh ? " new" : "", tail);
        save_doses();  // a kind was updated or added — persist to NVS (a couple of times a day)
        break;
    }
    logbook_add(type, Journal::pack((uint8_t)r.kind, sched_to_journal(sd.status), (uint8_t)sd.slot),
                off_s, (float)d, g);
}

// Learned doses in NVS: namespace "doses", key "book" — the struct as is.
void CommandLine::load_doses() {
    doses_loaded_ = true;
    nvs_handle_t h;
    if (nvs_open("doses", NVS_READONLY, &h) != ESP_OK) return;
    DoseBook::State st;
    size_t len = sizeof(st);
    if (nvs_get_blob(h, "book", &st, &len) == ESP_OK && len == sizeof(st)) doses_.restore(st);
    nvs_close(h);
}

void CommandLine::save_doses() {
    nvs_handle_t h;
    if (nvs_open("doses", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "book", &doses_.state(), sizeof(DoseBook::State));
    nvs_commit(h);
    nvs_close(h);
}

// Lift tracker: events placed / removed / returned / changed / missing
// (docs/api.md). Everything goes to the journal; to USB only with event on.
void CommandLine::track(int32_t raw) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint16_t off_s = (uint16_t)((now - removed_ms_) / 1000 < 65535 ? (now - removed_ms_) / 1000 : 65535);
    switch (tracker_.add(scale_.to_grams(raw))) {
    case LiftTracker::PLACED:
        if (print_events_) printf("E placed g=%.3f\n", tracker_.box());
        logbook_add(Journal::PLACED, 0, 0, 0, (float)tracker_.box());
        break;
    case LiftTracker::REMOVED:
        removed_ms_ = now;
        sched_lift();  // the person is at the scale: hold the reminder
        if (print_events_) printf("E removed g=%.3f\n", tracker_.before());
        logbook_add(Journal::REMOVED, 0, 0, 0, (float)tracker_.before());
        break;
    case LiftTracker::RETURNED:
        if (print_events_)
            printf("E returned delta=%.3f off_s=%.1f g=%.3f%s\n", tracker_.delta(),
                   (now - removed_ms_) / 1000.0, tracker_.after(), tracker_.reliable() ? "" : " ok=0");
        judge(off_s);
        break;
    case LiftTracker::CHANGED:
        if (print_events_) printf("E changed delta=%.3f g=%.3f\n", tracker_.delta(), tracker_.after());
        judge(0);
        break;
    case LiftTracker::MISSING:
        if (print_events_) printf("E missing off_s=%.1f\n", (now - removed_ms_) / 1000.0);
        logbook_add(Journal::MISSING, 0, off_s, 0, (float)tracker_.level());
        break;
    default:
        break;
    }

    // Weight every 10 minutes — so the journal shows the drift over a week
    uint32_t s = logbook_uptime_s();
    if (s - status_s_ >= 600) {
        status_s_ = s;
        logbook_add(Journal::STATUS, 0, 0, 0, (float)tracker_.level());
    }
    if (++snap_n_ >= 10) { snap_n_ = 0; snapshot(); }  // ~once a second
}

// Mean of the last LIVE_N samples — for the "Scale" tab. Does not touch the
// tracker or the journal: they have their own ~1 s blocks.
void CommandLine::live(int32_t raw) {
    live_raw_[live_i_] = raw;
    live_i_ = (live_i_ + 1) % LIVE_N;
    if (live_n_ < LIVE_N) live_n_++;
    RunningStats s;
    for (int i = 0; i < live_n_; i++) s.add(live_raw_[i]);
    Live l;
    l.calibrated = scale_.scale() != 1.0f;
    l.raw = raw;
    l.offset = scale_.offset();
    l.scale = scale_.scale();
    l.g = scale_.to_grams(s.mean());
    l.sd = (float)(s.sd() / fabs(scale_.scale()));
    l.n = (uint8_t)live_n_;
    logbook_set_live(l);
}

void CommandLine::snapshot() {
    Snapshot sn;
    sn.weight = (float)tracker_.level();
    sn.box = (float)tracker_.box();
    sn.box_on = tracker_.box_on();
    if (!sn.box_on && removed_ms_)
        sn.off_s = ((uint32_t)(esp_timer_get_time() / 1000) - removed_ms_) / 1000;
    sn.calibrated = scale_.scale() != 1.0f;
    const DoseBook::State &st = doses_.state();
    sn.kinds = st.n;
    for (int i = 0; i < st.n && i < 4; i++) { sn.kind_g[i] = st.k[i].mean; sn.kind_n[i] = st.k[i].count; }
    logbook_set_snapshot(sn);
}

void CommandLine::begin_tracking() {
    if (tracking_) return;
    if (scale_.scale() == 1.0f) return;  // not calibrated — thresholds in grams are meaningless
    LiftTracker::Config cfg;
    cfg.window = scale_.cell().sps();  // ~1 s block
    tracker_ = LiftTracker(cfg);
    if (!doses_loaded_) load_doses();
    status_s_ = logbook_uptime_s();
    tracking_ = true;
}

// Detector: prints an event when the weight has settled at a new level.
void CommandLine::detect(int32_t raw) {
    StepDetector::Event e = detector_.add(scale_.to_grams(raw));
    if (e == StepDetector::NONE) return;
    printf("E %s delta=%.3f g=%.3f\n", e == StepDetector::ADDED ? "added" : "removed",
           detector_.delta(), detector_.level());
}

// Stream: every sample goes into the block; a full block is printed.
void CommandLine::stream(int32_t raw_sample) {
    block_.add(raw_sample);
    if ((int)block_.count() < stream_n_) return;

    unsigned long t = (unsigned long)(esp_timer_get_time() / 1000);
    double raw = block_.mean();
    if (stream_n_ == 1) {
        printf("D %lu %ld %.3f\n", t, (long)raw, scale_.to_grams(raw));
    } else {
        printf("D %lu %.1f %.3f\n", t, raw, scale_.to_grams(raw));
    }
    block_.reset();
}

void CommandLine::read_input() {
    char chunk[32];
    size_t got = console_read(chunk, sizeof(chunk));
    for (size_t i = 0; i < got; i++) {
        char c = chunk[i];
        if (c == '\r') continue;
        if (c == '\n') {
            buf_[len_] = '\0';
            if (len_ > 0) execute(buf_);
            len_ = 0;
        } else if (len_ < (int)sizeof(buf_) - 1) {
            buf_[len_++] = c;
        }
        // An overlong line is silently truncated — commands are short.
    }
}

// "OK n=2 window=30/60 1=08:00 2=20:00"
void CommandLine::print_schedule() {
    Schedule::State st = sched_state();
    printf("OK n=%u window=%u/%u", st.n, st.before, st.after);
    for (int i = 0; i < st.n; i++) printf(" %d=%02u:%02u", i + 1, st.minute[i] / 60, st.minute[i] % 60);
    printf("\n");
}

// "HH:MM" → minutes after midnight; false — not a time.
static bool parse_hhmm(const char *s, uint16_t &out) {
    if (!s) return false;
    int h, m;
    char rest;
    if (sscanf(s, "%d:%d%c", &h, &m, &rest) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return false;
    out = (uint16_t)(h * 60 + m);
    return true;
}

// schedule | add HH:MM | del n | window before after | clear (docs/api.md)
void CommandLine::schedule_cmd(const char *sub, const char *a, const char *b) {
    if (!sub) { print_schedule(); return; }
    if (!strcasecmp(sub, "add")) {
        uint16_t minute;
        if (!parse_hhmm(a, minute)) { puts("ERR bad_argument"); return; }
        Schedule::State t = sched_state();  // try on a copy to tell why it is refused
        if (t.n >= Schedule::MAX_SLOTS) { puts("ERR full"); return; }
        if (!Schedule::insert(t, minute)) { puts("ERR bad_argument"); return; }  // already there
        if (Schedule::overlap(t)) { puts("ERR overlap"); return; }
        if (!sched_add(minute)) { puts("ERR bad_argument"); return; }
    } else if (!strcasecmp(sub, "del")) {
        if (!a || !sched_del(atoi(a))) { puts("ERR bad_argument"); return; }
    } else if (!strcasecmp(sub, "window")) {
        if (!a || !b) { puts("ERR bad_argument"); return; }
        int before = atoi(a), after = atoi(b);
        if (before < 0 || before > 720 || after < 0 || after > 720) { puts("ERR bad_argument"); return; }
        Schedule::State t = sched_state();
        t.before = (uint16_t)before;
        t.after = (uint16_t)after;
        if (Schedule::overlap(t)) { puts("ERR overlap"); return; }
        if (!sched_window(before, after)) { puts("ERR bad_argument"); return; }
    } else if (!strcasecmp(sub, "clear")) {
        sched_clear();
    } else {
        puts("ERR bad_argument");
        return;
    }
    print_schedule();
}

void CommandLine::execute(char *line) {
    char *cmd = strtok(line, " \t");
    char *a1 = strtok(nullptr, " \t");
    char *a2 = strtok(nullptr, " \t");
    char *a3 = strtok(nullptr, " \t");
    if (!cmd) return;
    // Commands are lowercase; input in any case is folded to lowercase.
    for (char *p = cmd; *p; p++) *p = (char)tolower(*p);

    RunningStats s;
    int n;

    if (!strcmp(cmd, "ping")) {
        puts("OK pong");

    } else if (!strcmp(cmd, "info")) {
        printf("OK fw=%s adc=%s offset=%ld scale=%.4f sps=%d\n",
               esp_app_get_description()->version, scale_.cell().name(),
               (long)scale_.offset(), scale_.scale(), scale_.cell().sps());

    } else if (!strcmp(cmd, "raw")) {
        if (!parse_n(a1, N_RAW_DEFAULT, n)) { puts("ERR bad_argument"); return; }
        if (!scale_.collect(n, s)) { puts("ERR adc_timeout"); return; }
        printf("OK raw=%.1f n=%lu sd=%.1f\n",
               s.mean(), (unsigned long)s.count(), s.sd());

    } else if (!strcmp(cmd, "read")) {
        if (!parse_n(a1, N_READ_DEFAULT, n)) { puts("ERR bad_argument"); return; }
        if (!scale_.collect(n, s)) { puts("ERR adc_timeout"); return; }
        // sd in grams: divide the spread of counts by the absolute scale factor
        printf("OK g=%.3f n=%lu sd=%.3f\n",
               scale_.to_grams(s.mean()), (unsigned long)s.count(),
               s.sd() / fabsf(scale_.scale()));

    } else if (!strcmp(cmd, "tare")) {
        if (!parse_n(a1, N_TARE_DEFAULT, n)) { puts("ERR bad_argument"); return; }
        if (!scale_.tare(n)) { puts("ERR adc_timeout"); return; }
        printf("OK offset=%ld\n", (long)scale_.offset());

    } else if (!strcmp(cmd, "cal")) {
        if (!a1) { puts("ERR bad_argument"); return; }
        float g = strtof(a1, nullptr);
        if (g <= 0 || !parse_n(a2, N_TARE_DEFAULT, n)) { puts("ERR bad_argument"); return; }
        if (scale_.offset() == 0) { puts("ERR not_tared"); return; }
        if (!scale_.calibrate(g, n)) { puts("ERR adc_timeout"); return; }
        printf("OK scale=%.4f\n", scale_.scale());
        begin_tracking();  // calibrated — the tracker can run now

    } else if (!strcmp(cmd, "stream")) {
        if (!a1) { puts("ERR bad_argument"); return; }
        for (char *p = a1; *p; p++) *p = (char)tolower(*p);
        if (!strcmp(a1, "on")) {
            if (!parse_n(a2, 1, n)) { puts("ERR bad_argument"); return; }
            stream_n_ = n;
            block_.reset();  // start the block afresh, without old samples
            streaming_ = true;
            printf("OK stream=on n=%d\n", stream_n_);
        } else if (!strcmp(a1, "off")) {
            streaming_ = false;
            puts("OK stream=off");
        } else {
            puts("ERR bad_argument");
        }

    } else if (!strcmp(cmd, "detect")) {
        if (!a1) { puts("ERR bad_argument"); return; }
        for (char *p = a1; *p; p++) *p = (char)tolower(*p);
        if (!strcmp(a1, "on")) {
            // Detector thresholds are in grams — meaningless without calibration.
            if (scale_.scale() == 1.0f) { puts("ERR not_calibrated"); return; }
            StepDetector::Config cfg;
            cfg.window = scale_.cell().sps();  // ~1 s block
            detector_ = StepDetector(cfg);
            detecting_ = true;
            puts("OK detect=on");
        } else if (!strcmp(a1, "off")) {
            detecting_ = false;
            puts("OK detect=off");
        } else {
            puts("ERR bad_argument");
        }

    } else if (!strcmp(cmd, "event")) {
        if (!a1) { puts("ERR bad_argument"); return; }
        for (char *p = a1; *p; p++) *p = (char)tolower(*p);
        if (!strcmp(a1, "on")) {
            // Tracker thresholds are in grams — meaningless without calibration.
            if (scale_.scale() == 1.0f) { puts("ERR not_calibrated"); return; }
            begin_tracking();  // usually already running since power-on
            print_events_ = true;
            sched_print_events(true);
            puts("OK event=on");
        } else if (!strcmp(a1, "off")) {
            print_events_ = false;  // the tracker and the journal keep running
            sched_print_events(false);
            puts("OK event=off");
        } else {
            puts("ERR bad_argument");
        }

    } else if (!strcmp(cmd, "time")) {
        if (a1) {
            char *end;
            long long unix = strtoll(a1, &end, 10);
            int tz = a2 ? atoi(a2) : logbook_tz();
            if (*end != '\0' || unix < 1700000000LL || tz < -720 || tz > 840) { puts("ERR bad_argument"); return; }
            logbook_set_time((uint32_t)unix, tz);
        }
        if (logbook_time_known())
            printf("OK known=1 unix=%lu tz=%d\n", (unsigned long)logbook_now_unix(), logbook_tz());
        else
            printf("OK known=0 uptime_s=%lu\n", (unsigned long)logbook_uptime_s());

    } else if (!strcmp(cmd, "log")) {
        if (!logbook_ok()) { puts("ERR no_log_partition"); return; }
        if (a1 && !strcasecmp(a1, "clear")) {
            puts(logbook_clear() ? "OK" : "ERR flash");
            return;
        }
        int want;
        if (!parse_n(a1, 20, want)) { puts("ERR bad_argument"); return; }
        uint32_t have = logbook_count();
        uint32_t k = (uint32_t)want < have ? (uint32_t)want : have;
        for (uint32_t i = k; i-- > 0;) {  // oldest to newest
            Record r;
            if (!logbook_recent(i, r)) continue;
            char t[24];
            logbook_format_time(r, t, sizeof(t));
            printf("L %lu boot=%lu up=%lu t=%s %s", (unsigned long)r.seq, (unsigned long)r.boot,
                   (unsigned long)r.uptime_s, t, Journal::name(r.type));
            if (Journal::dose_kind(r.kind)) printf(" kind=%u", Journal::dose_kind(r.kind));
            if (Journal::slot(r.kind)) printf(" slot=%u", Journal::slot(r.kind));
            if (Journal::sched(r.kind)) printf(" sched=%s", Journal::sched_name(Journal::sched(r.kind)));
            if (r.off_s) printf(" off=%u", r.off_s);
            if (r.delta != 0) printf(" d=%.3f", r.delta);
            printf(" g=%.3f\n", r.weight);
        }
        printf("OK n=%lu total=%lu\n", (unsigned long)k, (unsigned long)logbook_total());

    } else if (!strcmp(cmd, "net")) {
        NetInfo n;
        wifi_info(n);
        printf("OK wifi=%s ssid=%s ip=%s", n.mode, n.ssid[0] ? n.ssid : "-", n.ip);
        if (!strcmp(n.mode, "ap")) printf(" clients=%d", n.clients);
        if (!strcmp(n.mode, "sta")) printf(" ntp=%s", n.ntp);
        printf(" web=%s\n", web_running() ? "on" : "off");

    } else if (!strcmp(cmd, "wifi")) {
        // Home network in NVS; takes effect after reboot. The password is never printed.
        if (a1 && !strcasecmp(a1, "clear")) {
            if (!wifi_sta_set("", "")) { puts("ERR nvs"); return; }
        } else if (a1) {
            if (!wifi_sta_set(a1, a2 ? a2 : "")) { puts("ERR bad_argument"); return; }
        }
        char ssid[33];
        if (wifi_sta_get(ssid, sizeof(ssid))) printf("OK set=1 ssid=%s\n", ssid);
        else puts("OK set=0");

    } else if (!strcmp(cmd, "beep")) {
        if (!buzzer_present()) { puts("ERR no_buzzer"); return; }
        long ms = 200;
        if (a1) {
            char *end;
            ms = strtol(a1, &end, 10);
            if (*end != '\0' || ms < 1 || ms > 5000) { puts("ERR bad_argument"); return; }
        }
        long hz = buzzer_hz();
        if (a2 && hz) {
            char *end;
            hz = strtol(a2, &end, 10);
            if (*end != '\0' || hz < 100 || hz > 10000) { puts("ERR bad_argument"); return; }
        }
        buzzer_beep((uint32_t)ms, (uint32_t)hz);
        printf("OK ms=%ld hz=%ld\n", ms, hz);

    } else if (!strcmp(cmd, "reboot")) {
        puts("OK");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));  // let the line leave over USB

        esp_restart();

    } else if (!strcmp(cmd, "schedule")) {
        schedule_cmd(a1, a2, a3);

    } else if (!strcmp(cmd, "doses")) {
        if (!doses_loaded_) load_doses();
        const DoseBook::State &st = doses_.state();
        printf("OK n=%d", st.n);
        for (int i = 0; i < st.n; i++) printf(" %d=%.3fx%u", i + 1, st.k[i].mean, (unsigned)st.k[i].count);
        printf("\n");

    } else if (!strcmp(cmd, "reset")) {
        if (a1 && !strcasecmp(a1, "cal")) {
            scale_.reset_calibration();
        } else if (a1 && !strcasecmp(a1, "doses")) {
            doses_.clear();
            doses_loaded_ = true;
            save_doses();
        } else {
            puts("ERR bad_argument");
            return;
        }
        puts("OK");

    } else {
        puts("ERR unknown_command");
    }
}
