/*
 * Idle wallpaper screensaver.
 *
 * Driven globally from app::Update() (which already holds the LVGL lock). After
 * the screen has been idle past a threshold it loads a full-screen Bing daily
 * wallpaper with an overlaid clock; any touch dismisses it and restores the
 * previously active screen. The wallpaper is fetched + hardware-decoded on a
 * worker thread once per day and kept resident in PSRAM, so entering is instant.
 */
#pragma once
#include <cstdint>

namespace screensaver {

// Call once after the app framework is up (kicks the first wallpaper fetch).
void init();

// Call every frame with the global pointer-inactive time (ms). Must run with
// the LVGL lock held (app::Update does).
void tick(uint32_t inactive_ms);

// True while the wallpaper is showing — lets other input handlers (e.g. the HA
// swipe-up gesture) ignore the touch that is only meant to dismiss it.
bool isActive();

// Dismiss the screensaver now, e.g. when xiaozhi is voice-woken so the user
// sees the conversation. The overlay is torn down on the next tick().
void wake();

// Suppress the screensaver entirely (and dismiss it if showing). Used while the
// xiaozhi voice assistant is open so it never covers or competes with it.
void setInhibited(bool inhibited);

}  // namespace screensaver
