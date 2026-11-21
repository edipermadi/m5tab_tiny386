/*
 * The clock: at boot, Wi-Fi (through the on-board ESP32-C6, ESP-Hosted over
 * SDIO) only long enough to get the time over NTP, then the C6 is switched
 * off again. Plus the timezone. Settings come from the console
 * (tools/tab5ctl.py, through sdfs_serial.c) and live in NVS:
 *
 *   @fs wifi <ssid> [password]   save (used from the next boot)
 *   @fs wifi-forget              remove them (no Wi-Fi from the next boot)
 *   @fs tz [Area/City]           set the timezone (IANA name, applied at once),
 *                                or show it
 *   @fs net                      status
 *   @fs reboot
 *
 * net_init() runs early in boot, before the SD card is mounted (the C6 link
 * uses slot 1 of the same SDMMC controller) and before USB and the display
 * take their share of the scarce internal RAM: connect, wait for the time
 * (CONFIG_TAB5_TIME_WAIT_S at most), then SNTP, Wi-Fi and ESP-Hosted are
 * shut down and the C6's supply cut. The emulator starts after that. The
 * guest's CMOS clock is local time in the saved zone.
 */
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_hosted.h"
#include "esp_hosted_event.h"
#include "esp_timer.h"
#include "bsp/bsp_board.h"

#include "tab5.h"
#include "tz_db.h"

static const char *TAG = "net";

#define NVS_NS            "tab5net"
#define HOSTED_TIMEOUT_MS 5000

#define BIT_HOSTED_UP     BIT0
#define BIT_GOT_IP        BIT1
#define BIT_TIME_SET      BIT2

static EventGroupHandle_t s_ev;
static char s_ssid[33], s_pass[65], s_tz[64];
static char s_ip[16];
static int s_disconnect_reason;
static char s_result[80];           /* how the boot-time sync went */

/* ------------------------------------------------------------- settings */

static void nvs_get(nvs_handle_t h, const char *key, char *buf, size_t size)
{
    size_t len = size;
    if (nvs_get_str(h, key, buf, &len) != ESP_OK) {
        buf[0] = 0;
    }
}

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;                     /* nothing saved yet */
    }
    nvs_get(h, "ssid", s_ssid, sizeof(s_ssid));
    nvs_get(h, "pass", s_pass, sizeof(s_pass));
    nvs_get(h, "tz", s_tz, sizeof(s_tz));
    nvs_close(h);
}

/* key = NULL value: erase it */
static esp_err_t settings_store(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        return e;
    }
    if (value) {
        e = nvs_set_str(h, key, value);
    } else {
        e = nvs_erase_key(h, key);
        if (e == ESP_ERR_NVS_NOT_FOUND) {
            e = ESP_OK;
        }
    }
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e;
}

