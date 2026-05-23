cmake_minimum_required(VERSION 3.16)

set(CCACHE_ENABLE 0 CACHE BOOL "Disable ccache to avoid ESP-IDF 5.4.1 xtensa GCC ICE on Windows")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(codex_ornament)
