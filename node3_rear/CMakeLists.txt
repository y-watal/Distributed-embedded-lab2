cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(node3_rear)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared)

target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/raft_agent.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/raft_transport.c
    # shared with node2_front (identical files)
    src/main.c
    src/blink.c
    src/button.c
    src/canbus.c
    src/ctrl.c
    src/current_sensor.c
    src/evlog.c
    src/hb.c
    src/relay.c
    src/raft_node.c
    src/report.c
    src/tp.c
    src/wcet.c
    src/jitter.c
    src/watchdog.c
    src/zone.c
    # N3 only: drivetrain zone
    src/motor.c
    src/encoder.c
    src/drive.c
)
