// Web page on the scale: state, journal, CSV, time from the browser.
// URLs: docs/api.md, "Веб-страница". Runs in the web server's own task; takes
// data from logbook (mutex inside), measures nothing itself.
// UI strings are Russian on purpose: the page is what the user sees.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "esp_http_server.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board/wifi.h"
#include "logbook.h"
#include "sched.h"
#include "web.h"

static const int PAGE_ROWS = 100;  // how many recent events on the page

// Schedule verdict stored with a dose: " · по расписанию" and the like.
static const char *sched_suffix(const Record &r) {
    switch (Journal::sched(r.kind)) {
    case Journal::S_ON:    return " · по расписанию";
    case Journal::S_EXTRA: return " · <b>ПОВТОРНО</b> в окне приёма";
    case Journal::S_OFF:   return " · вне расписания";
    }
    return "";
}

// "08:00" for moment k of the current schedule, or "момент k" if it is gone.
static void moment(int slot, char *buf, size_t n) {
    Schedule::State st = sched_state();
    if (slot >= 1 && slot <= st.n) snprintf(buf, n, "%02u:%02u", st.minute[slot - 1] / 60, st.minute[slot - 1] % 60);
    else snprintf(buf, n, "момент %d", slot);
}

static const char *label(const Record &r, char *buf, size_t n) {
    char m[24];
    switch (r.type) {
    case Journal::BOOT:    return "включение";
    case Journal::TIMESET: return "время установлено";
    case Journal::PLACED:  return "поставили коробку";
    case Journal::REMOVED: return "сняли коробку";
    case Journal::MISSING: return "коробки давно нет";
    case Journal::NONE:    return "ничего не взяли";
    case Journal::DOSE:    snprintf(buf, n, "доза (вид %u)%s", Journal::dose_kind(r.kind), sched_suffix(r)); return buf;
    case Journal::DOUBLE:  snprintf(buf, n, "<b>ДВОЙНАЯ доза</b> (вид %u)%s", Journal::dose_kind(r.kind), sched_suffix(r)); return buf;
    case Journal::REFILL:  return "пополнение";
    case Journal::NEWBOX:  return "другая коробка";
    case Journal::UNSURE:  return "не понял";
    case Journal::STATUS:  return "вес";
    case Journal::MISSED:  moment(Journal::slot(r.kind), m, sizeof(m));
                           snprintf(buf, n, "<b>ПРОПУЩЕН</b> приём %s", m); return buf;
    }
    return "?";
}

// Record time for a human: local, or "power-on N, +h:mm:ss".
static void when(const Record &r, char *buf, size_t n) {
    logbook_format_time(r, buf, n);
    if (strcmp(buf, "-") == 0) {
        unsigned s = r.uptime_s;
        snprintf(buf, n, "вкл. %lu, +%u:%02u:%02u", (unsigned long)r.boot, s / 3600, s / 60 % 60, s % 60);
    } else {
        buf[10] = ' ';  // 2026-10-25T08:05:12 → 2026-10-25 08:05:12
    }
}

// Page header: style and tabs. active — URL of the current tab.
static void head(httpd_req_t *req, const char *active) {
    char b[640];
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html lang=ru><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'><title>pillscale</title>"
        "<style>body{font-family:system-ui,sans-serif;margin:16px;max-width:760px;color:#111;background:#fff}"
        "table{border-collapse:collapse;width:100%}td,th{padding:5px 6px;border-bottom:1px solid #ddd;"
        "text-align:left;font-size:14px}td.n{text-align:right;font-variant-numeric:tabular-nums}"
        ".m{color:#555}h1{font-size:22px;display:inline-block;margin:0 16px 12px 0}"
        ".nav{display:flex;flex-wrap:wrap;gap:6px 14px;margin-bottom:4px}"  // five tabs do not fit a phone in one line
        ".nav a.on{font-weight:bold;text-decoration:none;color:#111}"
        "label{display:block;margin:10px 0 4px}input[type=text],input[type=password]{font-size:16px;"
        "padding:6px;width:100%;max-width:320px;box-sizing:border-box}"
        "button{font-size:16px;padding:8px 14px;margin:12px 8px 0 0}.ok{color:#060}"
        ".card{border:1px solid #ddd;border-radius:10px;padding:12px 14px;margin:12px 0}"
        ".card h3{font-size:13px;font-weight:600;color:#555;margin:0 0 6px;text-transform:uppercase;letter-spacing:.03em}"
        ".big2{font-size:24px;font-weight:600}.warn{color:#b00}.due{background:#fff4d6;border-color:#e0b000}"
        ".alarm{background:#fde8e8;border-color:#d33}.nums{display:flex;flex-wrap:wrap;gap:8px 20px}"
        ".nums div{min-width:84px}.nums b{display:block;font-size:22px}</style></head><body>"
        "<h1>pillscale</h1>");
    snprintf(b, sizeof(b),
             "<span class=nav><a href='/'%s>Сводка</a><a href='/log'%s>Журнал</a>"
             "<a href='/schedule'%s>Расписание</a><a href='/scale'%s>Весы</a><a href='/settings'%s>Настройки</a></span>",
             active[0] == '\0' || !strcmp(active, "/") ? " class=on" : "",
             !strcmp(active, "/log") ? " class=on" : "",
             !strcmp(active, "/schedule") ? " class=on" : "",
             !strcmp(active, "/scale") ? " class=on" : "",
             !strcmp(active, "/settings") ? " class=on" : "");
    httpd_resp_sendstr_chunk(req, b);
}

// ---- "Summary" tab: the landing page ----
// What a person wants at a glance: is the box there, what is due next, how
// today went, the last dose, the last week. Details are on the other tabs.

static void hhmm(int minute, char *b, size_t n) { snprintf(b, n, "%02d:%02d", minute / 60 % 24, minute % 60); }

// A duration for a human: "40 мин", "2 ч 05 мин", "3 дн".
static void span(int64_t min, char *b, size_t n) {
    if (min < 0) min = 0;
    if (min < 60) snprintf(b, n, "%d мин", (int)min);
    else if (min < 48 * 60) snprintf(b, n, "%d ч %02d мин", (int)(min / 60), (int)(min % 60));
    else snprintf(b, n, "%d дн", (int)(min / Schedule::DAY));
}

