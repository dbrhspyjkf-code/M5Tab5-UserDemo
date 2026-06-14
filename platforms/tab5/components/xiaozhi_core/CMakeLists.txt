# ─── xiaozhi_core — xiaozhi ESP32 embedded as a native component ─────────────
# Hardcoded for: ESP32-P4 (Tab5), Chinese language (zh-CN), Tab5BridgeBoard.
# Excludes: main.cc (app_main conflict), m5stack_tab5.cc (replaced by bridge).

set(XZ "${CMAKE_CURRENT_SOURCE_DIR}/xiaozhi-esp32/main")

# ── Core application sources ───────────────────────────────────────────────
set(SOURCES
    "${XZ}/application.cc"
    "${XZ}/mcp_server.cc"
    "${XZ}/system_info.cc"
    "${XZ}/ota.cc"
    "${XZ}/settings.cc"
    "${XZ}/device_state_machine.cc"
    "${XZ}/assets.cc"
)

# ── Audio ─────────────────────────────────────────────────────────────────
list(APPEND SOURCES
    "${XZ}/audio/audio_codec.cc"
    "${XZ}/audio/audio_service.cc"
    "${XZ}/audio/demuxer/ogg_demuxer.cc"
    "${XZ}/audio/codecs/no_audio_codec.cc"
    "${XZ}/audio/codecs/es8388_audio_codec.cc"
    "${XZ}/audio/codecs/dummy_audio_codec.cc"
    "${XZ}/audio/processors/no_audio_processor.cc"
    "${XZ}/audio/processors/audio_debugger.cc"
    "${XZ}/audio/wake_words/afe_wake_word.cc"
    "${XZ}/audio/wake_words/custom_wake_word.cc"
    # Tab5 real ES8388/ES7210 codec implementation
    "${XZ}/boards/m5stack-tab5/tab5_audio_codec.cc"
)

# ── Display ───────────────────────────────────────────────────────────────
list(APPEND SOURCES
    "${XZ}/display/display.cc"
    "${XZ}/display/lcd_display.cc"
    "${XZ}/display/oled_display.cc"
    "${XZ}/display/lvgl_display/lvgl_display.cc"
    "${XZ}/display/lvgl_display/emoji_collection.cc"
    "${XZ}/display/lvgl_display/lvgl_theme.cc"
    "${XZ}/display/lvgl_display/lvgl_font.cc"
    "${XZ}/display/lvgl_display/lvgl_image.cc"
    "${XZ}/display/lvgl_display/gif/lvgl_gif.cc"
    "${XZ}/display/lvgl_display/gif/gifdec.c"
    "${XZ}/display/lvgl_display/jpg/image_to_jpeg.cpp"
    "${XZ}/display/lvgl_display/jpg/jpeg_to_image.c"
)

# ── Protocols ─────────────────────────────────────────────────────────────
list(APPEND SOURCES
    "${XZ}/protocols/protocol.cc"
    "${XZ}/protocols/mqtt_protocol.cc"
    "${XZ}/protocols/websocket_protocol.cc"
)

# ── LED ───────────────────────────────────────────────────────────────────
list(APPEND SOURCES
    "${XZ}/led/single_led.cc"
    "${XZ}/led/gpio_led.cc"
)

# ── Board common ──────────────────────────────────────────────────────────
list(APPEND SOURCES
    "${XZ}/boards/common/board.cc"
    "${XZ}/boards/common/wifi_board.cc"
    "${XZ}/boards/common/backlight.cc"
    "${XZ}/boards/common/button.cc"
    "${XZ}/boards/common/i2c_device.cc"
    "${XZ}/boards/common/power_save_timer.cc"
    "${XZ}/boards/common/press_to_talk_mcp_tool.cc"
    "${XZ}/boards/common/sleep_timer.cc"
    "${XZ}/boards/common/system_reset.cc"
    "${XZ}/boards/common/afsk_demod.cc"
    "${XZ}/boards/common/adc_battery_monitor.cc"
)

# ── Our bridge files + emote stubs (EmoteDisplay vtable for dynamic_cast) ─
list(APPEND SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/tab5_bridge_lcd.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/tab5_bridge_board.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/emote_stubs.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/xiaozhi_ctl.cc"
)

# ── Embedded audio assets (zh-CN + common) ────────────────────────────────
file(GLOB LANG_SOUNDS   "${XZ}/assets/locales/zh-CN/*.ogg")
file(GLOB COMMON_SOUNDS "${XZ}/assets/common/*.ogg")

# Full-coverage Chinese chat font (GB2312 common ~3500 glyphs). The builtin
# font_puhui_basic only has a small subset, so LLM replies render as boxes.
# cbin font data is used in-place from flash (no big RAM copy).
set(CHAT_FONT_BIN "${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/78__xiaozhi-fonts/cbin/font_puhui_common_30_4.bin")

# ── Include directories ───────────────────────────────────────────────────
set(BSP_PRIV "${CMAKE_CURRENT_SOURCE_DIR}/../m5stack_tab5/priv_include")

set(INCLUDE_DIRS
    "${XZ}"
    "${XZ}/display"
    "${XZ}/display/lvgl_display"
    "${XZ}/display/lvgl_display/jpg"
    "${XZ}/audio"
    "${XZ}/audio/demuxer"
    "${XZ}/protocols"
    "${XZ}/boards/common"
    "${XZ}/boards/m5stack-tab5"
    "${CMAKE_CURRENT_SOURCE_DIR}"   # provides our gfx.h / expression_emote.h stubs
    "${BSP_PRIV}"                   # esp_lcd_st7123.h / esp_lcd_st7121.h for config.h
)

idf_component_register(
    SRCS            ${SOURCES}
    EMBED_FILES     ${LANG_SOUNDS} ${COMMON_SOUNDS} ${CHAT_FONT_BIN}
    INCLUDE_DIRS    ${INCLUDE_DIRS}
    WHOLE_ARCHIVE
    PRIV_REQUIRES
        esp_pm
        esp_psram
        esp_netif
        esp_wifi
        esp_driver_gpio
        esp_driver_uart
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_jpeg
        esp_driver_ppa
        esp_app_format
        app_update
        spi_flash
        bt
        efuse
        esp_video
        espressif__led_strip
        espressif__esp_lcd_ili9881c
        m5stack_tab5
)

# xiaozhi-esp32 uses dynamic_cast; re-enable RTTI for this component only
target_compile_options(${COMPONENT_LIB} PRIVATE -frtti)

# ── Compile-time board/feature flags ──────────────────────────────────────
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BOARD_TYPE="m5stack-tab5"
    BOARD_NAME="m5stack-tab5"
    BUILTIN_TEXT_FONT=font_puhui_basic_30_4
    BUILTIN_ICON_FONT=font_awesome_30_4
    CONFIG_LANGUAGE_ZH_CN=1
    CONFIG_BOARD_TYPE_M5STACK_CORE_TAB5=1
    CONFIG_USE_AUDIO_PROCESSOR=0
    CONFIG_IDF_TARGET_ESP32P4=1
)
