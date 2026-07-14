# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(blinky)

target_sources(app PRIVATE src/main.c)
target_sources(app PRIVATE src/sentry_auth.c)
target_sources(app PRIVATE src/aux_uart.c)
target_sources_ifdef(CONFIG_BT_RAS_RRSP app PRIVATE src/cs_reflector.c)