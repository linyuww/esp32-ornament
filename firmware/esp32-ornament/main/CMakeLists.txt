set(ORNAMENT_SRCS
  "app_main.c"
  "asrpro_link.c"
  "bridge_client.c"
  "device_identity.c"
  "ornament_state.c"
  "ornament_http_client.c"
  "settings.c"
  "system_status.c"
  "task_audio.c"
  "music_player.c"
  "display.c"
  "display_core.c"
  "wifi.c"
)

if(CONFIG_ORNAMENT_XIAOZHI_TRANSPORT_DIRECT)
  list(APPEND ORNAMENT_SRCS "xiaozhi_client_direct.c" "xiaozhi_mcp.c")
else()
  list(APPEND ORNAMENT_SRCS "xiaozhi_client.c")
endif()

if(CONFIG_ORNAMENT_CONFIG_PORTAL_ENABLED)
  list(APPEND ORNAMENT_SRCS "config_portal.c")
endif()

if(CONFIG_ORNAMENT_LOCAL_WEATHER_ENABLED)
  list(APPEND ORNAMENT_SRCS "weather_client.c")
endif()

if(CONFIG_ORNAMENT_WEB_CONSOLE_ENABLED)
  list(APPEND ORNAMENT_SRCS "web_console.c" "system_diagnostics.c")
endif()

if(CONFIG_ORNAMENT_STANDBY_WALLPAPER_ENABLED)
  list(APPEND ORNAMENT_SRCS "assets/standby_wallpaper_rgb565.c")
endif()

if(CONFIG_ORNAMENT_TASK_DONE_AUDIO_CLIP_ENABLED)
  list(APPEND ORNAMENT_EMBED_FILES "assets/task_done.pcm")
endif()

set(ORNAMENT_REQUIRES
  driver
  esp_driver_i2s
  esp_http_server
  esp_lcd
  esp_netif
  esp_timer
  esp_wifi
  json
  lwip
  nvs_flash
)

if(CONFIG_ORNAMENT_LOCAL_WEATHER_ENABLED)
  list(APPEND ORNAMENT_REQUIRES esp-tls esp_http_client mbedtls)
endif()

if(CONFIG_ORNAMENT_MDNS_ENABLED)
  list(APPEND ORNAMENT_REQUIRES mdns)
endif()

if(CONFIG_ORNAMENT_XIAOZHI_TRANSPORT_DIRECT)
  list(APPEND ORNAMENT_REQUIRES esp-tls esp_http_client mbedtls esp_websocket_client esp-opus)
endif()

if(CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED)
  list(APPEND ORNAMENT_REQUIRES lvgl 78__xiaozhi-fonts)
endif()

if(ORNAMENT_REQUIRES)
  list(REMOVE_DUPLICATES ORNAMENT_REQUIRES)
endif()

idf_component_register(
  SRCS ${ORNAMENT_SRCS}
  INCLUDE_DIRS "."
  EMBED_FILES ${ORNAMENT_EMBED_FILES}
  REQUIRES ${ORNAMENT_REQUIRES}
)
