#include "xiaozhi_mcp.h"

#include "music_player.h"

#include "esp_check.h"
#include "esp_heap_caps.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_ORNAMENT_XIAOZHI_MCP_ENABLED
#define CONFIG_ORNAMENT_XIAOZHI_MCP_ENABLED 0
#endif

#define MCP_SERVER_NAME "esp32-ornament"
#define MCP_SERVER_VERSION "0.1.0"
#define MCP_PROTOCOL_VERSION "2024-11-05"
#define MCP_RESPONSE_BUFFER_INITIAL 768
#define MCP_RESPONSE_BUFFER_MAX 4096

#if CONFIG_ORNAMENT_XIAOZHI_ENABLED && CONFIG_ORNAMENT_XIAOZHI_MCP_ENABLED

static void append_text(char *target, size_t target_size, const char *text)
{
    if (target == NULL || target_size == 0 || text == NULL) {
        return;
    }
    size_t used = strnlen(target, target_size);
    if (used >= target_size - 1) {
        return;
    }
    strlcpy(target + used, text, target_size - used);
}

static void json_escape_append(char *target, size_t target_size, const char *text)
{
    if (target == NULL || target_size == 0 || text == NULL) {
        return;
    }

    size_t used = strnlen(target, target_size);
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor != '\0' && used + 1 < target_size; cursor++) {
        unsigned char ch = *cursor;
        const char *escape = NULL;
        if (ch == '\\') {
            escape = "\\\\";
        } else if (ch == '"') {
            escape = "\\\"";
        } else if (ch == '\n') {
            escape = "\\n";
        } else if (ch == '\r') {
            escape = "\\r";
        } else if (ch == '\t') {
            escape = "\\t";
        }

        if (escape != NULL) {
            size_t escape_len = strlen(escape);
            if (used + escape_len >= target_size) {
                break;
            }
            memcpy(target + used, escape, escape_len);
            used += escape_len;
        } else if (ch >= 0x20) {
            target[used++] = (char)ch;
        }
        target[used] = '\0';
    }
}