// The last week from the journal, newest first. Stops at the first timed record
// older than 7 days; records with unknown time (power-ons that never learned
// it) are skipped, they cannot be placed in the week.
struct Week {
    int on, extra, off, unscheduled, missed, doubles;
    bool have_last;
    Record last;        // the newest dose
    uint32_t last_utc;  // its time, 0 — unknown
};

static void scan_week(Week &w, uint32_t now_utc) {
    memset(&w, 0, sizeof(w));
    for (uint32_t i = 0; i < 3000; i++) {  // a week is ~1200 records, status every 10 min included
        Record r;
        if (!logbook_recent(i, r)) break;
        bool dose = r.type == Journal::DOSE || r.type == Journal::DOUBLE;
        uint32_t t = logbook_record_utc(r);
        if (dose && !w.have_last) { w.have_last = true; w.last = r; w.last_utc = t; }
        if (!now_utc) { if (w.have_last) break; continue; }  // no clock — only the last dose
        if (t && t + 7 * 86400 < now_utc) break;
        if (!t) continue;
        if (r.type == Journal::MISSED) w.missed++;
        if (!dose) continue;
        if (r.type == Journal::DOUBLE) w.doubles++;
        switch (Journal::sched(r.kind)) {
        case Journal::S_ON:    w.on++; break;
        case Journal::S_EXTRA: w.extra++; break;
        case Journal::S_OFF:   w.off++; break;
        default:               w.unscheduled++; break;
        }
    }
}

