# pillscale — build for ESP32-C3, ESP-IDF 6.1.
#
#   idf.py build flash monitor
#
# Pins and options are chosen in `idf.py menuconfig` → pillscale
# (src/Kconfig.projbuild). Defaults are in sdkconfig.defaults; commands in README.md.

cmake_minimum_required(VERSION 3.22)

set(PROJECT_VER "1.0.0")

# Firmware sources live in src/, not in the standard main/: src/ is a component.
set(EXTRA_COMPONENT_DIRS src)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pillscale)
