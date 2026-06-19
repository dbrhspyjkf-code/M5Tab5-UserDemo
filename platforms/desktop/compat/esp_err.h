// Desktop stub for <esp_err.h> from ESP-IDF. Just enough for app_unit_puzzle's
// RMT init path — esp_err_t is widened to int, ESP_OK is 0, esp_err_to_name
// returns a stable "0" so log lines stay readable.

#pragma once
typedef int esp_err_t;
#define ESP_OK          0
#define ESP_FAIL        -1
#define ESP_ERR_NO_MEM  0x101
inline const char* esp_err_to_name(int) { return "0"; }