static esp_err_t summary_page(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char b[512], t1[24], t2[24];
    Snapshot s = logbook_snapshot();
    bool known = logbook_time_known();
    int64_t now = 0;
    sched_local_now(now);
    int nowm = Schedule::minute_of_day(now);
    Schedule::State st = sched_state();

    head(req, "/");

    // 1. Time. Without it the schedule cannot work — say so first.
    if (known) {
        hhmm(nowm, t1, sizeof(t1));
        snprintf(b, sizeof(b), "<p class=m>Сейчас %s%s</p>", t1, wifi_ntp_synced() ? " · время с интернета" : "");
    } else {
        snprintf(b, sizeof(b), "<div class='card alarm'><b>Время на весах неизвестно</b> — страница его сейчас "
                 "установит. Пока времени нет, напоминаний нет, дозы пишутся без расписания.</div>");
    }
    httpd_resp_sendstr_chunk(req, b);

    // 2. The box.
    if (!s.calibrated) {
        snprintf(b, sizeof(b), "<div class='card alarm'><h3>Весы</h3><b>Не откалиброваны</b> — автомат не работает "
                 "(команды tare и cal по USB).</div>");
    } else if (s.box_on) {
        snprintf(b, sizeof(b), "<div class=card><h3>Таблетница</h3><span class=big2>На весах</span> "
                 "<span class=m>· %.2f г</span></div>", s.box);
    } else if (s.off_s) {
        span(s.off_s / 60, t1, sizeof(t1));
        snprintf(b, sizeof(b), "<div class='card%s'><h3>Таблетница</h3><span class=big2>Снята</span> "
                 "<span class=m>· %s назад</span>%s</div>", s.off_s > 510 ? " due" : "",
                 s.off_s < 60 ? "меньше минуты" : t1,
                 s.off_s > 510 ? "<br>Давно не возвращали — поставьте коробку на весы." : "");
    } else {
        snprintf(b, sizeof(b), "<div class='card due'><h3>Таблетница</h3><span class=big2>Нет на весах</span>"
                 "<br><span class=m>Поставьте коробку — весы запомнят её вес.</span></div>");
    }
    httpd_resp_sendstr_chunk(req, b);

    // 3. Now / next.
    if (known && st.n) {
        int due = -1, next = -1, next_d = 0;
        for (int i = 0; i < st.n; i++) {
            Schedule::View v = sched_view(i);
            if (v.state == Schedule::DUE && due < 0) { due = i; continue; }
            int d = ((int)st.minute[i] - nowm + Schedule::DAY) % Schedule::DAY;
            // taken early or missed today, moment still ahead: its next turn is tomorrow
            if ((v.state == Schedule::TAKEN || v.state == Schedule::MISSED_ST) && st.minute[i] > nowm) d += Schedule::DAY;
            if (v.state == Schedule::DUE) continue;
            if (next < 0 || d < next_d) { next = i; next_d = d; }
        }
        if (due >= 0) {
            int late = (nowm - (int)st.minute[due] + Schedule::DAY) % Schedule::DAY;
            bool after = late <= st.after;  // past the moment, still inside the window
            hhmm(st.minute[due], t1, sizeof(t1));
            if (after) {
                char lateness[64] = "только что";
                if (late) { span(late, t2, sizeof(t2)); snprintf(lateness, sizeof(lateness), "опоздание %s", t2); }
                snprintf(b, sizeof(b), "<div class='card alarm'><h3>Сейчас</h3><span class=big2>⏰ Пора принять — %s</span>"
                         "<br>%s · окно закроется через %d мин</div>", t1, lateness, (int)st.after - late);
            } else {
                int ahead = Schedule::DAY - late;
                snprintf(b, sizeof(b), "<div class='card due'><h3>Сейчас</h3><span class=big2>Окно приёма открыто</span>"
                         "<br>приём в %s, через %d мин — можно уже сейчас</div>", t1, ahead);
            }
            httpd_resp_sendstr_chunk(req, b);
        } else if (next >= 0) {
            hhmm(st.minute[next], t1, sizeof(t1));
            span(next_d, t2, sizeof(t2));
            snprintf(b, sizeof(b), "<div class=card><h3>Следующий приём</h3><span class=big2>%s%s</span>"
                     " <span class=m>· через %s</span></div>", nowm + next_d >= Schedule::DAY ? "завтра " : "", t1, t2);
            httpd_resp_sendstr_chunk(req, b);
        }
    } else if (known) {
        httpd_resp_sendstr_chunk(req, "<div class=card><h3>Расписание</h3>Не задано — дозы записываются, "
                                      "напоминаний нет. <a href='/schedule'>Задать</a></div>");
    }

    // 4. Today, moment by moment.
    if (known && st.n) {
        httpd_resp_sendstr_chunk(req, "<div class=card><h3>Сегодня</h3><table>");
        for (int i = 0; i < st.n; i++) {
            Schedule::View v = sched_view(i);
            const char *what = "<span class=m>ждём</span>";
            char w[96];
            switch (v.state) {
            case Schedule::TAKEN:
                hhmm(Schedule::minute_of_day(v.at), t2, sizeof(t2));
                snprintf(w, sizeof(w), "<span class=ok>✓ принято в %s</span>", t2);
                what = w;
                break;
            case Schedule::DUE: {
                int late = (nowm - (int)v.minute + Schedule::DAY) % Schedule::DAY;  // wraps midnight
                what = late <= st.after ? "<b class=warn>⏰ пора</b>" : "<b>окно открыто</b>";
                break;
            }
            case Schedule::MISSED_ST:
                what = "<b class=warn>✗ пропущено</b>";
                break;
            default:
                // the moment is past, but the scale did not watch it (no clock then, or the schedule changed)
                if (v.minute + st.after < nowm) what = "<span class=m>— не отслеживалось</span>";
                break;
            }
            hhmm(v.minute, t1, sizeof(t1));
            snprintf(b, sizeof(b), "<tr><td><b>%s</b></td><td>%s</td></tr>", t1, what);
            httpd_resp_sendstr_chunk(req, b);
        }
        httpd_resp_sendstr_chunk(req, "</table></div>");
    }

    // 5. The last dose and 6. the last week — both from the journal.
    Week w;
    scan_week(w, known ? logbook_now_unix() : 0);
    if (w.have_last) {
        const Record &r = w.last;
        const char *how = "";
        switch (Journal::sched(r.kind)) {
        case Journal::S_ON:    how = " · по расписанию"; break;
        case Journal::S_EXTRA: how = " · <b class=warn>повторно</b>"; break;
        case Journal::S_OFF:   how = " · вне расписания"; break;
        }
        if (w.last_utc) {
            int64_t local = (int64_t)w.last_utc + logbook_tz() * 60;
            int64_t lmin = local / 60;
            hhmm(Schedule::minute_of_day(lmin), t1, sizeof(t1));
            const char *day = "";
            char dd[16] = "";
            if (known) {
                int64_t days = now / Schedule::DAY - lmin / Schedule::DAY;
                if (days == 0) day = "сегодня ";
                else if (days == 1) day = "вчера ";
                else { time_t lt = (time_t)local; struct tm tm; gmtime_r(&lt, &tm); strftime(dd, sizeof(dd), "%d.%m ", &tm); day = dd; }
                span(now - lmin, t2, sizeof(t2));
                snprintf(b, sizeof(b), "<div class=card><h3>Последняя доза</h3><span class=big2>%s%s</span> "
                         "<span class=m>· %s назад</span><br>%s%.3f г%s</div>", day, t1, t2,
                         r.type == Journal::DOUBLE ? "<b class=warn>двойная</b> · " : "", -r.delta, how);
            } else {
                snprintf(b, sizeof(b), "<div class=card><h3>Последняя доза</h3><span class=big2>%s</span><br>%.3f г%s</div>",
                         t1, -r.delta, how);
            }
        } else {
            snprintf(b, sizeof(b), "<div class=card><h3>Последняя доза</h3>%.3f г <span class=m>· время неизвестно "
                     "(было до установки часов)</span></div>", -r.delta);
        }
        httpd_resp_sendstr_chunk(req, b);
    } else {
        httpd_resp_sendstr_chunk(req, "<div class=card><h3>Последняя доза</h3><span class=m>Доз ещё не было</span></div>");
    }

    if (known) {
        snprintf(b, sizeof(b), "<div class=card><h3>За 7 дней</h3><div class=nums>"
                 "<div><b class=ok>%d</b>по расписанию</div>"
                 "<div><b%s>%d</b>пропущено</div>"
                 "<div><b>%d</b>вне расписания</div>"
                 "<div><b%s>%d</b>повторно</div>"
                 "<div><b%s>%d</b>двойных</div>",
                 w.on, w.missed ? " class=warn" : "", w.missed, w.off,
                 w.extra ? " class=warn" : "", w.extra, w.doubles ? " class=warn" : "", w.doubles);
        httpd_resp_sendstr_chunk(req, b);
        // doses with no verdict: taken while there was no schedule (or no clock) — not "off schedule"
        if (w.unscheduled) {
            snprintf(b, sizeof(b), "<div><b class=m>%d</b>без расписания</div>", w.unscheduled);
            httpd_resp_sendstr_chunk(req, b);
        }
        httpd_resp_sendstr_chunk(req, "</div></div>");
    }

    // 7. Small print: how long the scale has been up, journal fill.
    uint32_t up = logbook_uptime_s();
    span(up / 60, t1, sizeof(t1));
    snprintf(b, sizeof(b), "<p class=m style='font-size:12px'>Весы работают %s · в журнале %lu записей · "
             "<a href='/log'>журнал</a> · <a href='/log.csv'>CSV</a></p>",
             t1, (unsigned long)logbook_count());
    httpd_resp_sendstr_chunk(req, b);
    if (!logbook_ok())
        httpd_resp_sendstr_chunk(req, "<p class=warn>Нет раздела журнала во флеше — прошить с новым partitions.csv.</p>");

    // Phone time goes to the scale if it does not know it; refresh every 15 s.
    snprintf(b, sizeof(b),
             "<script>var known=%d;"
             "if(!known)fetch('/time?unix='+Math.floor(Date.now()/1000)+'&tz='+(-new Date().getTimezoneOffset()),"
             "{method:'POST'}).then(function(r){if(r.ok)location.reload()});"
             "setTimeout(function(){location.reload()},15000);</script></body></html>",
             known ? 1 : 0);
    httpd_resp_sendstr_chunk(req, b);
    return httpd_resp_sendstr_chunk(req, nullptr);
}

