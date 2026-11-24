#include "wifi.h"
#include "settings.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_hosted.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_events;
#define BIT_GOT_IP  BIT0
#define BIT_FAILED  BIT1

static char s_ip[16] = "0.0.0.0";
static esp_netif_t *s_netif_sta;
static esp_netif_t *s_netif_ap;
static bool s_ready;
static bool s_attempting;
static int  s_retries;

// Backoff rather than a tight reconnect loop: a router that is rebooting
// should not be hammered, and a wrong password should not spin forever.
static const int RETRY_MAX = 8;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;

    // Association events for the setup AP. Without these, "my computer will
    // not load the page" cannot be told apart from "my computer never joined".
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "station joined the setup network: %02x:%02x:%02x:%02x:%02x:%02x (aid %d)",
                 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->aid);
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGW(TAG, "station left the setup network: %02x:%02x:%02x:%02x:%02x:%02x",
                 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ready = false;
        strcpy(s_ip, "0.0.0.0");
        if (s_retries < RETRY_MAX) {
            s_retries++;
            ESP_LOGW(TAG, "disconnected, retry %d/%d", s_retries, RETRY_MAX);
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "giving up after %d attempts -- check SSID and password", RETRY_MAX);
            s_attempting = false;
            xEventGroupSetBits(s_events, BIT_FAILED);
        }
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        s_ready = true;
        s_attempting = false;
        ESP_LOGI(TAG, "connected, IP %s", s_ip);

        // DHCP hands out DNS servers along with the address, but over the
        // esp_hosted RPC link that is worth verifying rather than assuming:
        // without a resolver every fetch fails at getaddrinfo() with an error
        // that looks nothing like a networking problem.
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(s_netif_sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
            dns.ip.u_addr.ip4.addr != 0) {
            ESP_LOGI(TAG, "DNS " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        } else {
            // Fall back to a public resolver rather than sitting there unable
            // to resolve anything. Better a working frame than a correct one.
            ESP_LOGW(TAG, "no DNS server from DHCP -- falling back to 1.1.1.1");
            esp_netif_dns_info_t f = {0};
            f.ip.type = ESP_IPADDR_TYPE_V4;
            IP4_ADDR(&f.ip.u_addr.ip4, 1, 1, 1, 1);
            esp_netif_set_dns_info(s_netif_sta, ESP_NETIF_DNS_MAIN, &f);
        }
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

esp_err_t wifi_begin(void)
{
    s_events = xEventGroupCreate();

    // Bring up the co-processor link first. Everything else depends on it, and
    // its failure mode is completely different from a bad password.
    ESP_LOGI(TAG, "starting ESP32-C6 link over SDIO slot %d...", CONFIG_ESP_HOSTED_HOST_SDIO_SLOT);
    esp_err_t err = esp_hosted_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_init: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "  the C6 did not answer. This is the co-processor link,");
        ESP_LOGE(TAG, "  not Wi-Fi: check the SDIO pins and the C6's own firmware.");
        return err;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    // BOTH interfaces are created here, before esp_wifi_start().
    //
    // Creating the AP netif later -- only when the setup portal is needed --
    // looks tidier and does not work: the netif glue wires up its DHCP server
    // during interface start, so an AP created after the fact comes up with
    // no DHCP at all. The access point appears, clients associate, and then
    // sit there without an address forever.
    s_netif_sta = esp_netif_create_default_wifi_sta();
    s_netif_ap  = esp_netif_create_default_wifi_ap();
    if (!s_netif_sta || !s_netif_ap) {
        ESP_LOGE(TAG, "could not create netifs");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL), TAG, "wifi evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL, NULL), TAG, "ip evt");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    ESP_LOGI(TAG, "C6 link up");
    return ESP_OK;
}

esp_err_t wifi_scan(void)
{
    // No credentials involved, so this separates "the radio works" from
    // "the password is right" -- two failures that otherwise look alike.
    ESP_LOGI(TAG, "scanning...");
    wifi_scan_config_t cfg = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) { ESP_LOGE(TAG, "scan: %s", esp_err_to_name(err)); return err; }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) { ESP_LOGW(TAG, "no networks found"); return ESP_OK; }

    uint16_t want = n > 20 ? 20 : n;
    wifi_ap_record_t *aps = calloc(want, sizeof(wifi_ap_record_t));
    if (!aps) return ESP_ERR_NO_MEM;
    esp_wifi_scan_get_ap_records(&want, aps);

    ESP_LOGI(TAG, "%u network(s):", n);
    for (int i = 0; i < want; i++) {
        // The 2.4/5 GHz split matters: the C6 is dual-band capable, but a
        // network the scan cannot see is the commonest "wrong password".
        ESP_LOGI(TAG, "  %-32s ch%-3d %4d dBm%s",
                 (char *)aps[i].ssid, aps[i].primary, aps[i].rssi,
                 aps[i].authmode == WIFI_AUTH_OPEN ? "  (open)" : "");
    }
    free(aps);
    return ESP_OK;
}

esp_err_t wifi_connect(void)
{
    if (g_settings.wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "no SSID configured -- staying offline");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid,     g_settings.wifi_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, g_settings.wifi_pass, sizeof(wc.sta.password) - 1);

    ESP_LOGI(TAG, "connecting to \"%s\"...", g_settings.wifi_ssid);
    s_retries = 0;
    s_attempting = true;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "config");
    return esp_wifi_connect();
}

esp_netif_t *wifi_netif_ap(void)  { return s_netif_ap; }
esp_netif_t *wifi_netif_sta(void) { return s_netif_sta; }

bool wifi_ready(void)      { return s_ready; }
bool wifi_connecting(void) { return s_attempting && !s_ready; }
const char *wifi_ip(void)  { return s_ip; }

int wifi_rssi(void)
{
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &t);
    ESP_LOGI(TAG, "time synchronised: %s", buf);
}

void wifi_start_sntp(void)
{
    if (esp_sntp_enabled()) return;
    // The TZ is a POSIX string, not an IANA name: the ESP has no timezone
    // database, so DST rules have to be spelled out. Same as the CYD project.
    setenv("TZ", g_settings.tz, 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = on_time_sync;
    esp_netif_sntp_init(&cfg);
    ESP_LOGI(TAG, "SNTP started (pool.ntp.org), TZ=%s", g_settings.tz);
}

bool wifi_time_valid(void)
{
    // Anything before late 2023 means the clock was never set.
    return time(NULL) > 1700000000;
}
