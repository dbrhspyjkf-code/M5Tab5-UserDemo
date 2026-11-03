#pragma once
// web 平台收音机音频桥: 命令经 postMessage → 主线程 <audio> (relay mp3 直流),
// 状态回读自 SAB。仅 PLATFORM_BUILD_WEB 下由 app_radio 调用。
namespace webui {
void web_media_radio(const char* key, bool play);   // key=电台 path, play=false 停止
void web_media_volume(int v);                       // 0..100
int  web_media_is_playing();
int  web_media_has_err();
}
