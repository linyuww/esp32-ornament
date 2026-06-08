#include "xiaozhi_mcp.h"

#include "music_player.h"

#include "esp_check.h"

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
    *response_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *response_json != NULL ? ESP_OK : ESP_ERR_NO_MEM;
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
    cJSON *play_required = cJSON_AddArrayToObject(play_schema, "required");
    cJSON_AddStringToObject(play_song, "name", "self.music.play_song");
    cJSON_AddStringToObject(play_song, "title", "Play Song");
    cJSON_AddStringToObject(play_song, "description", "Resolve and start local music playback through the ornament bridge.");
    cJSON_AddStringToObject(play_schema, "type", "object");
    cJSON_AddItemToArray(play_required, cJSON_CreateString("song_name"));
    cJSON *song_name = cJSON_AddObjectToObject(play_properties, "song_name");
    cJSON_AddStringToObject(song_name, "type", "string");
    cJSON_AddStringToObject(song_name, "description", "Song title to search on NetEase.");
    cJSON *artist_name = cJSON_AddObjectToObject(play_properties, "artist_name");
    cJSON_AddStringToObject(artist_name, "type", "string");
    cJSON_AddStringToObject(artist_name, "description", "Optional artist filter.");
    cJSON *index = cJSON_AddObjectToObject(play_properties, "index");
    cJSON_AddStringToObject(index, "type", "integer");
    cJSON_AddNumberToObject(index, "minimum", 1);
    cJSON_AddStringToObject(index, "description", "Optional Yaohud result index, defaults to 1.");
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

static esp_err_t handle_play_song_call(const cJSON *arguments, cJSON *root)
{
    const cJSON *song_name = cJSON_GetObjectItemCaseSensitive((cJSON *)arguments, "song_name");
    const cJSON *artist_name = cJSON_GetObjectItemCaseSensitive((cJSON *)arguments, "artist_name");
    const cJSON *index = cJSON_GetObjectItemCaseSensitive((cJSON *)arguments, "index");
    if (!cJSON_IsString(song_name) || song_name->valuestring == NULL || song_name->valuestring[0] == '\0') {
        return add_call_result(root, true, "song_name is required", music_status_json());
    }

    uint32_t selected_index = 1;
    if (cJSON_IsNumber(index) && index->valuedouble >= 1.0) {
        selected_index = (uint32_t)index->valuedouble;
    }

    esp_err_t err = music_player_play_song(
        song_name->valuestring,
        cJSON_IsString(artist_name) ? artist_name->valuestring : NULL,
        selected_index);
    music_player_snapshot_t snapshot = {0};
    music_player_status_snapshot(&snapshot);

    char detail[256];
    if (err == ESP_OK) {
        detail[0] = '\0';
        append_text(detail, sizeof(detail), "Starting local playback for ");
        append_text(detail, sizeof(detail), snapshot.song_name);
        if (snapshot.artist_name[0] != '\0') {
            append_text(detail, sizeof(detail), " - ");
            append_text(detail, sizeof(detail), snapshot.artist_name);
        }
        append_text(detail, sizeof(detail), ".");
        return add_call_result(root, false, detail, music_status_json());
    }

    snprintf(detail, sizeof(detail), "Failed to start music playback: %s", esp_err_to_name(err));
    return add_call_result(root, true, detail, music_status_json());
}

static esp_err_t handle_stop_call(cJSON *root)
{
    music_player_request_stop();
    return add_call_result(root, false, "Stop requested for local music playback.", music_status_json());
}

static esp_err_t handle_status_call(cJSON *root)
{
    return add_call_result(root, false, "Local music playback status.", music_status_json());
}

static esp_err_t handle_tools_call(const cJSON *id, const cJSON *params, char **response_json)
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
    if (strcmp(name->valuestring, "self.music.play_song") == 0) {
        err = handle_play_song_call(arguments, root);
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
    return mcp_finalize_json(root, response_json);
}

esp_err_t xiaozhi_mcp_init(void)
{
    return music_player_init();
}

esp_err_t xiaozhi_mcp_handle_request(const cJSON *payload, char **response_json)
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
        return handle_tools_call(id, params, response_json);
    }

    return mcp_finalize_json(mcp_error_response(id, -32601, "Method not found"), response_json);
}

#else

esp_err_t xiaozhi_mcp_init(void)
{
    return ESP_OK;
}

esp_err_t xiaozhi_mcp_handle_request(const cJSON *payload, char **response_json)
{
    (void)payload;
    if (response_json != NULL) {
        *response_json = NULL;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
