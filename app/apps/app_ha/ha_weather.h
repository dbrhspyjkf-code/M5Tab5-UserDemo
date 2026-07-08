#pragma once
#include <string>

// Shared Home Assistant access for weather, used by both the HA app (app_ha)
// and the home status bar (app_home). Weather now comes straight from HA's
// weather.* entity instead of the Mac-side Hermes :8766 service.
namespace ha_weather {

// HA long-lived access token — set via Settings app (stored in NVS).
// Generate one in HA → Profile → Long-Lived Access Tokens.
inline const char* TOKEN = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiI0NWZkMTg4ZmUyMTQ0ODVmODI2YjljNGQxYzgxMjRhMyIsImlhdCI6MTc4MzIyNDI1MCwiZXhwIjoyMDk4NTg0MjUwfQ.YOGEndo1vSDVhpH2suQ6Mt2xG4EflX6BbVvSTeHk3Uw";

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
