#pragma once
// Open-Meteo 公网直连天气 (参考 oneapp fetchers.py/dash.py 同款方案).
// 2026-11-15 主页天气去 OrangePi 化: 原走 Pi 上 HA 的
// weather.he_feng_tian_qi_heweather 实体 (REST + token), 改为设备直连:
//   定位  — ip-api.com (http, 中文省市+经纬度一步到位) → api.ip.sb/geoip
//           (https 备用) → 失败回落深圳 (oneapp WX_HOME 同款);
//   天气  — api.open-meteo.com 免 key API, 单请求覆盖当前+未来 6 天
//           (卡片 温度/天气 + 详情弹窗 湿度/露点/紫外线/云量/风/气压/预报)。
// 家庭网络实测 (2026-11-15): ip-api ~0.3s / open-meteo 1.4s, 字段全齐。
#include <string>
#include <cstdio>

namespace open_meteo {

// 定位失败回落 (oneapp WX_HOME 同款)
inline constexpr double HOME_LAT = 22.5629;
inline constexpr double HOME_LON = 114.1207;
inline constexpr const char* HOME_CITY = "深圳";

// WMO weather code → 中文 (oneapp wx_code_zh / car-dash wxCodeZh 同表)
inline const char* codeZh(int c)
{
    switch (c) {
    case 0: case 1:              return "晴";
    case 2:                      return "多云";
    case 3:                      return "阴";
    case 45: case 48:            return "雾";
    case 51: case 53: case 55:   return "小雨";
    case 56: case 57:            return "冻雨";
    case 61:                     return "小雨";
    case 63:                     return "中雨";
    case 65: case 66: case 67:   return "大雨";
    case 71:                     return "小雪";
    case 73:                     return "中雪";
    case 75:                     return "大雪";
    case 77:                     return "雪粒";
    case 80: case 81:            return "阵雨";
    case 82:                     return "强阵雨";
    case 85: case 86:            return "阵雪";
    case 95:                     return "雷阵雨";
    case 96: case 99:            return "雷雨";
    default:                     return "";
    }
}

// 风向 (度, 北=0 顺时针) → 中文方位 (旧 HA 版 wind_bearing 给 "东南风", 同形态)
inline std::string dirZh(int deg)
{
    static const char* s[8] = {"北", "东北", "东", "东南", "南", "西南", "西", "西北"};
    int idx = (((deg % 360) + 360) % 360 + 22) / 45;
    if (idx > 7) idx = 7;
    return std::string(s[idx]) + "风";
}

// 单请求 = 当前实况 (卡片 + 详情头三行) + 未来 6 天预报 (详情弹窗)
// (缓冲 400: GCC format-truncation 静态分析按 %.4f 最坏展开算, 280 不够)
inline std::string forecastUrl(double lat, double lon)
{
    char url[400];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,dew_point_2m,weather_code,"
             "cloud_cover,pressure_msl,wind_speed_10m,wind_direction_10m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,uv_index_max"
             "&timezone=auto&forecast_days=6",
             lat, lon);
    return url;
}

// IP 归属地定位 (① ip-api http 中文; ② ip.sb https 备用 — 调用方顺序尝试)
inline const char* LOCATE_URL_1 =
    "http://ip-api.com/json/?lang=zh-CN&fields=status,city,regionName,lat,lon";
inline constexpr const char* LOCATE_URL_2 = "https://api.ip.sb/geoip";

// 坐标 → NVS 字符串 (%.4f, oneapp URL 同精度)
inline std::string coordStr(double v)
{
    char b[24];
    snprintf(b, sizeof(b), "%.4f", v);
    return b;
}

}  // namespace open_meteo
