#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool connected;
    char ssid[33];
    char ip[16];
    char netmask[16];
    char gateway[16];
    char bssid[18];
    int rssi;
    int channel;
    int authmode;
    int retry_count;
    uint8_t last_disconnect_reason;
    int last_disconnect_rssi;
    const char *last_disconnect_name;
} wifi_debug_snapshot_t;

esp_err_t wifi_connect(void);
void wifi_status_snapshot(bool *connected, char *ssid, size_t ssid_size, int *rssi);
void wifi_debug_snapshot(wifi_debug_snapshot_t *snapshot);
