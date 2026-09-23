set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)

idf_component_register(
    SRCS
        "main.c"
        "board.c"
        "st7305.c"
        "twai_link.c"
        "${REPO_ROOT}/core/daly_proto.c"
        "${REPO_ROOT}/core/bms_model.c"
        "${REPO_ROOT}/core/poller.c"
        "${REPO_ROOT}/core/warnings.c"
        "${REPO_ROOT}/ui/fonts/fixed_6x13.c"
        "${REPO_ROOT}/ui/fonts/fixed_6x13b.c"
        "${REPO_ROOT}/ui/fonts/fixed_7x14.c"
        "${REPO_ROOT}/ui/fonts/fixed_7x14b.c"
        "${REPO_ROOT}/ui/fonts/fixed_9x15.c"
        "${REPO_ROOT}/ui/fonts/fixed_9x15b.c"
        "${REPO_ROOT}/ui/fonts/fixed_10x20.c"
        "${REPO_ROOT}/ui/fonts/fixed_6x13b_2x.c"
        "${REPO_ROOT}/ui/ui.c"
        "${REPO_ROOT}/ui/page_overview.c"
        "${REPO_ROOT}/ui/page_pack.c"
        "${REPO_ROOT}/ui/page_cells.c"
    INCLUDE_DIRS
        "."
        "${REPO_ROOT}/core"
        "${REPO_ROOT}/ui"
        "${REPO_ROOT}/ui/fonts"
    REQUIRES
        driver esp_driver_gpio esp_driver_spi esp_driver_twai esp_lcd esp_timer
)

# lv_font_conv emits `#include "lvgl/lvgl.h"` unless this is defined; under
# ESP-IDF the header is on the include path as plain "lvgl.h".
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
