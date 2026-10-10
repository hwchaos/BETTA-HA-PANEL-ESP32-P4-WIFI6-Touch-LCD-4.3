/* Ethernet link for the Waveshare ESP32-P4-ETH kit.
 *
 * Implements the wifi_mgr.h API on top of the P4's internal EMAC and the IP101
 * PHY, so the rest of the firmware (HA client, MQTT, web server, network
 * watchdog, settings UI) runs unchanged. Wi-Fi only concepts (setup AP, scan,
 * RSSI) report "not supported"; the static IP settings of the Wi-Fi page are
 * applied to the Ethernet interface.
 *
 * Pins as in the board's ESPHome configuration:
 *   IP101, PHY address 1, MDC GPIO31, MDIO GPIO52, PHY power/reset GPIO51,
 *   RMII reference clock from the PHY into GPIO50.
 */
#include "net/wifi_mgr.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"

#include "util/log_tags.h"

#define ETH_PHY_ADDR 1
#define ETH_MDC_GPIO 31
#define ETH_MDIO_GPIO 52
#define ETH_PHY_POWER_GPIO 51
#define ETH_REF_CLK_GPIO 50

#define ETH_GOT_IP_BIT BIT0

static const char *TAG = "eth_mgr";

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static esp_eth_handle_t s_eth;
static volatile bool s_link_up;
static volatile bool s_has_ip;
static wifi_mgr_link_stats_t s_stats;

static bool eth_parse_ipv4(const char *str, esp_ip4_addr_t *out)
{
    if (str == NULL || str[0] == '\0' || out == NULL) {
        return false;
    }
    ip4_addr_t addr;
    if (!ip4addr_aton(str, &addr)) {
        return false;
    }
    out->addr = addr.addr;
    return true;
}

static void eth_apply_static_ip(const wifi_mgr_config_t *cfg)
{
    if (cfg == NULL || !cfg->static_enabled || s_netif == NULL) {
        return;
    }
    esp_netif_ip_info_t ip_info = {0};
    if (!eth_parse_ipv4(cfg->static_ip, &ip_info.ip) || !eth_parse_ipv4(cfg->static_netmask, &ip_info.netmask) ||
        !eth_parse_ipv4(cfg->static_gateway, &ip_info.gw)) {
        ESP_LOGW(TAG, "Invalid static IP config, using DHCP");
        return;
    }
    esp_err_t err = esp_netif_dhcpc_stop(s_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "esp_netif_dhcpc_stop failed: %s", esp_err_to_name(err));
    }
    if (esp_netif_set_ip_info(s_netif, &ip_info) != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_ip_info failed, using DHCP");
        (void)esp_netif_dhcpc_start(s_netif);
        return;
    }
    esp_ip4_addr_t dns4;
    if (eth_parse_ipv4(cfg->static_dns, &dns4)) {
        esp_netif_dns_info_t dns_info = {0};
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        dns_info.ip.u_addr.ip4 = dns4;
        (void)esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    }
    ESP_LOGI(TAG, "Static IP %s / %s gw %s", cfg->static_ip, cfg->static_netmask, cfg->static_gateway);
}

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == ETH_EVENT) {
        switch (id) {
        case ETHERNET_EVENT_CONNECTED:
            s_link_up = true;
            ESP_LOGI(TAG, "Ethernet link up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            s_link_up = false;
            s_has_ip = false;
            s_stats.disconnect_count++;
            if (s_stats.last_connect_uptime_ms > 0) {
                s_stats.last_session_ms = esp_timer_get_time() / 1000 - s_stats.last_connect_uptime_ms;
            }
            xEventGroupClearBits(s_events, ETH_GOT_IP_BIT);
            ESP_LOGW(TAG, "Ethernet link down");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        s_has_ip = true;
        s_stats.connect_count++;
        s_stats.last_connect_uptime_ms = esp_timer_get_time() / 1000;
        xEventGroupSetBits(s_events, ETH_GOT_IP_BIT);
        ESP_LOGI(TAG, "Ethernet got IP " IPSTR, IP2STR(&ev->ip_info.ip));
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP) {
        s_has_ip = false;
        xEventGroupClearBits(s_events, ETH_GOT_IP_BIT);
        ESP_LOGW(TAG, "Ethernet lost IP");
    }
}

static esp_err_t eth_start_driver(void)
{
    /* Power the PHY before the driver talks to it (ESPHome: power_pin). */
    gpio_reset_pin(ETH_PHY_POWER_GPIO);
    gpio_set_direction(ETH_PHY_POWER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ETH_PHY_POWER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = ETH_REF_CLK_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (mac == NULL) {
        ESP_LOGE(TAG, "EMAC init failed");
        return ESP_FAIL;
    }

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = -1; /* powered above; GPIO51 stays high */
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "IP101 PHY init failed");
        mac->del(mac);
        return ESP_FAIL;
    }

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&config, &s_eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s", esp_err_to_name(err));
        phy->del(phy);
        mac->del(mac);
        return err;
    }
    err = esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach failed: %s", esp_err_to_name(err));
        return err;
    }
    return esp_eth_start(s_eth);
}

