// mail_mime 实现 — 见 mail_mime.h。语义逐条对齐 oneapp mail.py 解析层。
#include "mail_mime.h"

#ifndef PLATFORM_BUILD_DESKTOP

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <functional>
#include <esp_log.h>
#include <esp_heap_caps.h>

#include "../assets/assets_partition.h"

static const char* TAG = "mail-mime";

namespace mail_direct {

// ══════════════════════════════════════════════════════════════ 基础编解码 ══

static const char B64C[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const uint8_t* data, size_t n, size_t wrap)
{
    std::string out;
    out.reserve((n + 2) / 3 * 4 + n / wrap + 4);
    for (size_t i = 0; i < n; i += 3) {
        if (wrap && i && (i % wrap == 0)) out += "\r\n";
        uint32_t v = (uint32_t)data[i] << 16 |
                     (i + 1 < n ? (uint32_t)data[i + 1] << 8 : 0) |
                     (i + 2 < n ? (uint32_t)data[i + 2] : 0);
        out += B64C[(v >> 18) & 63];
        out += B64C[(v >> 12) & 63];
        out += (i + 1 < n) ? B64C[(v >> 6) & 63] : '=';
        out += (i + 2 < n) ? B64C[v & 63] : '=';
    }
    return out;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool b64_decode(const std::string& in, std::string* out)
{
    out->clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' ) break;
        int v = b64_val(c);
        if (v < 0) continue;                    // 容错: 跳过空白/非法
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back((char)((acc >> bits) & 0xff));
        }
    }
    return true;
}

std::string qp_decode(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '=') {
            auto hexv = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                return -1;
            };
            if (i + 2 < in.size() && hexv(in[i + 1]) >= 0 && hexv(in[i + 2]) >= 0) {
                out.push_back((char)((hexv(in[i + 1]) << 4) | hexv(in[i + 2])));
                i += 2;
            } else if (i + 2 < in.size() &&
                       (in[i + 1] == '\r' || in[i + 1] == '\n')) {
                i++;                            // 软换行 =\r\n / =\n → 丢弃
                if (in[i] == '\r' && i + 1 < in.size() && in[i + 1] == '\n') i++;
            } else if (i + 1 < in.size() &&
                       (in[i + 1] == '\r' || in[i + 1] == '\n')) {
                i += (in[i + 1] == '\r' && i + 2 < in.size() && in[i + 2] == '\n') ? 2 : 1;
            } else {
                out.push_back(c);               // 孤立 '=' 原样
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string utf16_to_utf8(const std::u16string& s)
{
    std::string out;
    out.reserve(s.size() * 2);
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t cp = (uint16_t)s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) {
            uint32_t lo = (uint16_t)s[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 63)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 63)));
            out.push_back((char)(0x80 | (cp & 63)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 63)));
            out.push_back((char)(0x80 | ((cp >> 6) & 63)));
            out.push_back((char)(0x80 | (cp & 63)));
        }
    }
    return out;
}

// ══════════════════════════════════════════════════════════════ GBK 表 ══════
// res 分区 gbk_tbl (gen_gbk_tbl.py): 0x81-0xFE 首字节 × 0x40-0xFE 尾字节(跳
// 0x7F) → u16 UTF-16 码元, 0=无效 (→U+FFFD)。23940 项 = 126×190, lazy 首次
// find() 加载。
static const uint16_t* gbk_table()
{
    static const uint16_t* tbl = nullptr;
    if (!tbl) {
        size_t sz = 0;
        const uint8_t* p = assets_part::find("gbk_tbl", &sz);
        if (!p || sz < 23940 * 2) {
            ESP_LOGW(TAG, "gbk_tbl asset missing (%p %u)", p, (unsigned)sz);
            return nullptr;
        }
        tbl = reinterpret_cast<const uint16_t*>(p);
    }
    return tbl;
}

static inline int gbk_index(uint8_t lead, uint8_t trail)
{
    if (lead < 0x81 || lead > 0xFE) return -1;
    if (trail < 0x40 || trail > 0xFE || trail == 0x7F) return -1;
    int t = trail < 0x7F ? (trail - 0x40) : (trail - 0x41);
    return (lead - 0x81) * 190 + t;
}

