cmake_minimum_required(VERSION 3.16)
project(psk_lcd_preview C)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(LVGL "${REPO}/managed_components/lvgl__lvgl")
file(GLOB_RECURSE LVGL_SOURCES "${LVGL}/src/*.c")
add_executable(lcd_preview preview.c "${REPO}/main/psk_modes.c"
    "${REPO}/main/ui_theme.c" "${REPO}/main/ui_i18n.c" "${REPO}/main/ui_font_14.c" "${REPO}/main/ui_font_18.c" "${REPO}/main/ui_font_26.c" "${REPO}/main/sdr_presets.c" "${REPO}/main/sdr_map.c" "${REPO}/main/sdr_scan.c"
    "${REPO}/main/modem_image.c" "${REPO}/main/spectrum.c" "${REPO}/main/sdr_types.c" ${LVGL_SOURCES})
target_include_directories(lcd_preview PRIVATE stubs . "${REPO}/main"
    "${REPO}/components/psk_rtty/include" "${LVGL}")
target_compile_definitions(lcd_preview PRIVATE LV_CONF_INCLUDE_SIMPLE LV_KCONFIG_IGNORE)
target_compile_options(lcd_preview PRIVATE -O1)
target_link_libraries(lcd_preview PRIVATE m)
