# ESP-IDF project for the ESP32-S3-RLCD-4.2.
#
#   . ~/esp/esp-idf/export.sh
#   cd port/esp32
#   idf.py set-target esp32s3
#   idf.py build flash monitor
#
# core/ and ui/ are pulled in from the repository root unchanged - the same
# sources the host simulator builds.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lier_bms_scherm)