std::string decode_bytes_as(const std::string& raw, const std::string& charset)
{
    // charset 归一小写
    std::string cs;
    cs.reserve(charset.size());
    for (char c : charset) cs.push_back((char)tolower((unsigned char)c));
    auto is_gbk = [](const std::string& s) {
        return s == "gbk" || s == "gb2312" || s == "gb18030" ||
               s == "cp936" || s == "euc-cn" || s == "eucgb2312-cn" ||
               s == "gb_2312-80" || s == "chinese";
    };
    if (!is_gbk(cs)) {
        // utf-8/us-ascii/unknown → utf-8 直通 (容错替换无效序列)
        std::string out;
        out.reserve(raw.size());
        size_t i = 0;
        while (i < raw.size()) {
            uint8_t c = (uint8_t)raw[i];
            size_t need = c < 0x80 ? 1 : c >> 5 == 6 ? 2 : c >> 4 == 14 ? 3
                        : c >> 3 == 30 ? 4 : 0;
            if (need == 0 || i + need > raw.size()) {
                out += "\xEF\xBF\xBD";
                i++;
                continue;
            }
            bool bad = need >= 2 && ((uint8_t)raw[i + 1] & 0xC0) != 0x80;
            if (!bad && need >= 3 && ((uint8_t)raw[i + 2] & 0xC0) != 0x80) bad = true;
            if (!bad && need >= 4 && ((uint8_t)raw[i + 3] & 0xC0) != 0x80) bad = true;
            if (bad) {
                out += "\xEF\xBF\xBD";
                i++;
                continue;
            }
            out.append(raw, i, need);
            i += need;
        }
        return out;
    }
    const uint16_t* tbl = gbk_table();
    std::string out;
    out.reserve(raw.size() + raw.size() / 2);
    size_t i = 0;
    while (i < raw.size()) {
        uint8_t c = (uint8_t)raw[i];
        if (c < 0x80) { out.push_back(raw[i]); i++; continue; }
        if (i + 1 >= raw.size()) { out += "\xEF\xBF\xBD"; break; }
        uint8_t t = (uint8_t)raw[i + 1];
        int idx = gbk_index(c, t);
        uint16_t u = (idx >= 0 && tbl) ? tbl[idx] : 0;
        if (u == 0) { out += "\xEF\xBF\xBD"; i += 2; continue; }
        std::u16string one;
        one.push_back((char16_t)u);
        out += utf16_to_utf8(one);
        i += 2;
    }
    return out;
}

// ═══════════════════════════════════════════════════════════ RFC2047/UTF-7 ══

// 单个 encoded-word: =?charset?enc?text?=
static bool rfc2047_one(const std::string& raw, size_t start, size_t* end,
                        std::string* out)
{
    // raw[start..] 须以 "=?" 起
    size_t c1 = raw.find('?', start + 2);
    if (c1 == std::string::npos) return false;
    size_t c2 = raw.find('?', c1 + 1);
    if (c2 == std::string::npos) return false;
    size_t fin = raw.find("?=", c2 + 1);
    if (fin == std::string::npos) return false;
    std::string cs = raw.substr(start + 2, c1 - start - 2);
    char enc = (char)toupper((unsigned char)raw[c1 + 1]);
    std::string text = raw.substr(c2 + 1, fin - c2 - 1);
    std::string bytes;
    if (enc == 'B') {
        b64_decode(text, &bytes);
    } else if (enc == 'Q') {
        for (size_t i = 0; i < text.size(); i++) {
            if (text[i] == '_') bytes.push_back(' ');
            else if (text[i] == '=' && i + 2 < text.size()) {
                auto hexv = [](char h) -> int {
                    if (h >= '0' && h <= '9') return h - '0';
                    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                    return -1;
                };
                int hi = hexv(text[i + 1]), lo = hexv(text[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    bytes.push_back((char)((hi << 4) | lo));
                    i += 2;
                } else bytes.push_back(text[i]);
            } else bytes.push_back(text[i]);
        }
    } else {
        return false;
    }
    *out = decode_bytes_as(bytes, cs);
    *end = fin + 2;
    return true;
}

std::string decode_rfc2047(const std::string& raw)
{
    if (raw.empty()) return raw;
    std::string out;
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '=' && i + 1 < raw.size() && raw[i + 1] == '?') {
            std::string piece;
            size_t end = i;
            if (rfc2047_one(raw, i, &end, &piece)) {
                out += piece;
                i = end;
                // 相邻 encoded-word 间的纯空白折叠 (email.decode_header 口径)
                size_t j = i;
                while (j < raw.size() &&
                       (raw[j] == ' ' || raw[j] == '\t' || raw[j] == '\r' ||
                        raw[j] == '\n'))
                    j++;
                if (j < raw.size() && raw[j] == '=' && j + 1 < raw.size() &&
                    raw[j + 1] == '?')
                    i = j;
                continue;
            }
        }
        // 普通文本段
        size_t j = i;
        while (j < raw.size() &&
               !(raw[j] == '=' && j + 1 < raw.size() && raw[j + 1] == '?'))
            j++;
        out.append(raw, i, j - i);
        i = j;
        if (i < raw.size()) {
            // "=?" 形态但解析失败 → 原样吐 2 字符防死循环
            out += "=?";
            i += 2;
        }
    }
    return out;
}

