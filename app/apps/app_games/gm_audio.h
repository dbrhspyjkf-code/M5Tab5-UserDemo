#pragma once
/*
 * gm_audio — 游戏音效 (2026-11-05)。
 *
 * 振荡器零素材 (car-dash 游戏同思路): tone() 单音 / jingle() 旋律。
 * 设备端与 utils/audio 的 play_tone 同链路 (24k mono 正弦 xiaozhi_codec_write
 * 阻塞写, 后台线程不占 UI), 但自持一把静态互斥 + 全部音效只走这一个入口 ——
 * xiaozhi_codec_write 内部缓冲非线程安全, 同一时刻只允许一个游戏音在写。
 * 忙则丢弃新请求 (play_tone 同款丢弃语义)。
 * 桌面端走 HAL audioPlay (48k 双声道, async)。
 */
namespace gm_audio {

// 单音 (短促翻牌/落子声)
void tone(float freq, double dur_sec);

// 逐音顺播旋律 (freq<=0 = 休止); 用于通关/失败 jingle
void jingle(const float* freqs, const double* durs, int n);

}  // namespace gm_audio
