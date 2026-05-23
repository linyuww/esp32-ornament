#include "system_status.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "wifi.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "system_status";
static bool sntp_started;
static volatile bool sntp_synced;

static int clamp_int(int value, int min, int max)
{
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    sntp_synced = true;
    ESP_LOGI(TAG, "SNTP time synchronized");
}

void system_status_start_time_sync(void)
{
    if (sntp_started) {
        return;
    }

    setenv("TZ", CONFIG_ORNAMENT_TIMEZONE, 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_ORNAMENT_SNTP_SERVER);
    esp_sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();
    sntp_started = true;
    ESP_LOGI(TAG, "SNTP started server=%s timezone=%s", CONFIG_ORNAMENT_SNTP_SERVER, CONFIG_ORNAMENT_TIMEZONE);
}

void system_status_update(ornament_state_t *state)
{
    if (state == NULL) {
        return;
    }

    wifi_status_snapshot(&state->wifi_connected, state->wifi_ssid, sizeof(state->wifi_ssid), &state->wifi_rssi);

    time_t now = 0;
    struct tm local = {0};
    time(&now);
    localtime_r(&now, &local);

    bool plausible_time = local.tm_year >= 123;
    state->time_synced = sntp_synced || plausible_time;
    if (state->time_synced) {
        int hour = clamp_int(local.tm_hour, 0, 23);
        int minute = clamp_int(local.tm_min, 0, 59);
        int month = clamp_int(local.tm_mon + 1, 1, 12);
        int day = clamp_int(local.tm_mday, 1, 31);
        snprintf(state->local_time, sizeof(state->local_time), "%02d:%02d", hour, minute);
        snprintf(state->local_date, sizeof(state->local_date), "%02d-%02d", month, day);
    } else {
        strlcpy(state->local_time, "--:--", sizeof(state->local_time));
        strlcpy(state->local_date, "-- --", sizeof(state->local_date));
    }
}
