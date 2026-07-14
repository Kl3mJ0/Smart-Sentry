cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ss2_central)

target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_BT_RAS_RREQ app PRIVATE src/cs_initiator.c)