std::string utf7_decode(const std::string& name)
{
    if (name == "INBOX") return name;
    std::string out;
    size_t i = 0;
    while (i < name.size()) {
        if (name[i] != '&') { out.push_back(name[i]); i++; continue; }
        size_t end = name.find('-', i);
        if (end == std::string::npos) { out.append(name, i); break; }
        std::string enc = name.substr(i + 1, end - i - 1);
        if (enc.empty()) {
            out.push_back('&');
        } else {
            std::string b64 = enc;
            for (auto& c : b64) if (c == ',') c = '/';
            size_t pad = (4 - b64.size() % 4) % 4;
            b64.append(pad, '=');
            std::string bytes;
            if (b64_decode(b64, &bytes) && bytes.size() % 2 == 0) {
                std::u16string u16;
                for (size_t k = 0; k + 1 < bytes.size(); k += 2)
                    u16.push_back((char16_t)(((uint8_t)bytes[k] << 8) |
                                             (uint8_t)bytes[k + 1]));
                out += utf16_to_utf8(u16);
            } else {
                out.append(name, i, end - i + 1);   // 坏段原样 (oneapp 同口径)
            }
        }
        i = end + 1;
    }
    return out;
}

void parse_addr(const std::string& raw, std::string* disp, std::string* addr)
{
    // email.utils.parseaddr 简化等价: 取 <> 内地址 (无 <> 时整串去尖括号),
    // <> 外部分为显示名 (去引号)。
    *disp = "";
    *addr = "";
    size_t lt = raw.rfind('<');
    size_t gt = (lt != std::string::npos) ? raw.find('>', lt) : std::string::npos;
    if (lt != std::string::npos && gt != std::string::npos) {
        *addr = raw.substr(lt + 1, gt - lt - 1);
        std::string d = raw.substr(0, lt);
        // 去首尾空白与引号
        size_t b = d.find_first_not_of(" \t\r\n\"");
        size_t e = d.find_last_not_of(" \t\r\n\"");
        if (b != std::string::npos) *disp = d.substr(b, e - b + 1);
    } else {
        std::string d = raw;
        size_t b = d.find_first_not_of(" \t\r\n");
        size_t e = d.find_last_not_of(" \t\r\n");
        if (b != std::string::npos) d = d.substr(b, e - b + 1);
        *addr = d;
        *disp = "";
    }
}

std::string decode_from(const std::string& raw_hdr, bool inbox)
{
    if (raw_hdr.empty()) return "";
    std::string dec = decode_rfc2047(raw_hdr);
    // 折叠空白 (头里可能残留 CRLF+WSP)
    std::string raw;
    raw.reserve(dec.size());
    for (char c : dec)
        raw.push_back((c == '\r' || c == '\n') ? ' ' : c);
    std::string disp, addr;
    parse_addr(raw, &disp, &addr);
    std::string out = addr.empty() ? disp : addr;
    if (!inbox && !disp.empty()) out = disp;
    // 去残余引号
    if (out.size() >= 2 && out.front() == '"' && out.back() == '"')
        out = out.substr(1, out.size() - 2);
    return out.substr(0, 60);
}

// ════════════════════════════════════════════════════════════════ MIME 树 ══

