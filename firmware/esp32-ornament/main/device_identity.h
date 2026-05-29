#pragma once

#include <stddef.h>

#define ORNAMENT_HOSTNAME_MAX 32
#define ORNAMENT_INSTANCE_MAX 48

void device_identity_hostname(char *out, size_t out_size);
void device_identity_instance_name(char *out, size_t out_size);
