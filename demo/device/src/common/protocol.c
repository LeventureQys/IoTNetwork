#include "protocol.h"

const char *device_state_str(device_state_t s)
{
    switch (s) {
    case DEV_STATE_BOOT:       return "boot";
    case DEV_STATE_WIFI_SCAN:  return "wifi_scan";
    case DEV_STATE_STA_JOIN:   return "sta_join";
    case DEV_STATE_CONNECT:    return "connect";
    case DEV_STATE_SESSION:    return "session";
    case DEV_STATE_HEAL:       return "heal";
    default:                   return "unknown";
    }
}