const std::string* MimeNode::hdr(const char* k) const
{
    for (const auto& h : hdrs)
        if (h.first == k) return &h.second;
    return nullptr;
}

// 头区参数提取: "text/plain; charset=\"gbk\"; ..." → type 与参数
static void parse_ctype(const std::string& v, std::string* type,
                        std::string* charset, std::string* boundary)
{
    std::string low;
    low.reserve(v.size());
    for (char c : v) low.push_back((char)tolower((unsigned char)c));
    size_t sc = low.find(';');
    *type = (sc == std::string::npos) ? low : low.substr(0, sc);
    // trim
    while (!type->empty() && (type->back() == ' ' || type->back() == '\t'))
        type->pop_back();
    auto get_param = [&](const char* key) -> std::string {
        size_t p = low.find(key);
        if (p == std::string::npos) return "";
        p += strlen(key);
        if (p >= low.size() || low[p] != '=') return "";
        p++;
        while (p < low.size() && (low[p] == ' ' || low[p] == '\t')) p++;
        if (p < low.size() && low[p] == '"') {
            size_t q = low.find('"', p + 1);
            if (q == std::string::npos) return low.substr(p + 1);
            return low.substr(p + 1, q - p - 1);
        }
        size_t q = low.find(';', p);
        std::string r = low.substr(p, q == std::string::npos ? std::string::npos : q - p);
        while (!r.empty() && (r.back() == ' ' || r.back() == '\t')) r.pop_back();
        return r;
    };
    *charset  = get_param("charset");
    *boundary = get_param("boundary");
    if (charset->empty()) *charset = "utf-8";
}

