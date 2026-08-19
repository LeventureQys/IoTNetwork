/* facade fake：与 device_host.h 声明同签名的桩实现（不链接真实 host）。 */
#include "fake_host.h"
#include <string.h>

static device_snapshot_t g_snap;
static device_result_t g_drain_rc = DEVICE_OK;
static device_log_record_t g_logs[16];
static int g_log_count = 0;
static device_result_t g_send_rc = DEVICE_OK;
static device_result_t g_inject_rc = DEVICE_OK;

static int g_stop_requested = 0;
static int g_send_calls = 0;
static int g_inject_calls = 0;
static char g_last_app_data[520];
static size_t g_last_app_data_len = 0;
static char g_last_inject_action[64];
static char g_last_inject_argument[512];

static char g_fake_host_storage[128];

device_host_t *fake_host_instance(void)
{
    return (device_host_t *)(void *)g_fake_host_storage;
}

void fake_host_init(void)
{
    memset(&g_snap, 0, sizeof(g_snap));
    g_snap.host_state = DEVICE_HOST_RUNNING;
    g_snap.device_state = 0;
    g_snap.rssi_dbm = -50;
    g_snap.uptime_seconds = 42;
    g_snap.backend_kind = DEVICE_BACKEND_SIM;
    snprintf(g_snap.device_id, sizeof(g_snap.device_id), "02:00:00:00:00:01");
    snprintf(g_snap.target_ssid, sizeof(g_snap.target_ssid), "Modu_PC");
    snprintf(g_snap.pc_host_ip, sizeof(g_snap.pc_host_ip), "192.168.137.1");
    g_snap.pc_host_port = 5935;
    snprintf(g_snap.backend_name, sizeof(g_snap.backend_name), "sim");
    g_drain_rc = DEVICE_OK;
    g_log_count = 0;
    g_send_rc = DEVICE_OK;
    g_inject_rc = DEVICE_OK;
    g_stop_requested = 0;
    g_send_calls = 0;
    g_inject_calls = 0;
    g_last_app_data[0] = '\0';
    g_last_app_data_len = 0;
    g_last_inject_action[0] = '\0';
    g_last_inject_argument[0] = '\0';
}

void fake_host_set_snapshot(const device_snapshot_t *snap)
{
    if (snap)
        g_snap = *snap;
}

void fake_host_set_drain_result(device_result_t rc) { g_drain_rc = rc; }

void fake_host_push_log(const device_log_record_t *rec)
{
    if (rec && g_log_count < (int)(sizeof(g_logs) / sizeof(g_logs[0])))
        g_logs[g_log_count++] = *rec;
}

void fake_host_set_send_app_data_result(device_result_t rc) { g_send_rc = rc; }
void fake_host_set_inject_result(device_result_t rc) { g_inject_rc = rc; }

int fake_host_stop_requested(void) { return g_stop_requested; }
int fake_host_send_calls(void) { return g_send_calls; }
int fake_host_inject_calls(void) { return g_inject_calls; }
const char *fake_host_last_app_data(void) { return g_last_app_data; }
size_t fake_host_last_app_data_len(void) { return g_last_app_data_len; }
const char *fake_host_last_inject_action(void) { return g_last_inject_action; }
const char *fake_host_last_inject_argument(void) { return g_last_inject_argument; }

/* ---------------- device_host.h 桩实现 ---------------- */

device_result_t device_host_get_snapshot(const device_host_t *host,
                                         device_snapshot_t *out_snapshot)
{
    (void)host;
    if (out_snapshot == NULL)
        return DEVICE_ERR_INVALID_ARGUMENT;
    *out_snapshot = g_snap;
    return DEVICE_OK;
}

device_result_t device_host_drain_logs(device_host_t *host,
                                       device_log_record_t *records, size_t capacity,
                                       size_t *out_count, uint64_t *out_dropped)
{
    (void)host;
    if (out_count)
        *out_count = 0;
    if (out_dropped)
        *out_dropped = 0;
    if (g_drain_rc != DEVICE_OK)
        return g_drain_rc;
    size_t n = g_log_count < (int)capacity ? (size_t)g_log_count : capacity;
    for (size_t i = 0; i < n; i++)
        records[i] = g_logs[i];
    g_log_count = 0;
    if (out_count)
        *out_count = n;
    return DEVICE_OK;
}

device_result_t device_host_send_app_data(device_host_t *host, const char *utf8_text,
                                          size_t text_length, device_error_t *error)
{
    (void)host;
    if (error)
        memset(error, 0, sizeof(*error));
    if (g_send_rc == DEVICE_OK && utf8_text && text_length <= 512) {
        g_send_calls++;
        memcpy(g_last_app_data, utf8_text, text_length);
        g_last_app_data[text_length] = '\0';
        g_last_app_data_len = text_length;
        return DEVICE_OK;
    }
    if (error) {
        error->code = g_send_rc;
        snprintf(error->message, sizeof(error->message), "fake send error");
    }
    return g_send_rc;
}

device_result_t device_host_inject_fault(device_host_t *host, const char *action,
                                         const char *argument_json,
                                         device_error_t *error)
{
    (void)host;
    if (error)
        memset(error, 0, sizeof(*error));
    if (g_inject_rc == DEVICE_OK && action && action[0]) {
        g_inject_calls++;
        snprintf(g_last_inject_action, sizeof(g_last_inject_action), "%s", action);
        if (argument_json)
            snprintf(g_last_inject_argument, sizeof(g_last_inject_argument), "%s",
                     argument_json);
        return DEVICE_OK;
    }
    if (error) {
        error->code = g_inject_rc;
        snprintf(error->message, sizeof(error->message), "fake inject error");
    }
    return g_inject_rc;
}

device_result_t device_host_request_stop(device_host_t *host)
{
    (void)host;
    g_stop_requested = 1;
    return DEVICE_OK;
}
