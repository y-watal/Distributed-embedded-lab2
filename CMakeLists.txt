cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lab2)

target_include_directories(app PRIVATE include)

target_sources(app PRIVATE
    src/main.c
    src/app_state.c
    src/tasks/communication.c
    src/drivers/motor.c
    src/drivers/current_sensor.c
    src/drivers/servo.c
    src/drivers/encoder.c
    src/drivers/led.c
)