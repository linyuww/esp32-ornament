#include "ornament_mdns.h"

#include "device_identity.h"
#include "esp_check.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "ornament_mdns";
static bool mdns_started;

esp_err_t ornament_mdns_start(void)
{
    if (mdns_started) {
        return ESP_OK;
    }

    char hostname[ORNAMENT_HOSTNAME_MAX];
    char instance[ORNAMENT_INSTANCE_MAX];
    device_identity_hostname(hostname, sizeof(hostname));
    device_identity_instance_name(instance, sizeof(instance));

    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mDNS init failed");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(hostname), TAG, "mDNS hostname set failed");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set(instance), TAG, "mDNS instance set failed");

    mdns_txt_item_t txt[] = {
        {"path", "/"},
        {"board", "esp32s3"},
    };
    ESP_RETURN_ON_ERROR(mdns_service_add(instance, "_http", "_tcp", 80, txt, 2), TAG, "mDNS HTTP service add failed");

    mdns_started = true;
    ESP_LOGI(TAG, "mDNS started: http://%s.local/", hostname);
    return ESP_OK;
}
