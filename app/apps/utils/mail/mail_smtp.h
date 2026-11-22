// mail_smtp — SMTP_SSL 直发 (oneapp mail.py send_mail/_append_sent 等价)。
// smtp.<dom>:465 隐式 TLS (outlook 587 端口同样按隐式 TLS 处理 — oneapp 原样)。
// AUTH LOGIN (授权码), 回信带 In-Reply-To/References(≤900), 正文 text/plain
// UTF-8 base64。成功后尽力 APPEND 已发送 (163/126 类自动存 Sent, 先按
// Message-ID 查重; 任何失败静默不影响发送结果)。
//
// 仅设备编译 (include 处守卫)。
#pragma once

#ifndef PLATFORM_BUILD_DESKTOP

#include <string>
#include <cstdint>
#include "mail_imap.h"

namespace mail_direct {

// 直发一封。err 走 _friendly_err 同款中文文案。
bool smtp_send(const std::string& email, const std::string& auth,
               const std::string& to, const std::string& subject,
               const std::string& body, const std::string& in_reply_to,
               const std::string& references, std::string* err,
               // 出参: 生成的原始消息 (APPEND 已发送用) 与 Message-ID
               std::string* raw_msg, std::string* msg_id);

}  // namespace mail_direct

#endif  // !PLATFORM_BUILD_DESKTOP
