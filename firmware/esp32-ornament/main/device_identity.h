#pragma once

#include "esp_err.h"

const char *device_identity_hostname(void);
const char *device_identity_mdns_url(void);
esp_err_t device_identity_init(void);
esp_err_t device_identity_start_mdns(void);