static const char *tz_lookup(const char *name)
{
    int lo = 0, hi = sizeof(tz_db) / sizeof(tz_db[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(name, tz_db[mid].name);
        if (c == 0) {
            return tz_db[mid].posix;
        }
        if (c < 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return NULL;
}

/* localtime() (and with it the guest's CMOS clock) follows TZ */
static void tz_apply(void)
{
    const char *posix = s_tz[0] ? tz_lookup(s_tz) : NULL;
    setenv("TZ", posix ? posix : "UTC0", 1);
    tzset();
}

/* ------------------------------------------------------------------ Wi-Fi */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == ESP_HOSTED_EVENT && id == ESP_HOSTED_EVENT_TRANSPORT_UP) {
        xEventGroupSetBits(s_ev, BIT_HOSTED_UP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        if (d->reason != s_disconnect_reason) {
            ESP_LOGW(TAG, "Wi-Fi \"%s\": disconnected (reason %d), retrying", s_ssid, d->reason);
        }
        s_disconnect_reason = d->reason;
        xEventGroupClearBits(s_ev, BIT_GOT_IP);
        s_ip[0] = 0;
        esp_wifi_connect();         /* the C6 paces the retries */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_disconnect_reason = 0;
        ESP_LOGI(TAG, "Wi-Fi \"%s\": %s", s_ssid, s_ip);
        /* SNTP from here: a request sent before there is an address fails,
           and lwIP's retry back-off would then outlast the boot wait */
        if (!(xEventGroupGetBits(s_ev) & BIT_GOT_IP)) {
            esp_netif_sntp_start();
        }
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

static void on_time(struct timeval *tv)
{
    time_t now = tv->tv_sec;
    struct tm tm;
    char buf[32];
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    ESP_LOGI(TAG, "time set: %s %s", buf, s_tz[0] ? s_tz : "UTC");
    xEventGroupSetBits(s_ev, BIT_TIME_SET);
}

static void wifi_set_credentials(void)
{
    wifi_config_t wc = { 0 };
    memcpy(wc.sta.ssid, s_ssid, strlen(s_ssid));
    memcpy(wc.sta.password, s_pass, strlen(s_pass));
    wc.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
}

static esp_netif_t *s_sta;

static bool wifi_start(void)
{
    /* the C6 link */
    esp_event_handler_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    if (esp_hosted_init() != 0 || esp_hosted_connect_to_slave() != 0) {
        ESP_LOGE(TAG, "ESP-Hosted init failed");
        return false;
    }
    if (!(xEventGroupWaitBits(s_ev, BIT_HOSTED_UP, pdFALSE, pdFALSE,
                              pdMS_TO_TICKS(HOSTED_TIMEOUT_MS)) & BIT_HOSTED_UP)) {
        ESP_LOGE(TAG, "ESP32-C6 not responding (no esp-hosted firmware on it?)");
        return false;
    }
    esp_hosted_coprocessor_fwver_t v;
    if (esp_hosted_get_coprocessor_fwversion(&v) == 0) {
        ESP_LOGI(TAG, "ESP32-C6 runs esp-hosted %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 v.major1, v.minor1, v.patch1);
    }

    s_sta = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed");
        return false;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);     /* ours are in NVS_NS */
    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_set_credentials();
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed");
        return false;
    }

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_TAB5_NTP_SERVER);
    sntp.sync_cb = on_time;
    sntp.start = false;             /* on IP_EVENT_STA_GOT_IP */
    esp_netif_sntp_init(&sntp);
    return true;
}

/* Undo wifi_start() as far as it got. lwIP itself stays (it cannot be
   deinitialised), idle. */
static void wifi_stop(void)
{
    esp_netif_sntp_deinit();
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event);
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_sta) {
        esp_netif_destroy_default_wifi(s_sta);
        s_sta = NULL;
    }
    esp_hosted_deinit();
    esp_event_handler_unregister(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, on_event);
}

static void net_sync(void);

void net_init(void)
{
    net_sync();
    bsp_c6_power(false);            /* nothing else uses the C6 */
}

static void net_sync(void)
{
    s_ev = xEventGroupCreate();
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS: %s", esp_err_to_name(e));
    }
    settings_load();
    tz_apply();
    if (s_tz[0] && !tz_lookup(s_tz)) {
        ESP_LOGW(TAG, "unknown timezone \"%s\", using UTC", s_tz);
    }

    if (!s_ssid[0]) {
        /* no lwIP either: its task and buffers take internal RAM */
        ESP_LOGI(TAG, "no Wi-Fi network saved (tools/tab5ctl.py wifi <ssid> <password>)");
        return;
    }
    esp_netif_init();
    esp_event_loop_create_default();
    ESP_LOGI(TAG, "Wi-Fi \"%s\": getting the time (up to %d s)", s_ssid, CONFIG_TAB5_TIME_WAIT_S);
    int64_t t0 = esp_timer_get_time();
    if (wifi_start()) {
        bool ok = xEventGroupWaitBits(s_ev, BIT_TIME_SET, pdFALSE, pdFALSE,
                                      pdMS_TO_TICKS(CONFIG_TAB5_TIME_WAIT_S * 1000)) & BIT_TIME_SET;
        int secs = (int)((esp_timer_get_time() - t0) / 1000000);
        if (ok) {
            snprintf(s_result, sizeof(s_result), "synced at boot in %d s", secs);
        } else if (xEventGroupGetBits(s_ev) & BIT_GOT_IP) {
            snprintf(s_result, sizeof(s_result), "no reply from %s", CONFIG_TAB5_NTP_SERVER);
        } else if (s_disconnect_reason) {
            snprintf(s_result, sizeof(s_result), "Wi-Fi did not connect (reason %d)", s_disconnect_reason);
        } else {
            snprintf(s_result, sizeof(s_result), "Wi-Fi did not connect");
        }
    } else {
        snprintf(s_result, sizeof(s_result), "no Wi-Fi: ESP32-C6 link failed");
    }
    if (!(xEventGroupGetBits(s_ev) & BIT_TIME_SET)) {
        ESP_LOGW(TAG, "no network time: %s", s_result);
    }
    wifi_stop();
    ESP_LOGI(TAG, "Wi-Fi off");
}

