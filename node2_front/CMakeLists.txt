cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(node2_front)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared)

# Lab 5 startup mode, same on all three nodes (CMake caches these: pass
# -DLEADER=cockpit -DSEED= to go back):
#   west build ... -- -DLEADER=random            election at boot
#   west build ... -- -DLEADER=random -DSEED=408  reproducible; the Pi wins
if(DEFINED LEADER AND LEADER STREQUAL "random")
  target_compile_definitions(app PRIVATE RAFT_DEFAULT_LEADER=-1)
endif()
if(DEFINED SEED AND NOT SEED STREQUAL "")
  target_compile_definitions(app PRIVATE RAFT_ELECTION_SEED=${SEED}u)
endif()

target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/raft_agent.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/raft_transport.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/timesync.c
    # shared with node3_rear (identical files)
    src/main.c
    src/blink.c
    src/button.c
    src/canbus.c
    src/ctrl.c
    src/current_sensor.c
    src/evlog.c
    src/gtime.c
    src/hb.c
    src/relay.c
    src/raft_node.c
    src/report.c
    src/tp.c
    src/wcet.c
    src/jitter.c
    src/watchdog.c
    src/zone.c
    # N2 only: steering zone
    src/servo.c
    src/steer.c
)
