set(ORNAMENT_SRCS
  "app_main.c"
  "asrpro_link.c"
  "bridge_client.c"
  "device_identity.c"
  "ornament_state.c"
  "settings.c"
  "system_status.c"
  "task_audio.c"
  "music_player.c"
  "xiaozhi_client.c"
  "xiaozhi_mcp.c"
  "display.c"
  "display_core.c"
  "wifi.c"
)

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

set(ORNAMENT_REQUIRES
  driver
  esp_driver_i2s
  esp-tls
  esp_http_client
  esp_lcd
  esp_netif
  esp_timer
  esp_wifi
  json
  lwip
  mbedtls
  nvs_flash
)

if(CONFIG_ORNAMENT_CONFIG_PORTAL_ENABLED OR CONFIG_ORNAMENT_WEB_CONSOLE_ENABLED)
  list(APPEND ORNAMENT_REQUIRES esp_http_server)
endif()

if(CONFIG_ORNAMENT_MDNS_ENABLED)
  list(APPEND ORNAMENT_REQUIRES mdns)
endif()

if(CONFIG_ORNAMENT_XIAOZHI_ENABLED)
  list(APPEND ORNAMENT_REQUIRES esp_websocket_client esp-opus)
endif()

if(CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED)
  list(APPEND ORNAMENT_REQUIRES lvgl 78__xiaozhi-fonts)
endif()

idf_component_register(
  SRCS ${ORNAMENT_SRCS}
  INCLUDE_DIRS "."
  EMBED_FILES
    "assets/task_done.pcm"
  REQUIRES ${ORNAMENT_REQUIRES}
)
