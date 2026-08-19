/* ESP32-C2 网络抽象层后端骨架
 * 目标芯片：ESP32-C2（ESP8684，RISC-V 单核 120MHz）
 * 状态：骨架交付，未在真实硬件验证。每个接口给出 ESP-IDF API 映射实现或占位。
 * 移植步骤见同目录 README.md。宿主机构建仅通过 tests/esp32 的语法/接口检查。 */
#include "esp32c2_impl.h"
#include <string.h>

/* 真实移植时包含：
 * #include "esp_wifi.h"
 * #include "esp_netif.h"
 * #include "esp_event.h"
 * #include "nvs_flash.h"
 * #include "mdns.h"
 * #include "esp_timer.h"
 * #include "esp_random.h"
 * #include "lwip/sockets.h"
 */

#define ESP32C2_NVS_NAMESPACE "provision"

/* ---------------- 生命周期 ---------------- */

static int esp32c2_init(void *user, const char *config_path)
{
    (void)user;
    (void)config_path;
    /* 真实实现：
     *   nvs_flash_init();
     *   esp_netif_init();
     *   esp_event_loop_create_default();
     *   esp_wifi_init(&cfg);
     */
    return DEMO_OK;
}

static void esp32c2_deinit(void *user)
{
    (void)user;
    /* 真实实现：esp_wifi_stop(); esp_wifi_deinit(); */
}

/* ---------------- WiFi ---------------- */

