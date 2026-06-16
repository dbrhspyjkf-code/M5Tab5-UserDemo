/*
 * Desktop JPEG decoder using libjpeg-turbo.
 * Mirrors the ESP32-P4 hardware decoder (platforms/tab5 hal_jpeg.cpp): decodes
 * a JPEG file to a freshly malloc'd RGB565 buffer (LVGL native format). Caller
 * owns / frees the returned buffer. Returns nullptr on failure.
 */
#include "../hal_desktop.h"
#include <mooncake_log.h>
#include <cstdio>
#include <cstdlib>
#include <jpeglib.h>
#include <csetjmp>

static const std::string _tag = "hal-jpeg";

struct _jpeg_err_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void _jpeg_error_exit(j_common_ptr cinfo)
{
    auto* err = reinterpret_cast<_jpeg_err_mgr*>(cinfo->err);
    longjmp(err->setjmp_buffer, 1);
}

uint8_t* HalDesktop::decodeJpegToRGB565(const std::string& filePath, int& outW, int& outH)
{
    outW = outH = 0;

    FILE* fp = fopen(filePath.c_str(), "rb");
    if (!fp) {
        mclog::tagWarn(_tag, "open {} failed", filePath);
        return nullptr;
    }

    struct jpeg_decompress_struct cinfo;
    struct _jpeg_err_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = _jpeg_error_exit;

    uint8_t* out = nullptr;
    uint8_t* row = nullptr;

    if (setjmp(jerr.setjmp_buffer)) {
        // libjpeg signalled a fatal error.
        mclog::tagWarn(_tag, "decode {} failed", filePath);
        jpeg_destroy_decompress(&cinfo);
        if (row) free(row);
        if (out) { free(out); out = nullptr; }
        fclose(fp);
        return nullptr;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    const int w = cinfo.output_width;
    const int h = cinfo.output_height;
    out = static_cast<uint8_t*>(malloc((size_t)w * h * 2));
    if (!out) {
        mclog::tagWarn(_tag, "alloc {}x{} failed", w, h);
        longjmp(jerr.setjmp_buffer, 1);
    }

    const int rb = w * cinfo.output_components;  // 3 bytes/px (RGB)
    row = static_cast<uint8_t*>(malloc(rb));

    uint16_t* dst = reinterpret_cast<uint16_t*>(out);
    while (cinfo.output_scanline < (JDIMENSION)h) {
        JSAMPROW rows[1] = {row};
        jpeg_read_scanlines(&cinfo, rows, 1);
        for (int x = 0; x < w; ++x) {
            uint8_t r = row[x * 3 + 0];
            uint8_t g = row[x * 3 + 1];
            uint8_t b = row[x * 3 + 2];
            *dst++ = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(row);
    fclose(fp);

    outW = w;
    outH = h;
    mclog::tagInfo(_tag, "decoded {}x{}", w, h);
    return out;
}