// 头区解析: data[0..hdr_end) → 折叠+切行+入 map; 返回头区结束位置 (body 起点)
static size_t parse_headers(const char* data, size_t n, MimeNode* node)
{
    // 找空行 (\r\n\r\n 或 \n\n) — 找不到时整块当头 (截断保护)
    size_t hdr_end = n;
    for (size_t i = 0; i + 1 < n; i++) {
        if (data[i] == '\n') {
            if (i + 1 < n && data[i + 1] == '\n') { hdr_end = i + 2; break; }
            if (i + 2 < n && data[i + 1] == '\r' && data[i + 2] == '\n') {
                hdr_end = i + 3;
                break;
            }
        }
    }
    // 逐行 + 折叠 (后续行以 WSP 开头则续前行)
    std::vector<std::string> lines;
    size_t i = 0;
    std::string cur;
    while (i < hdr_end) {
        size_t j = i;
        while (j < hdr_end && data[j] != '\n') j++;
        size_t e = j;
        if (e > i && data[e - 1] == '\r') e--;
        std::string line(data + i, e - i);
        i = j + 1;
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t') && !cur.empty()) {
            cur += line;                    // 折叠: 去 WSP? email 模块保留空格折叠
            for (auto& c : cur)
                if (c == '\t') c = ' ';
        } else {
            if (!cur.empty()) lines.push_back(cur);
            cur = line;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    for (const auto& line : lines) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        for (auto& c : key) c = (char)tolower((unsigned char)c);
        std::string val = line.substr(colon + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
            val.erase(val.begin());
        node->hdrs.emplace_back(std::move(key), std::move(val));
    }
    // 提取结构头
    if (const std::string* ct = node->hdr("content-type"))
        parse_ctype(*ct, &node->ctype, &node->charset, &node->boundary);
    if (const std::string* cte = node->hdr("content-transfer-encoding")) {
        node->cte.reserve(cte->size());
        for (char c : *cte) node->cte.push_back((char)tolower((unsigned char)c));
        size_t sc = node->cte.find(';');
        if (sc != std::string::npos) node->cte = node->cte.substr(0, sc);
        while (!node->cte.empty() && node->cte.back() == ' ') node->cte.pop_back();
    }
    return hdr_end;
}

static std::string decode_body_bytes(const std::string& raw, const MimeNode& node)
{
    if (node.cte == "base64") {
        std::string dec;
        b64_decode(raw, &dec);
        return decode_bytes_as(dec, node.charset);
    }
    if (node.cte == "quoted-printable") {
        return decode_bytes_as(qp_decode(raw), node.charset);
    }
    return decode_bytes_as(raw, node.charset);   // 7bit/8bit/binary/未知
}

// 找行首 boundary (data 内完整匹配 "--boundary"; final = "--boundary--" 终态)
struct BFind { size_t pos; bool final; };
static bool find_boundary(const char* data, size_t n, const std::string& b,
                          size_t from, BFind* out)
{
    std::string dash = "--" + b;
    size_t ls = from;
    if (from > 0 && data[from - 1] != '\n') {   // from 在行中 → 跳到下一行首
        while (ls < n && data[ls] != '\n') ls++;
        if (ls < n) ls++;
    }
    while (ls < n) {
        size_t le = ls;
        while (le < n && data[le] != '\n') le++;
        size_t e = le;
        if (e > ls && data[e - 1] == '\r') e--;
        std::string line(data + ls, e - ls);
        if (line.rfind(dash, 0) == 0) {
            std::string rest = line.substr(dash.size());
            out->pos = ls;
            out->final = rest.rfind("--", 0) == 0;
            return true;
        }
        if (le >= n) break;
        ls = le + 1;                        // 下一行首
    }
    return false;
}

static void mime_parse_part(const char* data, size_t n, MimeNode* node)
{
    size_t body_off = parse_headers(data, n, node);
    if (!node->multipart()) {
        node->body = decode_body_bytes(std::string(data + body_off, n - body_off),
                                       *node);
        return;
    }
    // multipart: 按 boundary 切子 part 递归。
    // part 区域 = [上一界线行尾, 下一界线行首); 无下一条界线时尾段到文末
    // (截断邮件保护)。
    if (node->boundary.empty()) return;
    BFind bf;
    if (!find_boundary(data, n, node->boundary, 0, &bf)) return;
    auto push_part = [&](size_t a, size_t b) {
        if (b <= a) return;
        MimeNode kid;
        mime_parse_part(data + a, b - a, &kid);
        node->kids.push_back(std::move(kid));
    };
    size_t e = bf.pos;
    while (e < n && data[e] != '\n') e++;
    size_t part_a = (e < n) ? e + 1 : n;
    while (true) {
        BFind next;
        if (!find_boundary(data, n, node->boundary, part_a, &next)) {
            push_part(part_a, n);          // 尾段无终界线 (截断) → 到文末
            break;
        }
        push_part(part_a, next.pos);
        if (next.final) break;
        e = next.pos;
        while (e < n && data[e] != '\n') e++;
        part_a = (e < n) ? e + 1 : n;
    }
}

bool mime_parse(const char* data, size_t n, MimeNode* out)
{
    *out = MimeNode{};
    if (!data || n == 0) return false;
    mime_parse_part(data, n, out);
    return true;
}

static void mime_walk(const MimeNode& node,
                      const std::function<void(const MimeNode&)>& fn)
{
    if (node.multipart()) {
        for (const auto& k : node.kids) mime_walk(k, fn);
    } else {
        fn(node);
    }
}

std::string mime_text(const MimeNode& node)
{
    // plain 优先, html 剥离兜底 (oneapp _msg_text 同口径)
    std::string plain, html;
    mime_walk(node, [&](const MimeNode& leaf) {
        if (leaf.ctype == "text/plain" && plain.empty()) plain = leaf.body;
        else if (leaf.ctype == "text/html" && html.empty()) html = leaf.body;
    });
    if (!plain.empty()) return plain;
    if (!html.empty()) return strip_html(html);
    // 非 multipart 单体
    if (node.ctype == "text/html") return strip_html(node.body);
    return node.body;
}

std::string mime_subject(const MimeNode& node)
{
    if (const std::string* s = node.hdr("subject")) {
        std::string d = decode_rfc2047(*s);
        return d.substr(0, 80);
    }
    return "(无主题)";
}

// ══════════════════════════════════════════════════════════════ HTML 剥离 ══

static std::string html_unescape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '&') { out.push_back(s[i]); i++; continue; }
        size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) {
            out.push_back('&');
            i++;
            continue;
        }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if (!ent.empty() && ent[0] == '#') {
            long cp = -1;
            if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                cp = strtol(ent.c_str() + 2, nullptr, 16);
            else
                cp = strtol(ent.c_str() + 1, nullptr, 10);
            if (cp > 0 && cp < 0x110000) {
                std::u16string u;
                if (cp >= 0x10000) {
                    cp -= 0x10000;
                    u.push_back((char16_t)(0xD800 + (cp >> 10)));
                    u.push_back((char16_t)(0xDC00 + (cp & 0x3FF)));
                } else u.push_back((char16_t)cp);
                out += utf16_to_utf8(u);
            } else out += "&" + ent + ";";
        } else {
            struct Named { const char* e; const char* r; };
            static const Named NAMED[] = {
                {"nbsp", " "},    {"amp", "&"},     {"lt", "<"},    {"gt", ">"},
                {"quot", "\""},   {"apos", "'"},    {"copy", "©"},  {"reg", "®"},
                {"trade", "™"},   {"hellip", "…"},  {"mdash", "—"}, {"ndash", "–"},
                {"laquo", "«"},   {"raquo", "»"},   {"ldquo", "“"}, {"rdquo", "”"},
                {"lsquo", "‘"},   {"rsquo", "’"},   {"lquo", "“"},  {"middot", "·"},
                {"bull", "•"},    {"times", "×"},   {"divide", "÷"}, {"deg", "°"},
                {"plusmn", "±"},  {"frac12", "½"},  {"frac14", "¼"}, {"sup2", "²"},
                {"eacute", "é"},  {"egrave", "è"},  {"agrave", "à"}, {"uuml", "ü"},
                {"ouml", "ö"},    {"auml", "ä"},    {"szlig", "ß"}, {"ccedil", "ç"},
            };
            bool hit = false;
            for (const auto& n : NAMED) {
                if (ent == n.e) {
                    out += n.r;
                    hit = true;
                    break;
                }
            }
            if (!hit) out += "&" + ent + ";";
        }
        i = semi + 1;
    }
    return out;
}

