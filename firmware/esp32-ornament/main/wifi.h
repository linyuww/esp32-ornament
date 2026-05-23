#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>

esp_err_t wifi_connect(void);
void wifi_status_snapshot(bool *connected, char *ssid, size_t ssid_size, int *rssi);
