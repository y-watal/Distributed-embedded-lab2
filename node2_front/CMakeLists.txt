cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(node2_front)

target_sources(app PRIVATE
    # shared with node3_rear (identical files)
    src/main.c
    src/blink.c
    src/button.c
    src/canbus.c
    src/ctrl.c
    src/current_sensor.c
    src/evlog.c
    src/hb.c
    src/relay.c
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