static esp_err_t format_id_json(const cJSON *id, char *target, size_t target_size)
{
    if (target == NULL || target_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (id == NULL || cJSON_IsNull(id)) {
        strlcpy(target, "null", target_size);
        return ESP_OK;
    }
    if (cJSON_IsNumber(id)) {
        int written = snprintf(target, target_size, "%.0f", id->valuedouble);
        return written > 0 && written < (int)target_size ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (cJSON_IsString(id) && id->valuestring != NULL) {
        strlcpy(target, "\"", target_size);
        json_escape_append(target, target_size, id->valuestring);
        append_text(target, target_size, "\"");
        size_t used = strnlen(target, target_size);
        return used > 0 && target[used - 1] == '"' ? ESP_OK : ESP_ERR_NO_MEM;
    }

    return ESP_ERR_INVALID_ARG;
}

static cJSON *mcp_base_response(const cJSON *id)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    if (id != NULL) {
        cJSON_AddItemToObject(root, "id", cJSON_Duplicate((cJSON *)id, true));
    } else {
        cJSON_AddNullToObject(root, "id");
    }
    return root;
}

static cJSON *mcp_error_response(const cJSON *id, int code, const char *message)
{
    cJSON *root = mcp_base_response(id);
    if (root == NULL) {
        return NULL;
    }
    cJSON *error = cJSON_AddObjectToObject(root, "error");
    if (error == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message != NULL ? message : "error");
    return root;
}

static cJSON *text_content(const char *text)
{
    cJSON *content = cJSON_CreateObject();
    if (content == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(content, "type", "text");
    cJSON_AddStringToObject(content, "text", text != NULL ? text : "");
    return content;
}

static cJSON *music_status_json(void)
{
    music_player_snapshot_t snapshot = {0};
    music_player_status_snapshot(&snapshot);

    cJSON *status = cJSON_CreateObject();
    if (status == NULL) {
        return NULL;
    }
    cJSON_AddBoolToObject(status, "active", snapshot.active);
    cJSON_AddBoolToObject(status, "stop_requested", snapshot.stop_requested);
    cJSON_AddStringToObject(status, "state", music_player_state_name(snapshot.state));
    cJSON_AddNumberToObject(status, "index", (double)snapshot.index);
    cJSON_AddStringToObject(status, "song_name", snapshot.song_name);
    cJSON_AddStringToObject(status, "artist_name", snapshot.artist_name);
    cJSON_AddStringToObject(status, "title", snapshot.title);
    cJSON_AddStringToObject(status, "album", snapshot.album);
    cJSON_AddStringToObject(status, "picture", snapshot.picture);
    cJSON_AddStringToObject(status, "last_error", snapshot.last_error);
    return status;
}

static esp_err_t mcp_finalize_json(cJSON *root, char **response_json)
{
    if (root == NULL || response_json == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t size = MCP_RESPONSE_BUFFER_INITIAL; size <= MCP_RESPONSE_BUFFER_MAX; size *= 2) {
        char *buffer = heap_caps_calloc(size, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buffer == NULL) {
            continue;
        }
        if (cJSON_PrintPreallocated(root, buffer, (int)size, false)) {
            cJSON_Delete(root);
            *response_json = buffer;
            return ESP_OK;
        }
        free(buffer);
    }

    *response_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *response_json != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t format_play_started_response(const cJSON *id, const char *detail, char **response_json)
{
    if (response_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *response_json = NULL;

    char id_json[96] = {0};
    esp_err_t err = format_id_json(id, id_json, sizeof(id_json));
    if (err != ESP_OK) {
        return err;
    }

    char detail_json[320] = {0};
    strlcpy(detail_json, "\"", sizeof(detail_json));
    json_escape_append(detail_json, sizeof(detail_json), detail != NULL ? detail : "Music started.");
    append_text(detail_json, sizeof(detail_json), "\"");
    size_t detail_len = strnlen(detail_json, sizeof(detail_json));
    if (detail_len == 0 || detail_json[detail_len - 1] != '"') {
        return ESP_ERR_NO_MEM;
    }

    char *buffer = heap_caps_calloc(MCP_RESPONSE_BUFFER_INITIAL, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int written = snprintf(
        buffer,
        MCP_RESPONSE_BUFFER_INITIAL,
        "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":[{\"type\":\"text\",\"text\":%s}],\"isError\":false}}",
        id_json,
        detail_json);
    if (written <= 0 || written >= MCP_RESPONSE_BUFFER_INITIAL) {
        free(buffer);
        return ESP_ERR_NO_MEM;
    }

    *response_json = buffer;
    return ESP_OK;
}

static esp_err_t handle_initialize(const cJSON *id, char **response_json)
{
    cJSON *root = mcp_base_response(id);
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *result = cJSON_AddObjectToObject(root, "result");
    cJSON *capabilities = cJSON_AddObjectToObject(result, "capabilities");
    cJSON *tools = cJSON_AddObjectToObject(capabilities, "tools");
    cJSON *server_info = cJSON_AddObjectToObject(result, "serverInfo");
    cJSON_AddStringToObject(result, "protocolVersion", MCP_PROTOCOL_VERSION);
    cJSON_AddBoolToObject(tools, "listChanged", false);
    cJSON_AddStringToObject(server_info, "name", MCP_SERVER_NAME);
    cJSON_AddStringToObject(server_info, "version", MCP_SERVER_VERSION);
    return mcp_finalize_json(root, response_json);
}

static cJSON *build_tools_array(void)
{
    cJSON *tools = cJSON_CreateArray();
    if (tools == NULL) {
        return NULL;
    }

    cJSON *play_song = cJSON_CreateObject();
    cJSON *play_schema = cJSON_AddObjectToObject(play_song, "inputSchema");
    cJSON *play_properties = cJSON_AddObjectToObject(play_schema, "properties");
    cJSON_AddStringToObject(play_song, "name", "self.music.play_song");
    cJSON_AddStringToObject(play_song, "title", "Play Song");
    cJSON_AddStringToObject(play_song, "description", "Play a song by resolving metadata and starting direct device-side streaming.");
    cJSON_AddStringToObject(play_schema, "type", "object");
    cJSON *song_name = cJSON_AddObjectToObject(play_properties, "song_name");
    cJSON_AddStringToObject(song_name, "type", "string");
    cJSON_AddStringToObject(song_name, "description", "Song title to play.");
    cJSON *artist_name = cJSON_AddObjectToObject(play_properties, "artist_name");
    cJSON_AddStringToObject(artist_name, "type", "string");
    cJSON_AddStringToObject(artist_name, "description", "Optional artist name.");
    cJSON_AddItemToArray(tools, play_song);

    cJSON *stop = cJSON_CreateObject();
    cJSON *stop_schema = cJSON_AddObjectToObject(stop, "inputSchema");
    cJSON_AddStringToObject(stop, "name", "self.music.stop");
    cJSON_AddStringToObject(stop, "title", "Stop Music");
    cJSON_AddStringToObject(stop, "description", "Request the current local music playback task to stop.");
    cJSON_AddStringToObject(stop_schema, "type", "object");
    cJSON_AddObjectToObject(stop_schema, "properties");
    cJSON_AddItemToArray(tools, stop);

    cJSON *get_status = cJSON_CreateObject();
    cJSON *status_schema = cJSON_AddObjectToObject(get_status, "inputSchema");
    cJSON_AddStringToObject(get_status, "name", "self.music.get_status");
    cJSON_AddStringToObject(get_status, "title", "Get Music Status");
    cJSON_AddStringToObject(get_status, "description", "Return the current local music playback state.");
    cJSON_AddStringToObject(status_schema, "type", "object");
    cJSON_AddObjectToObject(status_schema, "properties");
    cJSON_AddItemToArray(tools, get_status);

    return tools;
}

static esp_err_t handle_tools_list(const cJSON *id, char **response_json)
{
    cJSON *root = mcp_base_response(id);
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *result = cJSON_AddObjectToObject(root, "result");
    cJSON *tools = build_tools_array();
    if (result == NULL || tools == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(result, "tools", tools);
    return mcp_finalize_json(root, response_json);
}

static esp_err_t add_call_result(cJSON *root, bool is_error, const char *text, cJSON *structured)
{
    if (root == NULL) {
        cJSON_Delete(structured);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *result = cJSON_AddObjectToObject(root, "result");
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    if (result == NULL || content == NULL) {
        cJSON_Delete(structured);
        return ESP_ERR_NO_MEM;
    }
    cJSON *block = text_content(text);
    if (block == NULL) {
        cJSON_Delete(structured);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToArray(content, block);
    cJSON_AddBoolToObject(result, "isError", is_error);
    if (structured != NULL) {
        cJSON_AddItemToObject(result, "structuredContent", structured);
    }
    return ESP_OK;
}

static esp_err_t handle_play_song_call(
    const cJSON *arguments,
    const ornament_settings_t *settings,
    cJSON *root,
    char *started_detail,
    size_t started_detail_size)
{
    const cJSON *song_name = cJSON_GetObjectItemCaseSensitive((cJSON *)arguments, "song_name");
    const cJSON *artist_name = cJSON_GetObjectItemCaseSensitive((cJSON *)arguments, "artist_name");
    if (!cJSON_IsString(song_name) || song_name->valuestring == NULL || song_name->valuestring[0] == '\0') {
        return add_call_result(root, true, "song_name is required", music_status_json());
    }

    esp_err_t err = settings != NULL ?
        music_player_play_song_with_settings(
            song_name->valuestring,
            cJSON_IsString(artist_name) ? artist_name->valuestring : NULL,
            1,
            settings) :
        music_player_play_song(
            song_name->valuestring,
            cJSON_IsString(artist_name) ? artist_name->valuestring : NULL,
            1);
    music_player_snapshot_t snapshot = {0};
    music_player_status_snapshot(&snapshot);

    char detail[256];
    if (err == ESP_OK) {
        detail[0] = '\0';
        append_text(detail, sizeof(detail), "Music started: ");
        append_text(detail, sizeof(detail), snapshot.song_name);
        if (snapshot.artist_name[0] != '\0') {
            append_text(detail, sizeof(detail), " - ");
            append_text(detail, sizeof(detail), snapshot.artist_name);
        }
        append_text(detail, sizeof(detail), ".");
        if (started_detail != NULL && started_detail_size > 0) {
            strlcpy(started_detail, detail, started_detail_size);
        }
        return ESP_OK;
    }

    snprintf(detail, sizeof(detail), "Failed to start music playback: %s", esp_err_to_name(err));
    return add_call_result(root, true, detail, NULL);
}

static esp_err_t handle_stop_call(cJSON *root)
{
    music_player_request_stop();
    return add_call_result(root, false, "Stop requested for music playback.", music_status_json());
}

static esp_err_t handle_status_call(cJSON *root)
{
    return add_call_result(root, false, "Current music playback status.", music_status_json());
}

static esp_err_t handle_tools_call(
    const cJSON *id,
    const cJSON *params,
    const ornament_settings_t *settings,
    char **response_json)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive((cJSON *)params, "name");
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive((cJSON *)params, "arguments");
    if (!cJSON_IsString(name) || name->valuestring == NULL) {
        return mcp_finalize_json(mcp_error_response(id, -32602, "Missing tool name"), response_json);
    }
    if (arguments != NULL && !cJSON_IsObject(arguments)) {
        return mcp_finalize_json(mcp_error_response(id, -32602, "Tool arguments must be an object"), response_json);
    }

    cJSON *root = mcp_base_response(id);
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    char play_started_detail[256] = {0};
    if (strcmp(name->valuestring, "self.music.play_song") == 0) {
        err = handle_play_song_call(arguments, settings, root, play_started_detail, sizeof(play_started_detail));
    } else if (strcmp(name->valuestring, "self.music.stop") == 0) {
        err = handle_stop_call(root);
    } else if (strcmp(name->valuestring, "self.music.get_status") == 0) {
        err = handle_status_call(root);
    } else {
        cJSON_Delete(root);
        return mcp_finalize_json(mcp_error_response(id, -32601, "Unknown tool"), response_json);
    }
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }
    if (play_started_detail[0] != '\0') {
        cJSON_Delete(root);
        return format_play_started_response(id, play_started_detail, response_json);
    }
    return mcp_finalize_json(root, response_json);
}

esp_err_t xiaozhi_mcp_init(void)
{
    return music_player_init();
}

esp_err_t xiaozhi_mcp_handle_request(
    const cJSON *payload,
    const ornament_settings_t *settings,
    char **response_json)
{
    if (response_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *response_json = NULL;

    if (!cJSON_IsObject(payload)) {
        return mcp_finalize_json(mcp_error_response(NULL, -32600, "Invalid request"), response_json);
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "id");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "method");
    if (!cJSON_IsString(method) || method->valuestring == NULL) {
        return mcp_finalize_json(mcp_error_response(id, -32600, "Missing method"), response_json);
    }

    if (strcmp(method->valuestring, "notifications/initialized") == 0) {
        return ESP_OK;
    }
    if (strcmp(method->valuestring, "initialize") == 0) {
        return handle_initialize(id, response_json);
    }
    if (strcmp(method->valuestring, "tools/list") == 0) {
        return handle_tools_list(id, response_json);
    }
    if (strcmp(method->valuestring, "tools/call") == 0) {
        const cJSON *params = cJSON_GetObjectItemCaseSensitive((cJSON *)payload, "params");
        if (params != NULL && !cJSON_IsObject(params)) {
            return mcp_finalize_json(mcp_error_response(id, -32602, "params must be an object"), response_json);
        }
        return handle_tools_call(id, params, settings, response_json);
    }

    return mcp_finalize_json(mcp_error_response(id, -32601, "Method not found"), response_json);
}

#else

esp_err_t xiaozhi_mcp_init(void)
{
    return ESP_OK;
}

esp_err_t xiaozhi_mcp_handle_request(
    const cJSON *payload,
    const ornament_settings_t *settings,
    char **response_json)
{
    (void)payload;
    (void)settings;
    if (response_json != NULL) {
        *response_json = NULL;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
