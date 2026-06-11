#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    const char *method;
    const char *url;
    const char *accept;
    const char *user_agent;
    const char *content_type;
    const void *body;
    size_t body_len;
    void *response;
    size_t response_capacity;
    size_t *response_len;
    int *status_code;
    int timeout_ms;
} ornament_http_request_t;

typedef struct ornament_http_stream ornament_http_stream_t;

esp_err_t ornament_http_request(const ornament_http_request_t *request);
esp_err_t ornament_http_open_stream(const ornament_http_request_t *request, ornament_http_stream_t **stream);
esp_err_t ornament_http_stream_set_timeout(ornament_http_stream_t *stream, int timeout_ms);
esp_err_t ornament_http_stream_read(ornament_http_stream_t *stream, void *buffer, size_t capacity, size_t *bytes_read);
void ornament_http_stream_close(ornament_http_stream_t *stream);
int ornament_http_stream_status_code(const ornament_http_stream_t *stream);
int64_t ornament_http_stream_content_length(const ornament_http_stream_t *stream);
const char *ornament_http_stream_content_type(const ornament_http_stream_t *stream);
bool ornament_http_stream_is_complete(const ornament_http_stream_t *stream);
