// mail_mime — 邮件解码层 (oneapp mail.py §116-328 的 C++ 等价实现)。
//
// 覆盖: base64/QP 编解码、RFC2047 (=?charset?B/Q?...?=)、IMAP modified UTF-7
// (文件夹名)、GBK→UTF-8 (res 分区 gbk_tbl 表, gen_gbk_tbl.py 生成)、
// MIME 树解析 (multipart walk + 传输解码)、HTML 剥离 + 实体反转义、
// 引言/签名裁剪、preview/body 提取、RFC5322 Date→"mm-dd HH:MM"。
// 所有解码 errors=replace 口径 — 任何输入不抛异常、不崩溃。
//
// 仅设备编译 (include 处守卫)。
#pragma once

#ifndef PLATFORM_BUILD_DESKTOP

#include <string>
#include <vector>
#include <cstdint>

namespace mail_direct {

// ── 基础编解码 ──────────────────────────────────────────────────────────────
std::string b64_encode(const uint8_t* data, size_t n, size_t wrap = 0);   // wrap>0 时插 CRLF
bool      b64_decode(const std::string& in, std::string* out);            // 容错 (跳过非法字符)
std::string qp_decode(const std::string& in);                             // =XX / 软换行
std::string utf16_to_utf8(const std::u16string& s);                       // 含代理对

// ── 字符集 ──────────────────────────────────────────────────────────────────
// bytes.decode(charset, errors="replace") 等价: utf-8/us-ascii 直走;
// gbk/gb2312/gb18030 走 res 分区 gbk_tbl; 其他 charset 按 utf-8 容错。
std::string decode_bytes_as(const std::string& raw, const std::string& charset);

// ── 头与夹名 ────────────────────────────────────────────────────────────────
std::string decode_rfc2047(const std::string& raw);   // 解码头 (B/Q), 空串原样
std::string utf7_decode(const std::string& name);     // IMAP modified UTF-7 → 文本
// From 头 → 显示项 (parseaddr 等价: INBOX 显示地址, 其他夹显示名, 截 60)
std::string decode_from(const std::string& raw, bool inbox);

// ── MIME 树 ─────────────────────────────────────────────────────────────────
struct MimeNode {
    std::vector<std::pair<std::string, std::string>> hdrs;  // 小写键 (已折叠)
    std::string ctype    = "text/plain";   // type/subtype 小写
    std::string charset  = "utf-8";
    std::string cte;                       // Content-Transfer-Encoding 小写
    std::string boundary;
    std::string body;                      // 传输解码后的 body (multipart 为原始串)
    std::vector<MimeNode> kids;
    bool multipart() const { return ctype.rfind("multipart/", 0) == 0; }
    const std::string* hdr(const char* k) const;   // 无则 nullptr
};
// 解析整封 (data 可为截断的 16KB/128KB — 按可得的头+首 part 容错解析)
bool mime_parse(const char* data, size_t n, MimeNode* out);
// 正文文本: plain 优先, html 剥离兜底 (walk 深度先序, 同 oneapp _msg_text)
std::string mime_text(const MimeNode& node);
// 头取值解码便捷: Subject → decode_rfc2047
std::string mime_subject(const MimeNode& node);

// ── 正文加工 (oneapp _strip_html/_trim_quotes/extract_*) ────────────────────
std::string strip_html(const std::string& html);
std::string trim_quotes(const std::string& text);
std::string extract_preview(const MimeNode& node, size_t max_chars = 200);
std::string extract_body(const MimeNode& node, size_t max_chars = 6000);

// ── 时间 ────────────────────────────────────────────────────────────────────
// RFC5322 Date → UTC epoch (解析失败 0; 聚合视图按日期排序用)。
int64_t date_epoch(const std::string& raw);
// RFC5322 Date → 本地时区 "mm-dd HH:MM" (设备 TZ=CST-8, hal_esp32 启动设置)。
// 解析失败返回 ""。
std::string fmt_date(const std::string& raw);
// 本地 now → RFC5322 日期头 (发信 Date 用, "+0800" 显式时区)
std::string rfc5322_now();

// ── 地址 ────────────────────────────────────────────────────────────────────
// parseaddr 等价: 返回 (显示名, 地址); 只取地址用第二项
void parse_addr(const std::string& raw, std::string* disp, std::string* addr);

}  // namespace mail_direct

#endif  // !PLATFORM_BUILD_DESKTOP
