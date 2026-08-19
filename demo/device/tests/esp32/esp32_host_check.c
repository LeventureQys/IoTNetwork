/*
 * ESP32-C2 后端宿主检查（纯 C 语法/接口完整性检查）。
 *
 * 用途：在宿主机（Windows/Linux）上验证
 *   demo/device/backends/esp32c2/esp32c2_backend.c
 * 能以纯 C 编译、vtable 布局完整（net_backend_t 全字段非空）、骨架行为
 * 明确失败（未实现接口返回 DEMO_ERR，不宣称可用）。
 *
 * 注意：这是宿主检查，不是 ESP-IDF 构建；不得据此报告 ESP-IDF build 通过。
 */
#include "esp32c2_impl.h"

#include <stdio.h>

static int g_failures = 0;

static void check(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        ++g_failures;
    }
}

static void check_vtable(const net_backend_t *be)
{
    check(be != NULL, "esp32c2_backend_get() 返回非空 vtable");
    if (be == NULL)
        return;
    check(be->init != NULL, "init");
    check(be->deinit != NULL, "deinit");
    check(be->wifi_scan != NULL, "wifi_scan");
    check(be->wifi_sta_connect != NULL, "wifi_sta_connect");
    check(be->wifi_sta_disconnect != NULL, "wifi_sta_disconnect");
    check(be->wifi_ap_start != NULL, "wifi_ap_start");
    check(be->wifi_ap_stop != NULL, "wifi_ap_stop");
    check(be->wifi_get_rssi != NULL, "wifi_get_rssi");
    check(be->wifi_get_ip != NULL, "wifi_get_ip");
    check(be->wifi_get_current_ssid != NULL, "wifi_get_current_ssid");
    check(be->wifi_get_gateway != NULL, "wifi_get_gateway");
    check(be->tcp_listen != NULL, "tcp_listen");
    check(be->tcp_accept != NULL, "tcp_accept");
    check(be->tcp_connect != NULL, "tcp_connect");
    check(be->sock_send != NULL, "sock_send");
    check(be->sock_recv != NULL, "sock_recv");
    check(be->sock_close != NULL, "sock_close");
    check(be->udp_mcast_join != NULL, "udp_mcast_join");
    check(be->udp_send != NULL, "udp_send");
    check(be->udp_recv != NULL, "udp_recv");
    check(be->mdns_register != NULL, "mdns_register");
    check(be->mdns_unregister != NULL, "mdns_unregister");
    check(be->mdns_resolve != NULL, "mdns_resolve");
    check(be->nvs_get != NULL, "nvs_get");
    check(be->nvs_set != NULL, "nvs_set");
    check(be->nvs_erase != NULL, "nvs_erase");
    check(be->time_ms != NULL, "time_ms");
    check(be->random != NULL, "random");
    check(be->inject != NULL, "inject");
}

/* 骨架占位行为：未实现接口必须显式失败，不得静默返回成功。 */
static void check_skeleton_behavior(const net_backend_t *be)
{
    int count = 1;
    int rssi = 0;
    uint32_t ip = 1;
    uint32_t gw = 1;
    char ssid[33] = {0};
    void *sock = (void *)1;
    net_ap_info_t ap;
    net_addr_t peer;
    wifi_reason_t reason = WIFI_REASON_OK;
    const uint8_t byte = 0x42;

    check(be->wifi_scan(NULL, &ap, &count) != DEMO_OK, "wifi_scan 占位失败");
    check(be->wifi_sta_connect(NULL, "ssid", "pass", &reason) != DEMO_OK,
          "wifi_sta_connect 占位失败");
    check(be->wifi_sta_disconnect(NULL) != DEMO_OK, "wifi_sta_disconnect 占位失败");
    check(be->wifi_ap_start(NULL, "ssid", "pass", "1234") != DEMO_OK,
          "wifi_ap_start 占位失败");
    check(be->wifi_ap_stop(NULL) != DEMO_OK, "wifi_ap_stop 占位失败");
    check(be->wifi_get_rssi(NULL, &rssi) != DEMO_OK, "wifi_get_rssi 占位失败");
    check(be->wifi_get_ip(NULL, &ip) != DEMO_OK, "wifi_get_ip 占位失败");
    check(be->wifi_get_current_ssid(NULL, ssid, sizeof(ssid)) != DEMO_OK,
          "wifi_get_current_ssid 占位失败");
    check(be->wifi_get_gateway(NULL, &gw) != DEMO_OK, "wifi_get_gateway 占位失败");
    check(be->tcp_listen(NULL, 5935, &sock) != DEMO_OK, "tcp_listen 占位失败");
    check(be->tcp_accept(NULL, NULL, &sock, &peer) != DEMO_OK, "tcp_accept 占位失败");
    check(be->tcp_connect(NULL, NULL, &sock, 100) != DEMO_OK, "tcp_connect 占位失败");
    check(be->sock_send(NULL, NULL, &byte, 1) != DEMO_OK, "sock_send 占位失败");
    check(be->sock_recv(NULL, NULL, (uint8_t *)&byte, 1) != DEMO_OK,
          "sock_recv 占位失败");
    check(be->udp_mcast_join(NULL, "224.0.2.1", 5936, &sock) != DEMO_OK,
          "udp_mcast_join 占位失败");
    check(be->udp_send(NULL, "224.0.2.1", 5936, &byte, 1) != DEMO_OK,
          "udp_send 占位失败");
    check(be->udp_recv(NULL, NULL, (uint8_t *)&byte, 1, &peer) != DEMO_OK,
          "udp_recv 占位失败");
    check(be->mdns_register(NULL, NULL) != DEMO_OK, "mdns_register 占位失败");
    check(be->mdns_unregister(NULL, "_tactile._tcp") != DEMO_OK,
          "mdns_unregister 占位失败");
    check(be->mdns_resolve(NULL, "_tactile._tcp", NULL, 100) != DEMO_OK,
          "mdns_resolve 占位失败");
    {
        int len = 32;
        check(be->nvs_get(NULL, "wifi_creds", (uint8_t *)ssid, &len) != DEMO_OK,
              "nvs_get 占位失败");
        check(be->nvs_set(NULL, "wifi_creds", (const uint8_t *)ssid, 0) != DEMO_OK,
              "nvs_set 占位失败");
        check(be->nvs_erase(NULL, "wifi_creds") != DEMO_OK, "nvs_erase 占位失败");
    }
    check(be->inject(NULL, "fault", "{}") != DEMO_OK, "inject 占位失败");
    check(be->time_ms(NULL) == 0, "time_ms 占位返回 0");
    check(be->random(NULL) == 0, "random 占位返回 0");
}

int main(void)
{
    const net_backend_t *be = esp32c2_backend_get();

    check_vtable(be);
    check_skeleton_behavior(be);
    if (g_failures == 0) {
        printf("esp32_host_check: PASS (vtable 完整，占位行为显式失败)\n");
        return 0;
    }
    fprintf(stderr, "esp32_host_check: FAILED with %d checks\n", g_failures);
    return 1;
}