static esp_err_t log_page(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char b[512];
    Snapshot s = logbook_snapshot();
    bool known = logbook_time_known();

    head(req, "/log");

    if (known) {
        Record now = {};
        now.unix = logbook_now_unix();
        char t[24];
        logbook_format_time(now, t, sizeof(t));
        t[10] = ' ';
        snprintf(b, sizeof(b), "<p>Время на весах: %s</p>", t);
    } else {
        snprintf(b, sizeof(b), "<p class=m>Время на весах неизвестно — страница его сейчас установит.</p>");
    }
    httpd_resp_sendstr_chunk(req, b);

    if (!s.calibrated) {
        httpd_resp_sendstr_chunk(req, "<p><b>Весы не откалиброваны</b> — автомат не работает (команды tare и cal по USB).</p>");
    } else {
        snprintf(b, sizeof(b), "<p>На платформе: %.2f г — %s</p>", s.weight,
                 s.box_on ? "коробка стоит" : "коробки нет");
        httpd_resp_sendstr_chunk(req, b);
    }

    httpd_resp_sendstr_chunk(req, "<p>Выученные дозы: ");
    if (!s.kinds) httpd_resp_sendstr_chunk(req, "<span class=m>пока нет</span>");
    for (int i = 0; i < s.kinds && i < 4; i++) {
        snprintf(b, sizeof(b), "%sвид %d — %.3f г ×%u", i ? "; " : "", i + 1, s.kind_g[i], s.kind_n[i]);
        httpd_resp_sendstr_chunk(req, b);
    }
    snprintf(b, sizeof(b), "</p><p><a href='/log.csv'>Скачать журнал (CSV)</a> "
             "<span class=m>· записей %lu, всего записано %lu</span></p>",
             (unsigned long)logbook_count(), (unsigned long)logbook_total());
    httpd_resp_sendstr_chunk(req, b);

    httpd_resp_sendstr_chunk(req, "<table><tr><th>Время</th><th>Событие</th><th>Δ, г</th><th>Вес, г</th></tr>");
    int shown = 0;
    for (uint32_t i = 0; shown < PAGE_ROWS; i++) {
        Record r;
        if (!logbook_recent(i, r)) break;
        if (r.type == Journal::STATUS) continue;  // weight every 10 min — CSV only
        // label: Russian is 2 bytes a letter; the longest ("ДВОЙНАЯ … ПОВТОРНО …") is 93 bytes.
        // A cut mid-letter shows as "�", so keep a wide margin.
        char t[40], l[160], d[16] = "";
        when(r, t, sizeof(t));
        if (r.delta != 0) snprintf(d, sizeof(d), "%+.3f", r.delta);
        snprintf(b, sizeof(b), "<tr><td>%s</td><td>%s</td><td class=n>%s</td><td class=n>%.2f</td></tr>",
                 t, label(r, l, sizeof(l)), d, r.weight);
        httpd_resp_sendstr_chunk(req, b);
        shown++;
    }
    if (!shown) httpd_resp_sendstr_chunk(req, "<tr><td colspan=4 class=m>Журнал пуст</td></tr>");

    // Phone time goes to the scale if it does not know it; then reload the page.
    snprintf(b, sizeof(b),
             "</table><script>var known=%d;"
             "if(!known)fetch('/time?unix='+Math.floor(Date.now()/1000)+'&tz='+(-new Date().getTimezoneOffset()),"
             "{method:'POST'}).then(function(r){if(r.ok)location.reload()});"
             "setTimeout(function(){location.reload()},30000);</script></body></html>",
             known ? 1 : 0);
    httpd_resp_sendstr_chunk(req, b);
    return httpd_resp_sendstr_chunk(req, nullptr);
}

static esp_err_t csv(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=pillscale_log.csv");
    httpd_resp_sendstr_chunk(req, "seq,boot,uptime_s,unix,local_time,event,kind,off_s,delta_g,weight_g,slot,sched\n");
    char buf[1024];
    size_t len = 0;
    for (uint32_t i = logbook_count(); i-- > 0;) {  // oldest to newest
        Record r;
        if (!logbook_recent(i, r)) continue;
        char t[24];
        logbook_format_time(r, t, sizeof(t));
        if (len > sizeof(buf) - 160) { httpd_resp_send_chunk(req, buf, len); len = 0; }
        len += snprintf(buf + len, sizeof(buf) - len, "%lu,%lu,%lu,%lu,%s,%s,%u,%u,%.4f,%.4f,%u,%s\n",
                        (unsigned long)r.seq, (unsigned long)r.boot, (unsigned long)r.uptime_s,
                        (unsigned long)r.unix, t, Journal::name(r.type), Journal::dose_kind(r.kind), r.off_s,
                        r.delta, r.weight, Journal::slot(r.kind), Journal::sched_name(Journal::sched(r.kind)));
    }
    if (len) httpd_resp_send_chunk(req, buf, len);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t status(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    Snapshot s = logbook_snapshot();
    char b[1024];
    int n = snprintf(b, sizeof(b),
        "{\"time_known\":%s,\"unix\":%lu,\"tz\":%d,\"boot\":%lu,\"uptime_s\":%lu,"
        "\"calibrated\":%s,\"weight\":%.3f,\"box\":%.3f,\"box_on\":%s,"
        "\"log_count\":%lu,\"log_total\":%lu,\"doses\":[",
        logbook_time_known() ? "true" : "false", (unsigned long)logbook_now_unix(), logbook_tz(),
        (unsigned long)logbook_boot(), (unsigned long)logbook_uptime_s(),
        s.calibrated ? "true" : "false", s.weight, s.box, s.box_on ? "true" : "false",
        (unsigned long)logbook_count(), (unsigned long)logbook_total());
    for (int i = 0; i < s.kinds && i < 4; i++)
        n += snprintf(b + n, sizeof(b) - n, "%s{\"g\":%.4f,\"n\":%u}", i ? "," : "", s.kind_g[i], s.kind_n[i]);
    Schedule::State st = sched_state();
    n += snprintf(b + n, sizeof(b) - n, "],\"schedule\":{\"before\":%u,\"after\":%u,\"reminding\":%s,\"slots\":[",
                  st.before, st.after, sched_due() ? "true" : "false");
    static const char *names[] = {"upcoming", "due", "taken", "missed"};
    for (int i = 0; i < st.n; i++) {
        Schedule::View v = sched_view(i);
        char at[8] = "";
        if (v.state == Schedule::TAKEN)
            snprintf(at, sizeof(at), "%02d:%02d", Schedule::minute_of_day(v.at) / 60, Schedule::minute_of_day(v.at) % 60);
        n += snprintf(b + n, sizeof(b) - n, "%s{\"time\":\"%02u:%02u\",\"state\":\"%s\",\"at\":\"%s\"}",
                      i ? "," : "", v.minute / 60, v.minute % 60, names[v.state], at);
    }
    snprintf(b + n, sizeof(b) - n, "]}}");
    return httpd_resp_sendstr(req, b);
}

static esp_err_t set_time(httpd_req_t *req) {
    char q[96], v[24];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "unix", v, sizeof(v)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unix=...");
        return ESP_FAIL;
    }
    long long unix = strtoll(v, nullptr, 10);
    int tz = logbook_tz();
    if (httpd_query_key_value(q, "tz", v, sizeof(v)) == ESP_OK) tz = atoi(v);
    if (unix < 1700000000LL || tz < -720 || tz > 840) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad time");
        return ESP_FAIL;
    }
    // Already known and within a minute — do not multiply "time set" records.
    // NTP time is more accurate than the phone: then take only the time zone from the page.
    long long have = logbook_now_unix();
    if (wifi_ntp_synced()) {
        if (tz != logbook_tz()) logbook_set_time((uint32_t)have, tz);
    } else if (!logbook_time_known() || llabs(have - unix) > 60 || tz != logbook_tz()) {
        logbook_set_time((uint32_t)unix, tz);
    }
    return httpd_resp_sendstr(req, "OK");
}