esp_err_t wifi_mgr_init(const wifi_mgr_config_t *cfg)
{
    if (s_events == NULL) {
        s_events = xEventGroupCreate();
        if (s_events == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (s_netif == NULL) {
        esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
        s_netif = esp_netif_new(&netif_cfg);
        if (s_netif == NULL) {
            return ESP_ERR_NO_MEM;
        }
        (void)esp_netif_set_hostname(s_netif, APP_NAME);
        ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, eth_event_handler, NULL));
        eth_apply_static_ip(cfg);
        err = eth_start_driver();
        if (err != ESP_OK) {
            return err;
        }
    }

    const bool wait = (cfg == NULL) || cfg->wait_for_ip;
    const int timeout_ms = (cfg != NULL && cfg->connect_timeout_ms > 0) ? cfg->connect_timeout_ms : 15000;
    if (!wait) {
        return ESP_OK;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, ETH_GOT_IP_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & ETH_GOT_IP_BIT) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "No IP within %d ms (link %s)", timeout_ms, s_link_up ? "up" : "down");
    return ESP_ERR_TIMEOUT;
}

bool wifi_mgr_eth_link_up(void)
{
    return s_link_up;
}

bool wifi_mgr_is_connected(void)
{
    return s_link_up && s_has_ip;
}

esp_err_t wifi_mgr_force_reconnect(void)
{
    if (s_eth == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_link_up && !s_has_ip && s_netif != NULL) {
        /* Link but no lease: kick DHCP. */
        (void)esp_netif_dhcpc_stop(s_netif);
        return esp_netif_dhcpc_start(s_netif);
    }
    return ESP_OK;
}

esp_err_t wifi_mgr_force_transport_recover(void)
{
    if (s_eth == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_stats.hard_recover_count++;
    ESP_LOGW(TAG, "Restarting the Ethernet driver");
    (void)esp_eth_stop(s_eth);
    vTaskDelay(pdMS_TO_TICKS(200));
    return esp_eth_start(s_eth);
}

esp_err_t wifi_mgr_start_setup_ap(const wifi_mgr_ap_config_t *cfg)
{
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_mgr_stop_setup_ap(void)
{
    return ESP_OK;
}

bool wifi_mgr_is_setup_ap_active(void)
{
    return false;
}

const char *wifi_mgr_get_setup_ap_ssid(void)
{
    return "";
}

static esp_err_t eth_get_ip_info(esp_netif_ip_info_t *info)
{
    if (s_netif == NULL || !s_has_ip) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_netif_get_ip_info(s_netif, info);
}

esp_err_t wifi_mgr_get_sta_ip(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    esp_netif_ip_info_t info;
    esp_err_t err = eth_get_ip_info(&info);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(out, out_len, IPSTR, IP2STR(&info.ip));
    return ESP_OK;
}

esp_err_t wifi_mgr_get_sta_gateway(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    esp_netif_ip_info_t info;
    esp_err_t err = eth_get_ip_info(&info);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(out, out_len, IPSTR, IP2STR(&info.gw));
    return ESP_OK;
}

esp_err_t wifi_mgr_get_ap_ip(char *out, size_t out_len)
{
    if (out != NULL && out_len > 0) {
        out[0] = '\0';
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_mgr_get_sta_ap_info(wifi_mgr_sta_ap_info_t *out_info)
{
    if (out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_info, 0, sizeof(*out_info));
    if (!wifi_mgr_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(out_info->ssid, sizeof(out_info->ssid), "Ethernet");
    return ESP_OK;
}

esp_err_t wifi_mgr_get_sta_rssi(int8_t *out_rssi_dbm)
{
    (void)out_rssi_dbm;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_mgr_get_link_stats(wifi_mgr_link_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_stats = s_stats;
    return ESP_OK;
}

esp_err_t wifi_mgr_scan(wifi_mgr_scan_result_t *results, size_t max_results, size_t *out_count)
{
    (void)results;
    (void)max_results;
    if (out_count != NULL) {
        *out_count = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
