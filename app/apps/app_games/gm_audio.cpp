#include "gm_audio.h"

#include <cmath>
#include <vector>
#include <mutex>
#include <hal/hal.h>
#ifndef PLATFORM_BUILD_DESKTOP
#include <xiaozhi_ctl.h>
#endif

namespace gm_audio {

static std::mutex s_mu;   // 游戏音效串行化 (tone/jingle 共用)

struct Notes {
    float freqs[8];
    double durs[8];
    int n;
};

// 设备端: 24k mono 正弦 + 160 采样淡出 (utils/audio play_tone 同参数)
static void emit_note_24k(float freq, double dur, std::vector<int16_t>& buf)
{
    const int sr = 24000;
    int samples = (int)(sr * dur);
    if (samples < 16) samples = 16;
    const float amp = 32767.0f / 4;
    const int fade = 160;
    buf.clear();
    for (int i = 0; i < samples; i++) {
        float a = amp;
        if (samples - i < fade) a *= (float)(samples - i) / fade;
        buf.push_back((int16_t)(a * sinf(2.0f * 3.14159265f * freq * i / sr)));
    }
}

static void play_locked(const Notes& nt)
{
#ifndef PLATFORM_BUILD_DESKTOP
    std::vector<int16_t> buf;
    for (int i = 0; i < nt.n; i++) {
        emit_note_24k(nt.freqs[i], nt.durs[i], buf);
        xiaozhi_codec_write(buf.data(), (int)buf.size());
    }
#else
    std::vector<int16_t> out;
    const int sr = 48000;
    const int fade = 200;
    for (int i = 0; i < nt.n; i++) {
        int samples = (int)(sr * nt.durs[i]);
        if (samples < 16) samples = 16;
        const float amp = 32767.0f / 5;
        for (int s = 0; s < samples; s++) {
            float a = amp;
            if (samples - s < fade) a *= (float)(samples - s) / fade;
            int16_t v = (int16_t)(a * sin(2.0 * 3.14159265358979 * nt.freqs[i] * s / sr));
            out.push_back(v);
            out.push_back(v);
        }
    }
    GetHAL()->audioPlay(out);
#endif
}

void tone(float freq, double dur_sec)
{
    Notes nt;
    nt.freqs[0] = freq;
    nt.durs[0] = dur_sec;
    nt.n = 1;
    jingle(nt.freqs, nt.durs, 1);
}

void jingle(const float* freqs, const double* durs, int n)
{
    if (n <= 0 || n > 8) return;
    auto* hal = GetHAL();
    if (!hal) return;
    if (hal->getSpeakerVolume() <= 0) return;

    auto* nt = new Notes();
    for (int i = 0; i < n; i++) {
        nt->freqs[i] = freqs[i];
        nt->durs[i] = durs[i];
    }
    nt->n = n;
    bool ok = hal->tryRunDetached([nt]() {
        if (!s_mu.try_lock()) {
            delete nt;   // 上一段还在播: 丢弃本次请求
            return;
        }
        play_locked(*nt);
        s_mu.unlock();
        delete nt;
    });
    if (!ok) delete nt;
}

}  // namespace gm_audio
