file(GLOB_RECURSE APP_LAYER_SRCS
    ../../../app/*.c
    ../../../app/*.cc
    ../../../app/*.cpp
)

set(APP_LAYER_INCS
    ../../../app
)

file(GLOB_RECURSE MY_HAL_SRCS
    ./hal/*.c
    ./hal/*.cc
    ./hal/*.cpp
)

idf_component_register(SRCS "app_main.cpp" ${APP_LAYER_SRCS} ${MY_HAL_SRCS}
                    INCLUDE_DIRS "." ${APP_LAYER_INCS}
                    EMBED_TXTFILES "../audio/canon_in_d.mp3" "../audio/startup_sfx.mp3" "../audio/shutdown_sfx.mp3"
                    REQUIRES
                        mooncake
                        mooncake_log
                        lvgl
                        smooth_ui_toolkit
                        m5stack_tab5
                        esp_netif
                        nvs_flash
                        esp_wifi
                        esp_timer
                        esp_event
                        power_monitor_ina226
                        sensor_bmi270
                        sensor_icm20602
                        imlib
                        esp_video
                        keypad_scanner_tca8418
                        chmorgan__esp-audio-player
                        espressif__usb_host_hid
                        usb
                        78__esp-ml307
                        espressif__esp-sr
                        espressif__led_strip
                    PRIV_REQUIRES
                        xiaozhi_core
                        78__xiaozhi-fonts
                        esp_http_client
                        esp_pm
                        voice_input)

# LVGL assert handler: abort() instead of while(1) so crashes restart cleanly
target_compile_definitions(__idf_main PRIVATE
    "LV_ASSERT_HANDLER=abort();"
)