// Percent-decoding of a form body (application/x-www-form-urlencoded):
// httpd_query_key_value extracts the value but does not expand %XX and +.
static void url_decode(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '+') { *w++ = ' '; }
        else if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = {r[1], r[2], 0};
            *w++ = (char)strtol(hex, nullptr, 16);
            r += 2;
        } else { *w++ = *r; }
    }
    *w = '\0';
}

// "Сброс" on the settings tab: three separate buttons, each asks first.
// q — the page's query string: done=wifi|log|doses after a reset.
static void reset_section(httpd_req_t *req, const char *q) {
    char b[640], ssid[33];
    bool has_net = wifi_sta_get(ssid, sizeof(ssid));
    Snapshot s = logbook_snapshot();
    uint32_t records = logbook_count();

    httpd_resp_sendstr_chunk(req, "<h2>Сброс</h2><style>.rs{display:flex;align-items:center;gap:12px;flex-wrap:wrap;"
        "margin:10px 0}.rs form{margin:0}.rs button{margin:0;color:#b00;border:1px solid #d33;background:#fff;"
        "border-radius:6px}.rs button:disabled{color:#999;border-color:#ccc}</style>");
    if (strstr(q, "done=wifi"))
        httpd_resp_sendstr_chunk(req, "<p class=ok>Домашняя сеть забыта. Применится после перезагрузки: весы поднимут "
                                      "свою сеть pillscale, страница — http://192.168.4.1</p>");
    if (strstr(q, "done=log")) httpd_resp_sendstr_chunk(req, "<p class=ok>Журнал очищен.</p>");
    if (strstr(q, "done=doses")) httpd_resp_sendstr_chunk(req, "<p class=ok>Выученные дозы забыты.</p>");

    // The question is in data-q: no ASCII quotes inside it, the attribute is single-quoted.
    snprintf(b, sizeof(b),
        "<div class=rs><form method=post action='/reset?what=wifi' data-q='Забыть домашнюю сеть %s? После "
        "перезагрузки весы поднимут свою сеть pillscale (пароль pillscale), страница будет на http://192.168.4.1' "
        "onsubmit='return confirm(this.dataset.q)'><button%s>Забыть домашнюю сеть</button></form>"
        "<span class=m>%s</span></div>",
        has_net ? ssid : "", has_net ? "" : " disabled", has_net ? ssid : "не задана");
    httpd_resp_sendstr_chunk(req, b);
    snprintf(b, sizeof(b),
        "<div class=rs><form method=post action='/reset?what=log' data-q='Удалить весь журнал: %lu записей? "
        "Это нельзя отменить. Сначала можно скачать CSV на вкладке Журнал.' "
        "onsubmit='return confirm(this.dataset.q)'><button%s>Очистить журнал</button></form>"
        "<span class=m>%lu записей · <a href='/log.csv'>скачать CSV</a></span></div>",
        (unsigned long)records, records ? "" : " disabled", (unsigned long)records);
    httpd_resp_sendstr_chunk(req, b);
    snprintf(b, sizeof(b),
        "<div class=rs><form method=post action='/reset?what=doses' data-q='Забыть выученные дозы (видов: %u)? "
        "Весы выучат новые по следующим приёмам. Расписание и журнал останутся.' "
        "onsubmit='return confirm(this.dataset.q)'><button%s>Забыть выученные дозы</button></form>"
        "<span class=m>видов: %u — нужно, когда сменили лекарства</span></div>",
        s.kinds, s.kinds ? "" : " disabled", s.kinds);
    httpd_resp_sendstr_chunk(req, b);
    httpd_resp_sendstr_chunk(req, "<p class=m>Перед демонстрацией удобно очистить журнал и забыть дозы. "
                                  "Калибровка здесь не сбрасывается — её делает сборщик по USB.</p>");
}

