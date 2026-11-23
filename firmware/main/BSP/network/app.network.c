/**
 * @file    app.network.c
 * @brief   基于 W5500 的有线以太网（SPI 接口）应用层驱动实现。
 *          支持 NVS 保存 DHCP / Static IP 配置，支持动态应用与查询。
 */

#include "app.network.h"

#include <string.h>
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "app_network595.h"

#define W5500_SPI_HOST SPI2_HOST
#define W5500_CS       10
#define W5500_MOSI     11
#define W5500_SCK      12
#define W5500_MISO     13
#define W5500_INT      14
#define W5500_RST      15
#define IPV4_READY_BIT BIT0

static const char *TAG = "app_network";
static esp_eth_handle_t s_eth = NULL;
static esp_netif_t *s_netif = NULL;
static EventGroupHandle_t s_events = NULL;
static bool s_link_up = false;

// NVS 配置结构
#define NVS_ETH_NAMESPACE "eth_cfg"
static bool s_is_dhcp = true;
static char s_static_ip[16] = "192.168.1.188";
static char s_static_netmask[16] = "255.255.255.0";
static char s_static_gateway[16] = "192.168.1.1";
static char s_static_dns[16] = "114.114.114.114";

static void load_nvs_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_ETH_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t dhcp_val = 1;
        nvs_get_u8(nvs, "dhcp", &dhcp_val);
        s_is_dhcp = (dhcp_val == 1);

        size_t len = sizeof(s_static_ip);
        nvs_get_str(nvs, "ip", s_static_ip, &len);
        len = sizeof(s_static_netmask);
        nvs_get_str(nvs, "mask", s_static_netmask, &len);
        len = sizeof(s_static_gateway);
        nvs_get_str(nvs, "gw", s_static_gateway, &len);
        len = sizeof(s_static_dns);
        nvs_get_str(nvs, "dns", s_static_dns, &len);
        nvs_close(nvs);
        ESP_LOGI(TAG, "从 NVS 读取到以太网配置: %s (IP: %s)", s_is_dhcp ? "DHCP" : "静态IP", s_static_ip);
    } else {
        ESP_LOGI(TAG, "NVS 无以太网配置，使用默认 DHCP 模式");
    }
}

static void save_nvs_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_ETH_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "dhcp", s_is_dhcp ? 1 : 0);
        nvs_set_str(nvs, "ip", s_static_ip);
        nvs_set_str(nvs, "mask", s_static_netmask);
        nvs_set_str(nvs, "gw", s_static_gateway);
        nvs_set_str(nvs, "dns", s_static_dns);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "以太网配置已成功持久化至 NVS");
    }
}

static void apply_ip_config(void)
{
    if (!s_netif) return;

    if (s_is_dhcp) {
        ESP_LOGI(TAG, "正在启用 DHCP 动态获取 IP...");
        esp_netif_dhcpc_start(s_netif);
    } else {
        ESP_LOGI(TAG, "正在设置静态 IP: %s / %s / 网关: %s ...", s_static_ip, s_static_netmask, s_static_gateway);
        esp_netif_dhcpc_stop(s_netif);

        esp_netif_ip_info_t ip_info;
        ip_info.ip.addr = esp_ip4addr_aton(s_static_ip);
        ip_info.netmask.addr = esp_ip4addr_aton(s_static_netmask);
        ip_info.gw.addr = esp_ip4addr_aton(s_static_gateway);
        esp_netif_set_ip_info(s_netif, &ip_info);

        esp_netif_dns_info_t dns_info;
        dns_info.ip.u_addr.ip4.addr = esp_ip4addr_aton(s_static_dns);
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns_info);

        xEventGroupSetBits(s_events, IPV4_READY_BIT);
        app_network595_set_eth_state(true);
    }
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (*(esp_eth_handle_t *)data != s_eth) return;

    switch (id) {
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "W5500 driver started");
            break;
        case ETHERNET_EVENT_CONNECTED:
            s_link_up = true;
            ESP_LOGI(TAG, "Ethernet link up");
            apply_ip_config();
            break;
        case ETHERNET_EVENT_DISCONNECTED:
        case ETHERNET_EVENT_STOP:
            s_link_up = false;
            xEventGroupClearBits(s_events, IPV4_READY_BIT);
            ESP_LOGI(TAG, "Ethernet link unavailable");
            app_network595_set_eth_state(false);
            break;
        default:
            break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *event = data;
    if (event->esp_netif != s_netif) return;

    if (id == IP_EVENT_ETH_LOST_IP) {
        xEventGroupClearBits(s_events, IPV4_READY_BIT);
        ESP_LOGW(TAG, "Ethernet IPv4 lost");
        app_network595_set_eth_state(false);
        return;
    }

    const esp_netif_ip_info_t *ip = &event->ip_info;
    ESP_LOGI(TAG, "以太网 IPv4 就绪: " IPSTR " 掩码: " IPSTR " 网关: " IPSTR,
             IP2STR(&ip->ip), IP2STR(&ip->netmask), IP2STR(&ip->gw));
    xEventGroupSetBits(s_events, IPV4_READY_BIT);
    app_network595_set_eth_state(true);
}

