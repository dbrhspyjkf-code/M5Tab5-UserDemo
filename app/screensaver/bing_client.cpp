#include "bing_client.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <ctime>
#include <vector>

using json = nlohmann::json;
static const std::string _tag = "bing";

// Bing's metadata endpoint: idx=0 (today), n=1 (one image), zh-CN market.
static const char* BING_API =
    "http://www.bing.com/HPImageArchive.aspx?format=js&idx=0&n=1&mkt=zh-CN";
static const char* BING_HOST = "http://www.bing.com";

static const char* TMP_JSON = "/spiffs/bing.json";

std::string screensaver::todayKey()
{
    time_t t  = time(nullptr);
    tm*    lt = localtime(&t);
    char buf[16];
    strftime(buf, sizeof(buf), "%Y%m%d", lt);
    return buf;
}

// Read a small file fully into a string.
static bool _read_file(const std::string& path, std::string& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64 * 1024) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t rd = fread(&out[0], 1, (size_t)n, f);
    fclose(f);
    out.resize(rd);
    return rd > 0;
}

bool screensaver::fetchTodayWallpaper(const std::string& outJpgPath)
{
    auto* hal = GetHAL();
    if (!hal) return false;

    // 1) Fetch metadata JSON (to file, so the cert-bundle/redirect-following
    //    download path is reused — plain httpGet can't follow http→https).
    if (!hal->httpGetToFile(BING_API, TMP_JSON)) {
        mclog::tagWarn(_tag, "metadata fetch failed");
        return false;
    }
    std::string body;
    if (!_read_file(TMP_JSON, body)) {
        mclog::tagWarn(_tag, "metadata read failed");
        return false;
    }

    // 2) Parse: prefer urlbase ("/th?id=OHR.Foo_ZH-CN1234") + explicit size, so
    //    we get a screen-sized 1280x720 frame instead of the default 1920x1080.
    std::string image_path;
    try {
        auto j = json::parse(body);
        auto& img0 = j["images"][0];
        if (img0.contains("urlbase") && img0["urlbase"].is_string()) {
            image_path = img0["urlbase"].get<std::string>() + "_1280x720.jpg";
        } else if (img0.contains("url") && img0["url"].is_string()) {
            image_path = img0["url"].get<std::string>();
        }
    } catch (const std::exception& e) {
        mclog::tagWarn(_tag, "json parse error: {}", e.what());
        return false;
    }
    if (image_path.empty()) {
        mclog::tagWarn(_tag, "no image url in metadata");
        return false;
    }

    // 3) Download the JPEG.
    std::string full = std::string(BING_HOST) + image_path;
    mclog::tagInfo(_tag, "downloading {}", full);
    if (!hal->httpGetToFile(full, outJpgPath)) {
        mclog::tagWarn(_tag, "image download failed");
        return false;
    }
    remove(TMP_JSON);
    return true;
}
