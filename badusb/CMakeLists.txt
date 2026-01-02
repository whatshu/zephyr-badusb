# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

#
# Custom board root: allow using nologo_usb and nologo_usb2 boards
# from the project-local boards/ directory.
#
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

#
# Select Kconfig fragments by build type.
# - Debug: enable USB CDC + MSC + extra debug plumbing
# - Release: keep USB HID + flash-backed FS, but no CDC/MSC by default
#
if(NOT DEFINED NOLOGO_BUILD_FLAVOR)
  # Derive from CMAKE_BUILD_TYPE (case-insensitive). Default to release.
  string(TOLOWER "${CMAKE_BUILD_TYPE}" _nologo_bt_lc)
  if(_nologo_bt_lc STREQUAL "debug")
    set(NOLOGO_BUILD_FLAVOR "debug")
  else()
    set(NOLOGO_BUILD_FLAVOR "release")
  endif()
endif()

# IMPORTANT: Set CONF_FILE as a CACHE entry so Zephyr's build system will
# consistently pick it up even across re-configures.
if(NOLOGO_BUILD_FLAVOR STREQUAL "debug")
  set(CONF_FILE "prj.conf;prj.debug.conf" CACHE STRING "nologo config files" FORCE)
else()
  set(CONF_FILE "prj.conf;prj.release.conf" CACHE STRING "nologo config files" FORCE)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nologo_usb)

target_sources(app PRIVATE
  src/app_main.c
  src/nologo_config.c
  src/nologo_hid.c
  src/nologo_printk_router.c
  src/nologo_status_led.c
  src/nologo_usb.c
)

if(NOLOGO_BUILD_FLAVOR STREQUAL "debug")
  target_compile_definitions(app PRIVATE NOLOGO_DEBUG=1)
else()
  target_compile_definitions(app PRIVATE NOLOGO_DEBUG=0)
endif()
