# Host build: unit tests, the headless core monitor, and the LVGL GUI simulator.
#
# The ESP32 firmware is built separately with idf.py from port/esp32 - it reuses
# core/ and ui/ verbatim, which is the point of keeping them platform-free.
cmake_minimum_required(VERSION 3.20)
project(lier_bms_scherm C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

# Which way Daly's current points - see DALY_CURRENT_SIGN in core/daly_proto.h.
# Settled at bring-up: under load the current must read negative.
option(BMS_INVERT_CURRENT "Daly reports discharge above the 30000 bias" OFF)
if(BMS_INVERT_CURRENT)
    set(BMS_CURRENT_SIGN -1)
    set(BMS_OTHER_SIGN    1)
else()
    set(BMS_CURRENT_SIGN  1)
    set(BMS_OTHER_SIGN   -1)
endif()

# ---------------------------------------------------------------- core (portable)
set(BMS_CORE_SOURCES
    core/daly_proto.c
    core/bms_model.c
    core/poller.c
    core/warnings.c
    core/virtual_bms.c
)
add_library(bms_core STATIC ${BMS_CORE_SOURCES})
target_include_directories(bms_core PUBLIC core)
target_compile_definitions(bms_core PUBLIC DALY_CURRENT_SIGN=${BMS_CURRENT_SIGN})

# ---------------------------------------------------------------- tests
enable_testing()
add_executable(test_core tests/test_core.c)
target_link_libraries(test_core PRIVATE bms_core)
add_test(NAME core COMMAND test_core)

# The same tests against the opposite current sign. Only one setting is in use
# at a time, and the other would otherwise never be built.
add_library(bms_core_other_sign STATIC ${BMS_CORE_SOURCES})
target_include_directories(bms_core_other_sign PUBLIC core)
target_compile_definitions(bms_core_other_sign
    PUBLIC DALY_CURRENT_SIGN=${BMS_OTHER_SIGN})
add_executable(test_core_other_sign tests/test_core.c)
target_link_libraries(test_core_other_sign PRIVATE bms_core_other_sign)
add_test(NAME core_other_sign COMMAND test_core_other_sign)

# ---------------------------------------------------------------- headless monitor
add_executable(sim_headless
    port/sim/main_headless.c
    port/sim/can_socketcan.c
)
target_include_directories(sim_headless PRIVATE port/sim)
target_link_libraries(sim_headless PRIVATE bms_core)
target_compile_definitions(sim_headless PRIVATE _GNU_SOURCE)

# ---------------------------------------------------------------- GUI simulator
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
    endif()
endif()

if(SDL2_FOUND)
    include(FetchContent)
    FetchContent_Declare(lvgl
        GIT_REPOSITORY https://github.com/lvgl/lvgl.git
        GIT_TAG        v9.5.0
        GIT_SHALLOW    TRUE
    )
    # Our lv_conf.h lives with the simulator port. LVGL 9.5 takes a directory
    # here; older 9.x took a file path in LV_CONF_PATH, so set both.
    set(LV_BUILD_CONF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/port/sim CACHE STRING "" FORCE)
    set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/port/sim/lv_conf.h
        CACHE STRING "" FORCE)
    FetchContent_MakeAvailable(lvgl)

    add_executable(sim
        port/sim/main_sim.c
        port/sim/sdl_display.c
        port/sim/can_socketcan.c
        ui/fonts/fixed_6x13.c
        ui/fonts/fixed_6x13b.c
        ui/fonts/fixed_7x14.c
        ui/fonts/fixed_7x14b.c
        ui/fonts/fixed_9x15.c
        ui/fonts/fixed_9x15b.c
        ui/fonts/fixed_10x20.c
        ui/fonts/fixed_10x20b.c
        ui/fonts/fixed_6x13b_2x.c
        ui/ui.c
        ui/page_overview.c
        ui/page_pack.c
        ui/page_cells.c
    )
    target_include_directories(sim PRIVATE port/sim ui ui/fonts)
    target_link_libraries(sim PRIVATE bms_core lvgl)
    target_compile_definitions(sim PRIVATE _GNU_SOURCE LV_LVGL_H_INCLUDE_SIMPLE)
    if(TARGET PkgConfig::SDL2)
        target_link_libraries(sim PRIVATE PkgConfig::SDL2)
    else()
        target_include_directories(sim PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(sim PRIVATE ${SDL2_LIBRARIES})
    endif()
    target_link_libraries(sim PRIVATE m)
else()
    message(WARNING "SDL2 not found - the GUI simulator will not be built. "
                    "Install it with: sudo apt install libsdl2-dev")
endif()