static int esp32c2_wifi_scan(void *user, net_ap_info_t *aps, int *count)
{
    (void)user;
    (void)aps;
    (void)count;
    /* 真实实现：
     *   esp_wifi_scan_start(NULL, true);
     *   esp_wifi_scan_get_ap_records(&num, records);
     *   // 过滤 Modu_ 前缀与频段，填充 net_ap_info_t（SSID ≤32B 截断）
     */
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_wifi_sta_connect(void *user, const char *ssid, const char *pass,
                                    wifi_reason_t *reason)
{
    (void)user;
    (void)ssid;
    (void)pass;
    /* 真实实现（事件驱动，占位说明）：
     *   1) esp_wifi_set_mode(WIFI_MODE_STA);
     *   2) esp_wifi_set_config(WIFI_IF_STA, &cfg);   // cfg.sta.ssid/password
     *   3) esp_wifi_connect();
     *   4) 注册事件回调 WIFI_EVENT_STA_DISCONNECTED：
     *        事件 reason 字段与 wifi_reason_t 数值已对齐（201/202/205），
     *        在回调中把结果回传给等待方（本抽象层为同步语义，
     *        建议用事件组/信号量等待连接结果，超时返回 DEMO_ERR_TIMEOUT）。
     */
    if (reason) *reason = WIFI_REASON_OK;
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_wifi_sta_disconnect(void *user)
{
    (void)user;
    /* 真实实现：esp_wifi_disconnect(); */
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_wifi_ap_start(void *user, const char *ssid, const char *pass,
                                 const char *pin)
{
    (void)user;
    (void)ssid;
    (void)pass;
    (void)pin;
    /* 真实实现：
     *   wifi_config_t cfg = {0};
     *   snprintf((char*)cfg.ap.ssid, sizeof(cfg.ap.ssid), "%s", ssid);
     *   snprintf((char*)cfg.ap.password, sizeof(cfg.ap.password), "%s", pass);
     *   cfg.ap.ssid_len = strlen(ssid);
     *   cfg.ap.channel = 6;
     *   cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;      // WPA2 双因子（协议文档 5.1）
     *   cfg.ap.max_connection = 1;
     *   esp_wifi_set_mode(WIFI_MODE_AP);
     *   esp_wifi_set_config(WIFI_IF_AP, &cfg);
     *   esp_wifi_start();
     *   // PIN 由业务层生成，真实设备打印到标签/日志
     */
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_wifi_ap_stop(void *user)
{
    (void)user;
    /* 真实实现：esp_wifi_set_mode(WIFI_MODE_NULL); */
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_wifi_get_rssi(void *user, int *rssi)
{
    (void)user;
    (void)rssi;
    /* 真实实现：
     *   wifi_ap_record_t info;
     *   esp_wifi_sta_get_ap_info(&info); *rssi = info.rssi;
     */
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_wifi_get_ip(void *user, uint32_t *ip)
{
    (void)user;
    (void)ip;
    /* 真实实现：
     *   esp_netif_ip_info_t info;
     *   esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
     *   esp_netif_get_ip_info(nif, &info);
     *   *ip = info.ip.addr;   // 网络字节序
     */
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_wifi_get_current_ssid(void *user, char *ssid, int capacity)
{
    (void)user;
    (void)ssid;
    (void)capacity;
    /* 真实实现：
     *   wifi_ap_record_t info = {0};
     *   if (esp_wifi_sta_get_ap_info(&info) != ESP_OK) return DEMO_ERR;
     *   snprintf(ssid, capacity, "%s", (const char *)info.ssid);
     *   return DEMO_OK;
     */
    return DEMO_ERR;
}

/* ---------------- TCP（lwIP BSD socket） ---------------- */

static int esp32c2_tcp_listen(void *user, uint16_t port, void **sock)
{
    (void)user;
    (void)port;
    (void)sock;
    /* 真实实现：
     *   int s = socket(AF_INET, SOCK_STREAM, 0);
     *   int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
     *   sockaddr_in a = {0}; a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY;
     *   a.sin_port=htons(port); bind(s,(sockaddr*)&a,sizeof(a)); listen(s,4);
     *   fcntl(s, F_SETFL, O_NONBLOCK);
     *   *sock = (void*)(intptr_t)s;
     */
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_tcp_accept(void *user, void *listen, void **conn, net_addr_t *peer)
{
    (void)user;
    (void)listen;
    (void)conn;
    (void)peer;
    /* 真实实现：lwIP accept，EAGAIN → DEMO_ERR_AGAIN */
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_tcp_connect(void *user, const net_addr_t *addr, void **sock,
                               int timeout_ms)
{
    (void)user;
    (void)addr;
    (void)sock;
    (void)timeout_ms;
    /* 真实实现：非阻塞 connect + select 超时；注意真实环境无虚拟地址翻译，
     * addr->ip/port 即真实局域网地址 */
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_sock_send(void *user, void *sock, const uint8_t *buf, int len)
{
    (void)user;
    (void)sock;
    (void)buf;
    (void)len;
    /* 真实实现：lwIP send，EAGAIN → DEMO_ERR_AGAIN */
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_sock_recv(void *user, void *sock, uint8_t *buf, int cap)
{
    (void)user;
    (void)sock;
    (void)buf;
    (void)cap;
    /* 真实实现：lwIP recv，EAGAIN → DEMO_ERR_AGAIN；recv==0 → DEMO_ERR */
    return DEMO_ERR; /* 占位 */
}

static void esp32c2_sock_close(void *user, void *sock)
{
    (void)user;
    (void)sock;
    /* 真实实现：close((int)(intptr_t)sock); */
}

/* ---------------- UDP 组播 ---------------- */

static int esp32c2_udp_mcast_join(void *user, const char *group, uint16_t port,
                                  void **sock)
{
    (void)user;
    (void)group;
    (void)port;
    (void)sock;
    /* 真实实现：
     *   int s = socket(AF_INET, SOCK_DGRAM, 0);
     *   ip_mreq m = {0}; m.imr_multiaddr.s_addr = inet_addr(group);
     *   setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m));
     *   bind(s, ..., port); fcntl(s, F_SETFL, O_NONBLOCK);
     */
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_udp_send(void *user, const char *group, uint16_t port,
                            const uint8_t *buf, int len)
{
    (void)user;
    (void)group;
    (void)port;
    (void)buf;
    (void)len;
    /* 真实实现：sendto 组播地址，IP_MULTICAST_TTL=1 */
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_udp_recv(void *user, void *sock, uint8_t *buf, int cap,
                            net_addr_t *from)
{
    (void)user;
    (void)sock;
    (void)buf;
    (void)cap;
    (void)from;
    /* 真实实现：recvfrom，EAGAIN → DEMO_ERR_AGAIN */
    return DEMO_ERR; /* 占位 */
}

/* ---------------- mDNS ---------------- */

static int esp32c2_mdns_register(void *user, const net_mdns_service_t *svc)
{
    (void)user;
    (void)svc;
    /* 真实实现：
     *   mdns_init();
     *   mdns_hostname_set("tactile-host");
     *   mdns_instance_name_set("host");
     *   mdns_service_add(NULL, "_tactile", "_tcp", 5935, NULL, 0);
     *   mdns_service_txt_item_set("_tactile", "_tcp", "ip", <host_ip>);
     *   mdns_service_txt_item_set("_tactile", "_tcp", "tcp_port", "5935");
     */
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_mdns_unregister(void *user, const char *type)
{
    (void)user;
    (void)type;
    /* 真实实现：mdns_service_remove("_tactile", "_tcp"); */
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_mdns_resolve(void *user, const char *type,
                                net_mdns_service_t *out, int timeout_ms)
{
    (void)user;
    (void)type;
    (void)out;
    (void)timeout_ms;
    /* 真实实现：mdns_query_a("host._tactile._tcp.local", timeout_ms, &result) */
    return DEMO_ERR; /* 占位 */
}

/* ---------------- NVS ---------------- */

static int esp32c2_nvs_get(void *user, const char *key, uint8_t *buf, int *len)
{
    (void)user;
    (void)key;
    (void)buf;
    (void)len;
    /* 真实实现：
     *   nvs_handle_t h; nvs_open(ESP32C2_NVS_NAMESPACE, NVS_READONLY, &h);
     *   int rc = nvs_get_blob(h, key, buf, len);   // len 入=容量 出=长度
     *   nvs_close(h);  // ESP_ERR_NVS_NOT_FOUND → DEMO_ERR 且 *len=0
     */
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_nvs_set(void *user, const char *key, const uint8_t *buf, int len)
{
    (void)user;
    (void)key;
    (void)buf;
    (void)len;
    /* 真实实现：nvs_open(..., NVS_READWRITE) + nvs_set_blob + nvs_commit */
    return DEMO_ERR; /* 占位 */
}

static int esp32c2_nvs_erase(void *user, const char *key)
{
    (void)user;
    (void)key;
    /* 真实实现：nvs_open + nvs_erase_key */
    return DEMO_ERR; /* 占位 */
}

/* ---------------- 系统 ---------------- */

static uint64_t esp32c2_time_ms(void *user)
{
    (void)user;
    /* 真实实现：esp_timer_get_time() / 1000 */
    return 0; /* 占位 */
}

static uint32_t esp32c2_random(void *user)
{
    (void)user;
    /* 真实实现：esp_random() */
    return 0; /* 占位 */
}

static int esp32c2_wifi_get_gateway(void *user, uint32_t *ip)
{
    (void)user;
    if (ip)
        *ip = 0;
    return DEMO_ERR;
}

static int esp32c2_inject(void *user, const char *action, const char *arg_json)
{
    (void)user;
    (void)action;
    (void)arg_json;
    return DEMO_ERR; /* 注入仅模拟后端可用 */
}

/* ---------------- vtable ---------------- */

static const net_backend_t g_esp32c2_backend = {
    esp32c2_init, esp32c2_deinit,
    esp32c2_wifi_scan, esp32c2_wifi_sta_connect, esp32c2_wifi_sta_disconnect,
    esp32c2_wifi_ap_start, esp32c2_wifi_ap_stop,
    esp32c2_wifi_get_rssi, esp32c2_wifi_get_ip,
    esp32c2_wifi_get_current_ssid, esp32c2_wifi_get_gateway,
    esp32c2_tcp_listen, esp32c2_tcp_accept, esp32c2_tcp_connect,
    esp32c2_sock_send, esp32c2_sock_recv, esp32c2_sock_close,
    esp32c2_udp_mcast_join, esp32c2_udp_send, esp32c2_udp_recv,
    esp32c2_mdns_register, esp32c2_mdns_unregister, esp32c2_mdns_resolve,
    esp32c2_nvs_get, esp32c2_nvs_set, esp32c2_nvs_erase,
    esp32c2_time_ms, esp32c2_random,
    esp32c2_inject,
};

const net_backend_t *esp32c2_backend_get(void)
{
    return &g_esp32c2_backend;
}