static esp_err_t settings_page(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    head(req, "/settings");
    char b[512], q[32] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) q[0] = '\0';
    bool saved = strstr(q, "saved") != nullptr;

    NetInfo n;
    wifi_info(n);
    const char *how = !strcmp(n.mode, "sta") ? "подключены к домашней сети"
                    : !strcmp(n.mode, "ap") ? "весы раздают свою сеть" : "сеть не поднята";
    snprintf(b, sizeof(b), "<h2>Сеть</h2><p>Сейчас: %s <b>%s</b>, адрес %s</p>", how, n.ssid, n.ip);
    httpd_resp_sendstr_chunk(req, b);

    if (saved) httpd_resp_sendstr_chunk(req, "<p class=ok>Сохранено. Применится после перезагрузки.</p>");

    char ssid[33];
    wifi_sta_get(ssid, sizeof(ssid));
    snprintf(b, sizeof(b),
             "<form method=post action='/settings'>"
             "<label>Домашняя сеть Wi-Fi</label><input type=text name=ssid maxlength=32 value='%s' placeholder='имя сети'>"
             "<label>Пароль</label><input type=password name=pass maxlength=63 placeholder='пусто — открытая сеть'>",
             ssid);
    httpd_resp_sendstr_chunk(req, b);
    httpd_resp_sendstr_chunk(req,
             "<p class=m>При включении весы 30 с пытаются подключиться к этой сети; не вышло — поднимают "
             "свою точку <b>pillscale</b>, как обычно. Адрес в домашней сети выдаёт роутер: его печатает "
             "команда <code>net</code> по USB. Чтобы забыть сеть — очистить имя и сохранить.</p>"
             "<button type=submit>Сохранить</button>"
             "<button type=button onclick=\"if(confirm('Перезагрузить весы?'))fetch('/reboot',{method:'POST'})"
             ".then(function(){document.getElementById('r').textContent='Перезагружаются… ~10 с'})\">"
             "Перезагрузить</button> <span id=r class=m></span></form>");
    reset_section(req, q);
    httpd_resp_sendstr_chunk(req, "</body></html>");
    return httpd_resp_sendstr_chunk(req, nullptr);
}

static esp_err_t settings_save(httpd_req_t *req) {
    char body[256];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[len] = '\0';
    char ssid[33 * 3] = "", pass[64 * 3] = "";  // room for %XX
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    url_decode(ssid);
    url_decode(pass);
    if (!wifi_sta_set(ssid, pass)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid<=32, pass<=63"); return ESP_FAIL; }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/settings?saved=1");
    return httpd_resp_sendstr(req, "");
}

// POST /reset?what=wifi|log|doses — the "Сброс" buttons (the page asked first).
static esp_err_t reset(httpd_req_t *req) {
    char q[32], what[8] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "what", what, sizeof(what));
    if (!strcmp(what, "wifi")) {
        if (!wifi_sta_set("", "")) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs"); return ESP_FAIL; }
    } else if (!strcmp(what, "log")) {
        // erases the whole partition: a couple of seconds, the main loop may wait on the mutex meanwhile
        if (!logbook_clear()) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash"); return ESP_FAIL; }
    } else if (!strcmp(what, "doses")) {
        logbook_request_doses_reset();  // the main loop owns the doses
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "what=wifi|log|doses");
        return ESP_FAIL;
    }
    char loc[40];
    snprintf(loc, sizeof(loc), "/settings?done=%s", what);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_sendstr(req, "");
}

static esp_err_t reboot(httpd_req_t *req) {
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(200));  // let the reply leave
    esp_restart();
    return ESP_OK;
}

// ---- "Schedule" tab ----

// Why a schedule is refused: windows overlap (slot k runs into the next one).
static void overlap_msg(const Schedule::State &s, int k, char *b, size_t n) {
    char need[24], t1[8], t2[8];
    span(s.before + s.after, need, sizeof(need));
    if (s.n == 1) {
        snprintf(b, n, "Окно %u мин до + %u мин после — это сутки и больше: приём перекрывается сам с собой "
                 "на следующий день. Уменьшите окно.", s.before, s.after);
        return;
    }
    hhmm(s.minute[k - 1], t1, sizeof(t1));
    hhmm(s.minute[k % s.n], t2, sizeof(t2));
    snprintf(b, n, "Окна приёма %s и %s перекрываются: между приёмами нужно больше %s (окно %u мин до + "
             "%u мин после). Сдвиньте время или уменьшите окно.", t1, t2, need, s.before, s.after);
}

// The live check in the browser: the same rule as Schedule::overlap(). Marks
// the clashing times red, shows why, and disables "Save". The server checks
// again (schedule_save), so this is for comfort, not for safety.
static const char *SCHEDULE_JS = R"JS(<script>
function $(i){return document.getElementById(i)}
function hm(m){m%=1440;return ('0'+(m/60|0)).slice(-2)+':'+('0'+m%60).slice(-2)}
function sp(m){return m<60?m+' мин':(m/60|0)+' ч'+(m%60?' '+('0'+m%60).slice(-2)+' мин':'')}
function chk(){var f=$('f'),ins=f.querySelectorAll('input[type=time]'),bf=+f.before.value||0,af=+f.after.value||0,
 need=bf+af,t=[],msg='',i;
 for(i=0;i<ins.length;i++){ins[i].style.outline='';var v=ins[i].value;
  if(v){var p=v.split(':');t.push({m:p[0]*60+(+p[1]),e:ins[i]})}}
 t.sort(function(a,b){return a.m-b.m});
 for(i=0;i<t.length&&!msg;i++){var a=t[i],b=t[(i+1)%t.length],g=(i+1<t.length?b.m:b.m+1440)-a.m;
  if(t.length>1&&g==0)msg='Время '+hm(a.m)+' указано дважды.';
  else if(t.length==1&&g<=need)msg='Окно '+bf+' мин до + '+af+' мин после — это сутки и больше. Уменьшите окно.';
  else if(g<=need)msg='Окна приёма '+hm(a.m)+' и '+hm(b.m)+' перекрываются: между приёмами нужно больше '+sp(need)+
   ' (окно '+bf+' мин до + '+af+' мин после). Сдвиньте время или уменьшите окно.';
  if(msg){a.e.style.outline='2px solid #d33';b.e.style.outline='2px solid #d33'}}
 var o=$('ovl');o.textContent=msg;o.style.display=msg?'':'none';$('save').disabled=!!msg}
function del(btn){var i=btn.parentNode.querySelector('input');
 if(!confirm('Удалить приём '+i.value+'?'))return;i.required=false;i.value='';$('f').submit()}
$('f').addEventListener('input',chk);chk();
</script>)JS";

