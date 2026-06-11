#include "ornament_http_client.h"

#include "esp_check.h"
#include "esp_log.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/select.h>

#define ORNAMENT_HTTP_HOST_MAX 128
#define ORNAMENT_HTTP_PATH_MAX 512
#define ORNAMENT_HTTP_HEADER_MAX 768
#define ORNAMENT_HTTP_STATUS_LINE_MAX 96
#define ORNAMENT_HTTP_RESPONSE_HEADER_LINE_MAX 256
#define ORNAMENT_HTTP_CONTENT_TYPE_MAX 96

static const char *TAG = "ornament_http";

typedef struct {
    char host[ORNAMENT_HTTP_HOST_MAX];
    char path[ORNAMENT_HTTP_PATH_MAX];
    uint16_t port;
} ornament_http_url_t;

struct ornament_http_stream {
    int fd;
    int status_code;
    int64_t content_length;
    int64_t bytes_read;
    bool complete;
    char content_type[ORNAMENT_HTTP_CONTENT_TYPE_MAX];
};

static esp_err_t parse_http_url(const char *url, ornament_http_url_t *parsed)
{
    if (url == NULL || parsed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *cursor = url;
    const char *prefix = "http://";
    if (strncmp(cursor, prefix, strlen(prefix)) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    cursor += strlen(prefix);

    size_t host_len = strcspn(cursor, ":/?#");
    if (host_len == 0 || host_len >= sizeof(parsed->host)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(parsed->host, cursor, host_len);
    parsed->host[host_len] = '\0';
    cursor += host_len;

    parsed->port = 80;
    if (*cursor == ':') {
        cursor++;
        char *end = NULL;
        long port = strtol(cursor, &end, 10);
        if (end == cursor || port <= 0 || port > 65535) {
            return ESP_ERR_INVALID_ARG;
        }
        parsed->port = (uint16_t)port;
        cursor = end;
    }

    if (*cursor == '\0') {
        strlcpy(parsed->path, "/", sizeof(parsed->path));
        return ESP_OK;
    }
    if (*cursor != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlcpy(parsed->path, cursor, sizeof(parsed->path)) >= sizeof(parsed->path)) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t set_socket_timeout(int fd, int timeout_ms)
{
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t connect_http_socket(const ornament_http_url_t *url, int timeout_ms, int *fd_out)
{
    if (url == NULL || fd_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *fd_out = -1;

    char port_text[8] = {0};
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)url->port);
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *results = NULL;
    int gai = getaddrinfo(url->host, port_text, &hints, &results);
    if (gai != 0 || results == NULL) {
        ESP_LOGW(TAG, "resolve failed host=%s gai=%d", url->host, gai);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = ESP_FAIL;
    for (struct addrinfo *addr = results; addr != NULL; addr = addr->ai_next) {
        int fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (fd < 0) {
            continue;
        }
        (void)set_socket_timeout(fd, timeout_ms);
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
        int connected = connect(fd, addr->ai_addr, addr->ai_addrlen);
        if (connected < 0 && errno == EINPROGRESS) {
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(fd, &write_fds);
            struct timeval timeout = {
                .tv_sec = timeout_ms / 1000,
                .tv_usec = (timeout_ms % 1000) * 1000,
            };
            int selected = select(fd + 1, NULL, &write_fds, NULL, &timeout);
            connected = -1;
            if (selected > 0) {
                int socket_error = 0;
                socklen_t socket_error_len = sizeof(socket_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len) == 0 && socket_error == 0) {
                    connected = 0;
                } else if (socket_error != 0) {
                    errno = socket_error;
                }
            }
        }
        if (flags >= 0) {
            (void)fcntl(fd, F_SETFL, flags);
        }
        if (connected >= 0) {
            *fd_out = fd;
            err = ESP_OK;
            break;
        }
        close(fd);
    }
    freeaddrinfo(results);
    return err;
}

static esp_err_t send_all(int fd, const void *data, size_t len)
{
    const uint8_t *cursor = (const uint8_t *)data;
    while (len > 0) {
        ssize_t sent = send(fd, cursor, len, 0);
        if (sent <= 0) {
            return ESP_FAIL;
        }
        cursor += sent;
        len -= (size_t)sent;
    }
    return ESP_OK;
}

static esp_err_t read_byte(int fd, char *ch)
{
    ssize_t got = recv(fd, ch, 1, 0);
    if (got == 1) {
        return ESP_OK;
    }
    return got == 0 ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_TIMEOUT;
}

static esp_err_t read_line(int fd, char *target, size_t target_size)
{
    if (target == NULL || target_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t used = 0;
    while (used + 1 < target_size) {
        char ch = '\0';
        esp_err_t err = read_byte(fd, &ch);
        if (err != ESP_OK) {
            return err;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            target[used] = '\0';
            return ESP_OK;
        }
        target[used++] = ch;
    }
    target[target_size - 1] = '\0';
    return ESP_ERR_NO_MEM;
}

static bool header_is_content_length(const char *line, size_t *value)
{
    const char *name = "content-length:";
    size_t name_len = strlen(name);
    if (strncasecmp(line, name, name_len) != 0) {
        return false;
    }
    const char *cursor = line + name_len;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(cursor, &end, 10);
    if (end == cursor) {
        return false;
    }
    *value = (size_t)parsed;
    return true;
}

static bool copy_named_header_value(const char *line, const char *name, char *target, size_t target_size)
{
    size_t name_len = strlen(name);
    if (strncasecmp(line, name, name_len) != 0 || line[name_len] != ':') {
        return false;
    }
    const char *cursor = line + name_len + 1;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (target != NULL && target_size > 0) {
        strlcpy(target, cursor, target_size);
    }
    return true;
}

static esp_err_t send_http_request(int fd, const ornament_http_url_t *url, const ornament_http_request_t *request)
{
    char header[ORNAMENT_HTTP_HEADER_MAX] = {0};
    int written = snprintf(
        header,
        sizeof(header),
        "%s %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: %s\r\nAccept: %s\r\nConnection: close\r\n",
        request->method,
        url->path,
        url->host,
        (unsigned)url->port,
        request->user_agent != NULL ? request->user_agent : "codex-ornament/1.0",
        request->accept != NULL ? request->accept : "*/*");
    esp_err_t err = written > 0 && written < (int)sizeof(header) ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK && request->body != NULL && request->body_len > 0) {
        size_t used = strlen(header);
        written = snprintf(
            header + used,
            sizeof(header) - used,
            "Content-Type: %s\r\nContent-Length: %u\r\n",
            request->content_type != NULL ? request->content_type : "application/octet-stream",
            (unsigned)request->body_len);
        err = written > 0 && written < (int)(sizeof(header) - used) ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = strlcat(header, "\r\n", sizeof(header)) < sizeof(header) ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = send_all(fd, header, strlen(header));
    }
    if (err == ESP_OK && request->body != NULL && request->body_len > 0) {
        err = send_all(fd, request->body, request->body_len);
    }
    return err;
}

static esp_err_t read_response_headers(int fd, ornament_http_stream_t *stream)
{
    char line[ORNAMENT_HTTP_STATUS_LINE_MAX] = {0};
    esp_err_t err = read_line(fd, line, sizeof(line));
    if (err != ESP_OK) {
        return err;
    }
    int status = 0;
    if (sscanf(line, "HTTP/%*u.%*u %d", &status) != 1) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    stream->status_code = status;
    stream->content_length = -1;

    char header_line[ORNAMENT_HTTP_RESPONSE_HEADER_LINE_MAX] = {0};
    for (;;) {
        err = read_line(fd, header_line, sizeof(header_line));
        if (err != ESP_OK) {
            return err;
        }
        if (header_line[0] == '\0') {
            break;
        }
        size_t parsed_len = 0;
        if (header_is_content_length(header_line, &parsed_len)) {
            stream->content_length = (int64_t)parsed_len;
        } else {
            (void)copy_named_header_value(header_line, "content-type", stream->content_type, sizeof(stream->content_type));
        }
    }
    return ESP_OK;
}

static esp_err_t read_response_body(
    int fd,
    void *response,
    size_t response_capacity,
    size_t expected_len,
    bool has_content_length,
    size_t *response_len)
{
    size_t total = 0;
    uint8_t *target = (uint8_t *)response;
    for (;;) {
        if (has_content_length && total >= expected_len) {
            break;
        }
        size_t room = response_capacity > total ? response_capacity - total : 0;
        if (room == 0) {
            return ESP_ERR_NO_MEM;
        }
        size_t wanted = room;
        if (has_content_length && expected_len - total < wanted) {
            wanted = expected_len - total;
        }
        ssize_t got = recv(fd, target + total, wanted, 0);
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return ESP_ERR_TIMEOUT;
            }
            return ESP_FAIL;
        }
        if (got == 0) {
            break;
        }
        total += (size_t)got;
    }
    if (response_len != NULL) {
        *response_len = total;
    }
    if (response != NULL && total < response_capacity) {
        target[total] = '\0';
    }
    return has_content_length && total < expected_len ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t ornament_http_open_stream(const ornament_http_request_t *request, ornament_http_stream_t **stream)
{
    if (request == NULL || request->method == NULL || request->url == NULL || stream == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *stream = NULL;

    ornament_http_url_t url = {0};
    ESP_RETURN_ON_ERROR(parse_http_url(request->url, &url), TAG, "parse url");

    int fd = -1;
    int timeout_ms = request->timeout_ms > 0 ? request->timeout_ms : 5000;
    ESP_RETURN_ON_ERROR(connect_http_socket(&url, timeout_ms, &fd), TAG, "connect");

    esp_err_t err = send_http_request(fd, &url, request);
    if (err != ESP_OK) {
        close(fd);
        return err;
    }

    ornament_http_stream_t *opened = calloc(1, sizeof(*opened));
    if (opened == NULL) {
        close(fd);
        return ESP_ERR_NO_MEM;
    }
    opened->fd = fd;
    err = read_response_headers(fd, opened);
    if (err != ESP_OK) {
        free(opened);
        close(fd);
        return err;
    }
    *stream = opened;
    return ESP_OK;
}

esp_err_t ornament_http_request(const ornament_http_request_t *request)
{
    if (request == NULL || request->method == NULL || request->url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->response_capacity > 0 && request->response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ornament_http_stream_t *stream = NULL;
    esp_err_t err = ornament_http_open_stream(request, &stream);
    if (err != ESP_OK) {
        return err;
    }
    int status = ornament_http_stream_status_code(stream);
    if (request->status_code != NULL) {
        *request->status_code = status;
    }

    err = ESP_OK;
    if (request->response != NULL && request->response_capacity > 0) {
        err = read_response_body(
            stream->fd,
            request->response,
            request->response_capacity,
            stream->content_length >= 0 ? (size_t)stream->content_length : 0,
            stream->content_length >= 0,
            request->response_len);
        if (err == ESP_OK && request->response_len != NULL) {
            stream->bytes_read = (int64_t)*request->response_len;
            stream->complete = stream->content_length < 0 || stream->bytes_read >= stream->content_length;
        }
    } else if (request->response_len != NULL) {
        *request->response_len = 0;
    }
    ornament_http_stream_close(stream);
    return err;
}

esp_err_t ornament_http_stream_set_timeout(ornament_http_stream_t *stream, int timeout_ms)
{
    if (stream == NULL || stream->fd < 0 || timeout_ms <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return set_socket_timeout(stream->fd, timeout_ms);
}

esp_err_t ornament_http_stream_read(ornament_http_stream_t *stream, void *buffer, size_t capacity, size_t *bytes_read)
{
    if (stream == NULL || buffer == NULL || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes_read != NULL) {
        *bytes_read = 0;
    }
    if (stream->complete) {
        return ESP_OK;
    }
    if (stream->content_length >= 0) {
        int64_t remaining = stream->content_length - stream->bytes_read;
        if (remaining <= 0) {
            stream->complete = true;
            return ESP_OK;
        }
        if ((uint64_t)remaining < capacity) {
            capacity = (size_t)remaining;
        }
    }
    ssize_t got = recv(stream->fd, buffer, capacity, 0);
    if (got < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return ESP_ERR_TIMEOUT;
        }
        return ESP_FAIL;
    }
    if (got == 0) {
        stream->complete = true;
        return ESP_OK;
    }
    stream->bytes_read += got;
    if (stream->content_length >= 0 && stream->bytes_read >= stream->content_length) {
        stream->complete = true;
    }
    if (bytes_read != NULL) {
        *bytes_read = (size_t)got;
    }
    return ESP_OK;
}

void ornament_http_stream_close(ornament_http_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }
    if (stream->fd >= 0) {
        close(stream->fd);
    }
    free(stream);
}

int ornament_http_stream_status_code(const ornament_http_stream_t *stream)
{
    return stream != NULL ? stream->status_code : 0;
}

int64_t ornament_http_stream_content_length(const ornament_http_stream_t *stream)
{
    return stream != NULL ? stream->content_length : -1;
}

const char *ornament_http_stream_content_type(const ornament_http_stream_t *stream)
{
    return stream != NULL ? stream->content_type : NULL;
}

bool ornament_http_stream_is_complete(const ornament_http_stream_t *stream)
{
    return stream == NULL || stream->complete;
}