// 大小写不敏感子串查找 (用于标签名匹配)
static size_t ifind(const std::string& hay, const char* needle, size_t from)
{
    std::string l;
    l.reserve(hay.size());
    for (size_t k = 0; k < hay.size(); k++)
        l.push_back((char)tolower((unsigned char)hay[k]));
    return l.find(needle, from);
}

std::string strip_html(const std::string& html)
{
    std::string text = html;
    // <style|script>…</style|script> 整块剔除
    for (const char* tag : {"style", "script"}) {
        std::string open = std::string("<") + tag;
        std::string close = std::string("</") + tag;
        size_t p = ifind(text, open.c_str(), 0);
        while (p != std::string::npos) {
            size_t q = ifind(text, close.c_str(), p);
            size_t end = (q == std::string::npos) ? text.size()
                         : (text.find('>', q + strlen(close.c_str())));
            end = (end == std::string::npos) ? text.size() : end + 1;
            text.replace(p, end - p, " ");
            p = ifind(text, open.c_str(), p + 1);
        }
    }
    // <br|p|div|tr|li…> → 空格
    {
        std::string out;
        out.reserve(text.size());
        size_t i = 0;
        while (i < text.size()) {
            if (text[i] == '<') {
                size_t e = text.find('>', i);
                if (e == std::string::npos) { out.append(text, i); break; }
                std::string tag = text.substr(i + 1, e - i - 1);
                for (auto& c : tag) c = (char)tolower((unsigned char)c);
                bool block = tag.rfind("br", 0) == 0 || tag.rfind("p", 0) == 0 ||
                             tag.rfind("div", 0) == 0 || tag.rfind("tr", 0) == 0 ||
                             tag.rfind("li", 0) == 0 || tag.rfind("table", 0) == 0 ||
                             tag.rfind("h1", 0) == 0 || tag.rfind("h2", 0) == 0 ||
                             tag.rfind("h3", 0) == 0 || tag.rfind("hr", 0) == 0;
                out += block ? " " : "";    // 其他标签剔除
                i = e + 1;
            } else {
                out.push_back(text[i]);
                i++;
            }
        }
        text = out;
    }
    text = html_unescape(text);
    // nbsp (\xa0 → UTF-8 的 C2 A0) 归一普通空格
    {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); i++) {
            if ((uint8_t)text[i] == 0xC2 && i + 1 < text.size() &&
                (uint8_t)text[i + 1] == 0xA0) {
                out.push_back(' ');
                i++;
            } else out.push_back(text[i]);
        }
        text = out;
    }
    return text;
}

