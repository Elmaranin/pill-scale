// Web page on the scale: state, journal, CSV, time from the browser.
// URLs: docs/api.md, "Веб-страница". Runs in the web server's own task; takes
// data from logbook (mutex inside), measures nothing itself.
// UI strings are Russian on purpose: the page is what the user sees.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "esp_http_server.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board/wifi.h"
#include "logbook.h"
#include "web.h"

static const int PAGE_ROWS = 100;  // how many recent events on the page

static const char *label(const Record &r, char *buf, size_t n) {
    switch (r.type) {
    case Journal::BOOT:    return "включение";
    case Journal::TIMESET: return "время установлено";
    case Journal::PLACED:  return "поставили коробку";
    case Journal::REMOVED: return "сняли коробку";
    case Journal::MISSING: return "коробки давно нет";
    case Journal::NONE:    return "ничего не взяли";
    case Journal::DOSE:    snprintf(buf, n, "доза (вид %u)", r.kind); return buf;
    case Journal::DOUBLE:  snprintf(buf, n, "<b>ДВОЙНАЯ доза</b> (вид %u)", r.kind); return buf;
    case Journal::REFILL:  return "пополнение";
    case Journal::NEWBOX:  return "другая коробка";
    case Journal::UNSURE:  return "не понял";
    case Journal::STATUS:  return "вес";
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
    char b[384];
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html lang=ru><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'><title>pillscale</title>"
        "<style>body{font-family:system-ui,sans-serif;margin:16px;max-width:760px;color:#111;background:#fff}"
        "table{border-collapse:collapse;width:100%}td,th{padding:5px 6px;border-bottom:1px solid #ddd;"
        "text-align:left;font-size:14px}td.n{text-align:right;font-variant-numeric:tabular-nums}"
        ".m{color:#555}h1{font-size:22px;display:inline-block;margin:0 16px 12px 0}"
        ".nav a{margin-right:12px}.nav a.on{font-weight:bold;text-decoration:none;color:#111}"
        "label{display:block;margin:10px 0 4px}input[type=text],input[type=password]{font-size:16px;"
        "padding:6px;width:100%;max-width:320px;box-sizing:border-box}"
        "button{font-size:16px;padding:8px 14px;margin:12px 8px 0 0}.ok{color:#060}</style></head><body>"
        "<h1>pillscale</h1>");
    snprintf(b, sizeof(b),
             "<span class=nav><a href='/'%s>Журнал</a><a href='/scale'%s>Весы</a><a href='/settings'%s>Настройки</a></span>",
             active[0] == '\0' || !strcmp(active, "/") ? " class=on" : "",
             !strcmp(active, "/scale") ? " class=on" : "",
             !strcmp(active, "/settings") ? " class=on" : "");
    httpd_resp_sendstr_chunk(req, b);
}

static esp_err_t page(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char b[512];
    Snapshot s = logbook_snapshot();
    bool known = logbook_time_known();

    head(req, "");

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
        char t[40], l[64], d[16] = "";
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
    httpd_resp_sendstr_chunk(req, "seq,boot,uptime_s,unix,local_time,event,kind,off_s,delta_g,weight_g\n");
    char buf[1024];
    size_t len = 0;
    for (uint32_t i = logbook_count(); i-- > 0;) {  // oldest to newest
        Record r;
        if (!logbook_recent(i, r)) continue;
        char t[24];
        logbook_format_time(r, t, sizeof(t));
        if (len > sizeof(buf) - 160) { httpd_resp_send_chunk(req, buf, len); len = 0; }
        len += snprintf(buf + len, sizeof(buf) - len, "%lu,%lu,%lu,%lu,%s,%s,%u,%u,%.4f,%.4f\n",
                        (unsigned long)r.seq, (unsigned long)r.boot, (unsigned long)r.uptime_s,
                        (unsigned long)r.unix, t, Journal::name(r.type), r.kind, r.off_s,
                        r.delta, r.weight);
    }
    if (len) httpd_resp_send_chunk(req, buf, len);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t status(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    Snapshot s = logbook_snapshot();
    char b[640];
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
    snprintf(b + n, sizeof(b) - n, "]}");
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

static esp_err_t settings_page(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    head(req, "/settings");
    char b[512], q[32];
    bool saved = httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && strstr(q, "saved");

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

static esp_err_t reboot(httpd_req_t *req) {
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(200));  // let the reply leave
    esp_restart();
    return ESP_OK;
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
        {"/", HTTP_GET, page, nullptr},
        {"/log.csv", HTTP_GET, csv, nullptr},
        {"/status.json", HTTP_GET, status, nullptr},
        {"/time", HTTP_POST, set_time, nullptr},
        {"/scale", HTTP_GET, scale_page, nullptr},
        {"/live.json", HTTP_GET, live_json, nullptr},
        {"/tare", HTTP_POST, tare, nullptr},
        {"/settings", HTTP_GET, settings_page, nullptr},
        {"/settings", HTTP_POST, settings_save, nullptr},
        {"/reboot", HTTP_POST, reboot, nullptr},
    };
    for (const httpd_uri_t &u : uris) httpd_register_uri_handler(server, &u);
    running = true;
    return true;
}