// The "Schedule" tab. form — what the inputs show: the saved schedule, or the
// one just refused (so nothing typed is lost); error — why it was refused.
static esp_err_t schedule_render(httpd_req_t *req, const Schedule::State &form, const char *error, bool saved) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    head(req, "/schedule");
    char b[512];
    Schedule::State st = sched_state();

    httpd_resp_sendstr_chunk(req, "<h2>Приёмы</h2>");
    if (!logbook_time_known())
        httpd_resp_sendstr_chunk(req, "<p class=m>Время на весах неизвестно: расписание ждёт — откройте вкладку «Сводка», она его установит.</p>");
    if (saved) httpd_resp_sendstr_chunk(req, "<p class=ok>Сохранено.</p>");

    if (st.n) {
        httpd_resp_sendstr_chunk(req, "<table><tr><th>Время</th><th>Сегодня</th></tr>");
        for (int i = 0; i < st.n; i++) {
            Schedule::View v = sched_view(i);
            const char *what = "<span class=m>ждём</span>";
            char at[64] = "";
            switch (v.state) {
            case Schedule::TAKEN:
                snprintf(at, sizeof(at), "<span class=ok>✓ принято в %02d:%02d</span>",
                         Schedule::minute_of_day(v.at) / 60, Schedule::minute_of_day(v.at) % 60);
                what = at;
                break;
            case Schedule::DUE:       what = "<b>⏰ пора</b>"; break;
            case Schedule::MISSED_ST: what = "<b>✗ пропущен</b>"; break;
            default: break;
            }
            snprintf(b, sizeof(b), "<tr><td><b>%02u:%02u</b></td><td>%s</td></tr>", v.minute / 60, v.minute % 60, what);
            httpd_resp_sendstr_chunk(req, b);
        }
        httpd_resp_sendstr_chunk(req, "</table>");
    } else {
        httpd_resp_sendstr_chunk(req, "<p class=m>Расписание не задано: дозы записываются, напоминаний нет.</p>");
    }

    // One dose time per row. A filled <input type=time> cannot be cleared on a
    // phone (iOS has no clear button), so each row has its own remove button:
    // it empties the field and saves the form at once. One empty row below
    // the list adds a new time.
    httpd_resp_sendstr_chunk(req, "<form method=post action='/schedule' id=f><h2>Изменить</h2>"
        "<style>input[type=time],input[type=number]{font-size:16px;padding:6px;margin:4px 8px 4px 0}"
        "input[type=time]{width:140px}input[type=number]{width:64px}"
        ".slot{display:flex;align-items:center;gap:6px;margin:4px 0}"
        "button.del{width:44px;height:44px;padding:0;margin:0;border:0;background:none;cursor:pointer;line-height:0}"
        "button:disabled{opacity:.45}</style><p>Моменты приёма (до четырёх):</p>");
    // red crossed-out circle
    static const char *del_icon =
        "<svg width=28 height=28 viewBox='0 0 24 24' aria-hidden=true><circle cx=12 cy=12 r=10 fill=none "
        "stroke='#d33' stroke-width=2.4 /><line x1=5 y1=19 x2=19 y2=5 stroke='#d33' stroke-width=2.4 "
        "stroke-linecap=round /></svg>";
    for (int i = 0; i < form.n; i++) {
        snprintf(b, sizeof(b), "<div class=slot><input type=time name=t%d value='%02u:%02u' required>"
                 "<button type=button class=del title='Удалить' aria-label='Удалить %02u:%02u' onclick=del(this)>",
                 i + 1, form.minute[i] / 60, form.minute[i] % 60, form.minute[i] / 60, form.minute[i] % 60);
        httpd_resp_sendstr_chunk(req, b);
        httpd_resp_sendstr_chunk(req, del_icon);
        httpd_resp_sendstr_chunk(req, "</button></div>");
    }
    if (form.n < Schedule::MAX_SLOTS) {
        snprintf(b, sizeof(b), "<div class=slot><input type=time name=t%d> <span class=m>%s</span></div>",
                 form.n + 1, form.n ? "— добавить ещё" : "— добавить первый");
        httpd_resp_sendstr_chunk(req, b);
    }
    snprintf(b, sizeof(b), "<p>Окно: за <input type=number name=before min=0 max=720 value=%u> мин до "
             "и <input type=number name=after min=0 max=720 value=%u> мин после</p>", form.before, form.after);
    httpd_resp_sendstr_chunk(req, b);
    // the warning: filled by the server after a refused save, then kept up to date by chk()
    httpd_resp_sendstr_chunk(req, "<p id=ovl class='card alarm' role=alert");
    httpd_resp_sendstr_chunk(req, error ? ">" : " style='display:none'>");
    if (error) httpd_resp_sendstr_chunk(req, error);
    httpd_resp_sendstr_chunk(req, "</p>"
        "<p class=m>Доза, вынутая в окне, — приём по расписанию; вторая в том же окне — повторная; "
        "вне окон — не по расписанию. Окна разных приёмов не должны перекрываться: между приёмами — больше, "
        "чем окно до плюс окно после. В момент приёма весы пищат и повторяют каждые 5 минут, пока дозу не вынут "
        "или окно не закроется; окно закрылось без дозы — в журнале «пропущен». Сняли коробку — сигнал откладывается.</p>"
        "<button type=submit id=save>Сохранить</button></form>");
    httpd_resp_sendstr_chunk(req, SCHEDULE_JS);
    httpd_resp_sendstr_chunk(req, "</body></html>");
    return httpd_resp_sendstr_chunk(req, nullptr);
}

static esp_err_t schedule_page(httpd_req_t *req) {
    char q[32];
    bool saved = httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && strstr(q, "saved");
    return schedule_render(req, sched_state(), nullptr, saved);
}

static esp_err_t schedule_save(httpd_req_t *req) {
    char body[256];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[len] = '\0';
    Schedule::State st = {};  // sorted, without duplicates — as Schedule keeps it
    char v[16];
    for (int i = 0; i < Schedule::MAX_SLOTS; i++) {
        char key[4] = {'t', (char)('1' + i), 0};
        if (httpd_query_key_value(body, key, v, sizeof(v)) != ESP_OK) continue;
        url_decode(v);  // "08%3A00" → "08:00"
        int h, m;
        if (sscanf(v, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) continue;
        Schedule::insert(st, (uint16_t)(h * 60 + m));
    }
    int before = 30, after = 60;
    if (httpd_query_key_value(body, "before", v, sizeof(v)) == ESP_OK) before = atoi(v);
    if (httpd_query_key_value(body, "after", v, sizeof(v)) == ESP_OK) after = atoi(v);
    char msg[400];
    if (before < 0 || before > 720 || after < 0 || after > 720) {
        st.before = 30;
        st.after = 60;
        snprintf(msg, sizeof(msg), "Окно — от 0 до 720 минут до и после приёма.");
        return schedule_render(req, st, msg, false);
    }
    st.before = (uint16_t)before;
    st.after = (uint16_t)after;
    if (int k = Schedule::overlap(st)) {  // refused: show the form again as typed, with the reason
        overlap_msg(st, k, msg, sizeof(msg));
        return schedule_render(req, st, msg, false);
    }
    if (!sched_set(st)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "schedule"); return ESP_FAIL; }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/schedule?saved=1");
    return httpd_resp_sendstr(req, "");
}