// ═══════════════════════════════════════════════════════ 引言/预览/正文 ═════

static const char* const QUOTE_HEADS[] = {"from:", "de:", "sent:", "enviado",
                                          "le:", "am:", "van:"};

std::string trim_quotes(const std::string& text)
{
    std::string out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t eol = text.find('\n', start);
        std::string line = text.substr(
            start, eol == std::string::npos ? std::string::npos : eol - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // 剥首尾空白判定
        size_t b = line.find_first_not_of(" \t");
        std::string s = (b == std::string::npos) ? "" : line.substr(b);
        bool stop = s.rfind(">", 0) == 0 || s.rfind("--", 0) == 0;
        if (!stop) {
            std::string low;
            low.reserve(s.size() < 16 ? s.size() : 16);
            for (size_t k = 0; k < s.size() && k < 16; k++)
                low.push_back((char)tolower((unsigned char)s[k]));
            for (const char* h : QUOTE_HEADS)
                if (low.rfind(h, 0) == 0) { stop = true; break; }
        }
        if (stop) break;
        out += line;
        out.push_back('\n');
        if (eol == std::string::npos) break;
        start = eol + 1;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

static std::string msg_text_imgified(const MimeNode& node)
{
    std::string text = mime_text(node);
    // 营销邮件 plain 部分常内嵌 <img …> — 图标化 (oneapp 同口径)
    {
        std::string out;
        size_t i = 0;
        while (i < text.size()) {
            if (text[i] == '<' && i + 3 < text.size() &&
                (text[i + 1] == 'i' || text[i + 1] == 'I') &&
                (text[i + 2] == 'm' || text[i + 2] == 'M') &&
                (text[i + 3] == 'g' || text[i + 3] == 'G')) {
                size_t e = text.find('>', i);
                if (e == std::string::npos) { out.append(text, i); break; }
                out += "(图)";
                i = e + 1;
            } else out.push_back(text[i++]);
        }
        text = out;
    }
    return text;
}

std::string extract_preview(const MimeNode& node, size_t max_chars)
{
    try {
        std::string text = msg_text_imgified(node);
        if (text.empty()) return "";
        text = trim_quotes(text);
        std::string out;
        size_t start = 0;
        while (start <= text.size()) {
            size_t eol = text.find('\n', start);
            std::string line = text.substr(
                start, eol == std::string::npos ? std::string::npos : eol - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t b = line.find_first_not_of(" \t");
            if (b != std::string::npos) {
                if (!out.empty()) out.push_back(' ');
                out.append(line, b, line.size() - b);
            }
            if (eol == std::string::npos) break;
            start = eol + 1;
        }
        // 控制字符剔除 + 残余标签剔除
        std::string clean;
        clean.reserve(out.size());
        for (char c : out) {
            uint8_t u = (uint8_t)c;
            if (u >= 32 && u != 127) clean.push_back(c);
        }
        {
            std::string out2;
            size_t i = 0;
            while (i < clean.size()) {
                if (clean[i] == '<') {
                    size_t e = clean.find('>', i);
                    if (e != std::string::npos && e - i <= 201) {
                        bool tag = e > i && isalpha((unsigned char)clean[i + 1]);
                        if (tag) { out2.push_back(' '); i = e + 1; continue; }
                    }
                }
                out2.push_back(clean[i]);
                i++;
            }
            clean = out2;
        }
        return clean.substr(0, max_chars);
    } catch (...) {
        return "";
    }
}

std::string extract_body(const MimeNode& node, size_t max_chars)
{
    try {
        std::string text = msg_text_imgified(node);
        if (text.empty()) return "(空邮件)";
        text = trim_quotes(text);
        std::string out;
        bool blank = false;
        size_t start = 0;
        while (start <= text.size()) {
            size_t eol = text.find('\n', start);
            std::string line = text.substr(
                start, eol == std::string::npos ? std::string::npos : eol - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t b = line.find_first_not_of(" \t");
            if (b != std::string::npos) {
                // rstrip 行尾空白
                size_t e = line.find_last_not_of(" \t");
                out.append(line, 0, e + 1);
                out.push_back('\n');
                blank = false;
            } else if (!out.empty() && !blank) {
                out.push_back('\n');
                blank = true;
            }
            if (eol == std::string::npos) break;
            start = eol + 1;
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        if (out.empty()) return "(空邮件)";
        return out.substr(0, max_chars);
    } catch (...) {
        return "(正文解析失败)";
    }
}

// ═══════════════════════════════════════════════════════════════════ 时间 ══

static int month_from(const char* mon)
{
    static const char* M[] = {"jan", "feb", "mar", "apr", "may", "jun",
                              "jul", "aug", "sep", "oct", "nov", "dec"};
    char l[4] = {0, 0, 0, 0};
    for (int i = 0; i < 3 && mon[i]; i++)
        l[i] = (char)tolower((unsigned char)mon[i]);
    for (int i = 0; i < 12; i++)
        if (strncmp(l, M[i], 3) == 0) return i;
    return -1;
}

int64_t date_epoch(const std::string& raw)
{
    if (raw.empty()) return 0;
    // "Tue, 05 Nov 2026 10:11:12 +0800" — 手工解析 (容错: 逗号/星期可缺)
    std::vector<std::string> tok;
    {
        std::string cur;
        for (char c : raw) {
            if (c == ' ' || c == ',' || c == '\t' || c == '\r' || c == '\n') {
                if (!cur.empty()) tok.push_back(cur);
                cur.clear();
            } else cur.push_back(c);
        }
        if (!cur.empty()) tok.push_back(cur);
    }
    int day = -1, mon = -1, year = -1, hh = -1, mm = -1, ss = 0;
    int zoff = 0;
    for (size_t i = 0; i < tok.size(); i++) {
        const std::string& t = tok[i];
        if (mon < 0 && t.size() == 3 && month_from(t.c_str()) >= 0) {
            mon = month_from(t.c_str());
            continue;
        }
        if (day < 0 && t.size() <= 2) {
            day = atoi(t.c_str());
            if (day > 0 && day <= 31) continue;
            day = -1;
        }
        if (year < 0 && t.size() == 4 &&
            all_of(t.begin(), t.end(), ::isdigit)) {
            year = atoi(t.c_str());
            continue;
        }
        if (hh < 0 && t.find(':') != std::string::npos) {
            if (sscanf(t.c_str(), "%d:%d:%d", &hh, &mm, &ss) >= 2) continue;
            hh = -1;
        }
        if (t.size() == 5 && (t[0] == '+' || t[0] == '-')) {
            int zh = atoi(t.c_str() + 1) / 100;
            int zm = atoi(t.c_str() + 3);
            zoff = (t[0] == '+' ? 1 : -1) * (zh * 3600 + zm * 60);
        }
    }
    if (mon < 0 || day < 0 || year < 0 || hh < 0) return 0;
    // days_from_civil (Howard Hinnant) — newlib 无 timegm, 手工算 UTC epoch
    {
        int64_t y = year, m = mon + 1;
        y -= (m <= 2);
        int64_t era = (y >= 0 ? y : y - 399) / 400;
        int64_t yoe = y - era * 400;
        int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        int64_t days = era * 146097 + doe - 719468;
        return days * 86400 + hh * 3600 + mm * 60 + ss - zoff;
    }
}

std::string fmt_date(const std::string& raw)
{
    int64_t ep = date_epoch(raw);
    if (ep <= 0) return "";
    time_t epoch = (time_t)ep;
    struct tm loc;
    localtime_r(&epoch, &loc);
    char buf[24];
    strftime(buf, sizeof(buf), "%m-%d %H:%M", &loc);
    return buf;
}

std::string rfc5322_now()
{
    time_t now = time(nullptr);
    struct tm loc;
    localtime_r(&now, &loc);
    static const char* WD[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* MO[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[64];
    snprintf(buf, sizeof(buf), "%s, %d %s %d %02d:%02d:%02d +0800",
             WD[loc.tm_wday], loc.tm_mday, MO[loc.tm_mon], loc.tm_year + 1900,
             loc.tm_hour, loc.tm_min, loc.tm_sec);
    return buf;
}

}  // namespace mail_direct

#endif  // !PLATFORM_BUILD_DESKTOP
