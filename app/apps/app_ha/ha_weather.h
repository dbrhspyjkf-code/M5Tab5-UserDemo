#pragma once
#include <string>
#include <hal/hal.h>

// secrets.h is gitignored (see .gitignore + secrets.h.example) — a local-only
// compile-time fallback for the HA token, since it's too long to type on the
// on-screen keyboard. Falls back to "" if the file doesn't exist (fresh
// clone), which just means ha_token must be set via 主屏 → 网络设置 instead.
#if __has_include("secrets.h")
#include "secrets.h"
#else
inline const char* LOCAL_HA_TOKEN_DEFAULT = "";
#endif

// Shared Home Assistant access for weather, used by both the HA app (app_ha)
// and the home status bar (app_home). Weather now comes straight from HA's
// weather.* entity instead of the Mac-side Hermes :8766 service.
namespace ha_weather {

// HA long-lived access token. Preferred path: NVS "ha_token", set via 主屏 →
// 网络设置 → HA Token. Falls back to LOCAL_HA_TOKEN_DEFAULT (secrets.h,
// gitignored) when NVS is empty — see that file for why this exists instead
// of just hardcoding it here again: a real token got committed to this exact
// spot once before (leaked to the public fork, since revoked).
inline std::string token() { return GetHAL()->getConfig("ha_token", LOCAL_HA_TOKEN_DEFAULT); }

// Local home forecast entity (和风天气 HeWeather, °C, has temperature + humidity).
inline const char* ENTITY = "weather.he_feng_tian_qi_heweather";

// HA weather condition (English state string) → short Chinese label.
inline std::string condZh(const std::string& s)
{
    if (s == "sunny")            return "晴";
    if (s == "clear-night")      return "晴";
    if (s == "partlycloudy")     return "多云";
    if (s == "cloudy")           return "阴";
    if (s == "fog")              return "雾";
    if (s == "rainy")            return "小雨";
    if (s == "pouring")          return "大雨";
    if (s == "lightning")        return "雷阵雨";
    if (s == "lightning-rainy")  return "雷雨";
    if (s == "snowy")            return "雪";
    if (s == "snowy-rainy")      return "雨夹雪";
    if (s == "hail")             return "冰雹";
    if (s == "windy")            return "大风";
    if (s == "windy-variant")    return "大风";
    if (s == "exceptional")      return "异常";
    return s;  // fallback: show raw state rather than nothing
}

}  // namespace ha_weather
