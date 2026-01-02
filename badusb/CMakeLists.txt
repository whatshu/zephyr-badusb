# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Custom board root
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# Default nologo_usb2 to M33 core
if(BOARD STREQUAL "nologo_usb2")
  set(BOARD "nologo_usb2/rp2350a/m33" CACHE STRING "" FORCE)
endif()

# Build flavor: debug or release
if(NOT DEFINED NOLOGO_BUILD_FLAVOR)
  string(TOLOWER "${CMAKE_BUILD_TYPE}" _nologo_bt_lc)
  if(_nologo_bt_lc STREQUAL "debug")
    set(NOLOGO_BUILD_FLAVOR "debug")
  else()
    set(NOLOGO_BUILD_FLAVOR "release")
  endif()
endif()

if(NOLOGO_BUILD_FLAVOR STREQUAL "debug")
  set(CONF_FILE "prj.conf;prj.debug.conf" CACHE STRING "" FORCE)
else()
  set(CONF_FILE "prj.conf;prj.release.conf" CACHE STRING "" FORCE)
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
