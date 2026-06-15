/*
 * Bing daily wallpaper fetch.
 * Pulls the day's image metadata and downloads the 1280x720 JPEG to a file.
 */
#pragma once
#include <string>

namespace screensaver {

// Download today's Bing wallpaper (1280x720) to `outJpgPath`.
// Blocking (network + flash IO) — call from a worker thread, never the UI loop.
// Returns true on success.
bool fetchTodayWallpaper(const std::string& outJpgPath);

// Local calendar day as "YYYYMMDD", used to refresh at most once per day.
std::string todayKey();

}  // namespace screensaver