void app_network_get_eth_info(eth_network_info_t *info)
{
    if (!info) return;
    info->is_dhcp = s_is_dhcp;
    info->link_up = s_link_up;

    uint8_t mac[6] = {0};
    if (s_eth) {
        esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, mac);
    }
    snprintf(info->mac, sizeof(info->mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esp_netif_ip_info_t ip_info = {0};
    if (s_netif && esp_netif_get_ip_info(s_netif, &ip_info) == ESP_OK) {
        snprintf(info->ip, sizeof(info->ip), IPSTR, IP2STR(&ip_info.ip));
        snprintf(info->netmask, sizeof(info->netmask), IPSTR, IP2STR(&ip_info.netmask));
        snprintf(info->gateway, sizeof(info->gateway), IPSTR, IP2STR(&ip_info.gw));
    } else {
        strlcpy(info->ip, s_static_ip, sizeof(info->ip));
        strlcpy(info->netmask, s_static_netmask, sizeof(info->netmask));
        strlcpy(info->gateway, s_static_gateway, sizeof(info->gateway));
    }
    strlcpy(info->dns, s_static_dns, sizeof(info->dns));
}

esp_err_t app_network_set_config(bool is_dhcp, const char *ip, const char *netmask, const char *gateway, const char *dns)
{
    s_is_dhcp = is_dhcp;
    if (ip) strlcpy(s_static_ip, ip, sizeof(s_static_ip));
    if (netmask) strlcpy(s_static_netmask, netmask, sizeof(s_static_netmask));
    if (gateway) strlcpy(s_static_gateway, gateway, sizeof(s_static_gateway));
    if (dns) strlcpy(s_static_dns, dns, sizeof(s_static_dns));

    save_nvs_config();

    if (s_link_up) {
        apply_ip_config();
    }
    return ESP_OK;
}

esp_err_t app_network_init(void)
{
    if (s_eth != NULL) return ESP_OK;

    load_nvs_config();

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    const spi_bus_config_t buscfg = {
        .miso_io_num = W5500_MISO,
        .mosi_io_num = W5500_MOSI,
        .sclk_io_num = W5500_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(W5500_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI bus init failed");

    spi_device_interface_config_t devcfg = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = 20 * 1000 * 1000,
        .spics_io_num = W5500_CS,
        .queue_size = 20,
    };

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, &devcfg);
    w5500_config.base.int_gpio_num = W5500_INT;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (!mac) return ESP_FAIL;

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = W5500_RST;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) {
        mac->del(mac);
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_config, &s_eth);
    if (err != ESP_OK) {
        phy->del(phy);
        mac->del(mac);
        return err;
    }

    uint8_t eth_mac[6];
    esp_read_mac(eth_mac, ESP_MAC_ETH);
    esp_eth_ioctl(s_eth, ETH_CMD_S_MAC_ADDR, eth_mac);

    s_events = xEventGroupCreate();
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    esp_netif_set_hostname(s_netif, "esp32s3-gateway");
    esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth));

    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_ip_event, NULL);

    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth), TAG, "esp_eth_start failed");
    ESP_LOGI(TAG, "W5500 SPI 以太网驱动已启动");
    return ESP_OK;
}

esp_err_t app_network_wait_for_ipv4(TickType_t timeout_ticks)
{
    if (s_events == NULL) return ESP_ERR_INVALID_STATE;
    EventBits_t bits = xEventGroupWaitBits(s_events, IPV4_READY_BIT, pdFALSE, pdTRUE, timeout_ticks);
    return (bits & IPV4_READY_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_network_http_check(const char *url, int *status_code)
{
    if (url == NULL || status_code == NULL) return ESP_ERR_INVALID_ARG;
    *status_code = 0;
    ESP_RETURN_ON_ERROR(app_network_wait_for_ipv4(0), TAG, "Ethernet has no IPv4");

    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 4000,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_FAIL;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        *status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP GET status: %d", *status_code);
    }
    esp_http_client_cleanup(client);
    return err;
}