// ---- "Scale" tab: an ordinary scale on the phone ----

// Tab tare: display only. Does not touch calibration (the tare command), the
// tracker or the journal — otherwise with the box on the scale the tracker
// would see a false "removed". Lives in RAM, 0 after reboot. Only the web
// server task reads and writes it.
static float tare_g = 0;

static esp_err_t live_json(httpd_req_t *req) {
    Live l = logbook_live();
    char b[256];
    snprintf(b, sizeof(b),
             "{\"calibrated\":%s,\"raw\":%ld,\"offset\":%ld,\"scale\":%.2f,\"g\":%.3f,\"tare\":%.3f,"
             "\"net\":%.3f,\"sd\":%.4f,\"stable\":%s,\"n\":%u}",
             l.calibrated ? "true" : "false", (long)l.raw, (long)l.offset, l.scale, l.g, tare_g,
             l.g - tare_g, l.sd, l.n >= 4 && l.sd < 0.03f ? "true" : "false", l.n);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, b);
}

static esp_err_t tare(httpd_req_t *req) {
    Live l = logbook_live();
    if (!l.calibrated || l.n == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not calibrated");
        return ESP_FAIL;
    }
    tare_g = l.g;
    char b[32];
    snprintf(b, sizeof(b), "OK tare=%.3f", tare_g);
    return httpd_resp_sendstr(req, b);
}

static esp_err_t scale_page(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    head(req, "/scale");
    httpd_resp_sendstr_chunk(req,
        "<style>.big{font-size:72px;font-weight:600;font-variant-numeric:tabular-nums;margin:28px 0 4px;"
        "white-space:nowrap}.big small{font-size:32px;font-weight:400}"
        ".tare{font-size:22px;padding:14px 40px;margin:16px 0 8px}"
        ".raw{font-size:12px;color:#666;font-family:ui-monospace,monospace;margin-top:28px;line-height:1.5;"
        "overflow-wrap:anywhere}</style>"
        "<div id=w class=big>—</div><div id=st class=m>&nbsp;</div>"
        "<button class=tare onclick=tare()>Тара</button> <span id=t class=m></span>"
        "<div id=raw class=raw></div>"
        "<p class=m style='font-size:12px'>Тара — только на экране: калибровку, автомат и журнал не меняет. "
        "Дрейф здесь не вычитается: при смене температуры показание может немного уплыть.</p>"
        "<script>"
        "var busy=0;function $(i){return document.getElementById(i)}"
        "function f(x,k){return x.toFixed(k).replace('.',',')}"
        "function upd(){if(busy)return;busy=1;"
        "fetch('/live.json').then(function(r){return r.json()}).then(function(d){busy=0;"
        "if(!d.calibrated){$('w').textContent='—';$('st').textContent='Весы не откалиброваны: tare и cal по USB';$('t').textContent='';}"
        "else{var v=d.net;if(Math.abs(v)<0.005)v=0;$('w').innerHTML=f(v,2)+' <small>г</small>';"
        "$('st').textContent=d.stable?'● стабильно':'○ вес меняется';$('st').className=d.stable?'ok':'m';"
        "$('t').textContent=Math.abs(d.tare)>0.0005?'тара '+f(d.tare,2)+' г, брутто '+f(d.g,2)+' г':'';}"
        "var one=d.calibrated?' = '+f((d.raw-d.offset)/d.scale,3)+' г':'';"
        "$('raw').innerHTML='АЦП, последний отсчёт: <b>'+d.raw+'</b><br>'"
        "+'ноль калибровки: '+d.offset+' · отсчётов на грамм: '+f(d.scale,2)+'<br>'"
        "+'г = (отсчёт − ноль) / отсчётов на грамм = ('+d.raw+' − ('+d.offset+')) / '+f(d.scale,2)+one+'<br>'"
        "+'на экране — среднее '+d.n+' отсчётов, разброс '+f(d.sd*1000,1)+' мг';"
        "}).catch(function(){busy=0;$('st').textContent='нет связи с весами';$('st').className='m'})}"
        "function tare(){fetch('/tare',{method:'POST'}).then(upd)}"
        "setInterval(upd,250);upd();"
        "</script></body></html>");
    return httpd_resp_sendstr_chunk(req, nullptr);
}

static bool running = false;
bool web_running() { return running; }

bool web_begin() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 16;  // default is 8 — extra URLs are silently not registered

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &cfg) != ESP_OK) return false;
    const httpd_uri_t uris[] = {
        {"/", HTTP_GET, summary_page, nullptr},
        {"/log", HTTP_GET, log_page, nullptr},
        {"/log.csv", HTTP_GET, csv, nullptr},
        {"/status.json", HTTP_GET, status, nullptr},
        {"/time", HTTP_POST, set_time, nullptr},
        {"/scale", HTTP_GET, scale_page, nullptr},
        {"/live.json", HTTP_GET, live_json, nullptr},
        {"/tare", HTTP_POST, tare, nullptr},
        {"/settings", HTTP_GET, settings_page, nullptr},
        {"/settings", HTTP_POST, settings_save, nullptr},
        {"/schedule", HTTP_GET, schedule_page, nullptr},
        {"/schedule", HTTP_POST, schedule_save, nullptr},
        {"/reboot", HTTP_POST, reboot, nullptr},
        {"/reset", HTTP_POST, reset, nullptr},
    };
    for (const httpd_uri_t &u : uris) httpd_register_uri_handler(server, &u);
    running = true;
    return true;
}