/* --------------------------------------------------------------- commands */

static void cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || argc > 3 || strlen(argv[1]) > 32 ||
        (argc == 3 && (strlen(argv[2]) > 64 || (argv[2][0] && strlen(argv[2]) < 8)))) {
        sdfs_reply("err usage: wifi <ssid> [password] (SSID up to 32 chars, password 8-64)");
        return;
    }
    const char *pass = argc == 3 ? argv[2] : "";
    esp_err_t e = settings_store("ssid", argv[1]);
    if (e == ESP_OK) {
        e = settings_store("pass", pass);
    }
    if (e != ESP_OK) {
        sdfs_reply("err NVS: %s", esp_err_to_name(e));
        return;
    }
    snprintf(s_ssid, sizeof(s_ssid), "%s", argv[1]);
    snprintf(s_pass, sizeof(s_pass), "%s", pass);
    sdfs_reply("ok saved; used at the next boot to set the clock (tab5ctl.py reboot)");
}

static void cmd_wifi_forget(void)
{
    esp_err_t e = settings_store("ssid", NULL);
    if (e == ESP_OK) {
        e = settings_store("pass", NULL);
    }
    if (e != ESP_OK) {
        sdfs_reply("err NVS: %s", esp_err_to_name(e));
        return;
    }
    s_ssid[0] = s_pass[0] = 0;
    sdfs_reply("ok Wi-Fi settings removed");
}

static void cmd_tz(int argc, char **argv)
{
    if (argc == 1) {
        sdfs_reply("ok %s", s_tz[0] ? s_tz : "UTC (not set)");
        return;
    }
    const char *posix = tz_lookup(argv[1]);
    if (!posix || strlen(argv[1]) >= sizeof(s_tz)) {
        sdfs_reply("err unknown timezone \"%s\" (an IANA name, like Asia/Dubai)", argv[1]);
        return;
    }
    esp_err_t e = settings_store("tz", argv[1]);
    if (e != ESP_OK) {
        sdfs_reply("err NVS: %s", esp_err_to_name(e));
        return;
    }
    snprintf(s_tz, sizeof(s_tz), "%s", argv[1]);
    tz_apply();
    sdfs_reply("ok %s (%s)", s_tz, posix);
}

static void cmd_net(void)
{
    time_t now = time(NULL);
    struct tm tm;
    char buf[32];
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    EventBits_t bits = xEventGroupGetBits(s_ev);

    sdfs_reply("i wifi: %s", s_ssid[0] ? s_ssid : "(not set)");
    if (s_result[0]) {
        sdfs_reply("i at boot: %s%s%s", s_result, s_ip[0] ? ", IP was " : "", s_ip);
    }
    sdfs_reply("i timezone: %s", s_tz[0] ? s_tz : "UTC (not set)");
    sdfs_reply("i time: %s%s", buf, (bits & BIT_TIME_SET) ? "" : " (not synced)");
    sdfs_reply("ok");
}

bool net_command(int argc, char **argv)
{
    if (strcmp(argv[0], "wifi") == 0) {
        cmd_wifi(argc, argv);
    } else if (strcmp(argv[0], "wifi-forget") == 0) {
        cmd_wifi_forget();
    } else if (strcmp(argv[0], "tz") == 0) {
        cmd_tz(argc, argv);
    } else if (strcmp(argv[0], "net") == 0) {
        cmd_net();
    } else if (strcmp(argv[0], "reboot") == 0) {
        sdfs_reply("ok rebooting");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else {
        return false;
    }
    return true;
}
