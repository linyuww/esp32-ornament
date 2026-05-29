#include "device_identity.h"

#include "esp_mac.h"

#include <stdio.h>

void device_identity_hostname(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(out, out_size, "codex-ornament-%02x%02x", mac[4], mac[5]);
    } else {
        snprintf(out, out_size, "codex-ornament");
    }
}

void device_identity_instance_name(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(out, out_size, "Codex Ornament %02X%02X", mac[4], mac[5]);
    } else {
        snprintf(out, out_size, "Codex Ornament");
    }
}
