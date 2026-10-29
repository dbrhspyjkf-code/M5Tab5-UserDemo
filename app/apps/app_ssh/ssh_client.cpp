/*
 * ssh_client — libssh 客户端封装实现。阶段日志沿用参考项目的
 * reportSshStage 思路(排查连接卡在哪一步), 全部走 mclog。
 */
#include "ssh_client.h"
#include <mooncake_log.h>

#ifndef PLATFORM_BUILD_DESKTOP

#include <libssh/libssh.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

static const char* TAG = "ssh";

namespace {
void stage(const char* s) { mclog::tagInfo(TAG, "stage: {}", s); }

// libssh 初始化由 init.c 的 CONSTRUCTOR_ATTRIBUTE 构造函数自动完成
// (ewpa 包装层的 libssh_begin 已随 Arduino 壳删除, vendored 版无需手动 begin)
}  // namespace

bool SshClient::connect(const SshProfile& profile, std::string& error,
                        int columns, int rows)
{
    stage("disconnect");
    disconnect();

    stage("new");
    ssh_session session = ssh_new();
    if (!session) { error = "ssh_new failed"; return false; }

    stage("options");
    const int verbosity = SSH_LOG_NOLOG;
    const int port = profile.port;
    ssh_options_set(session, SSH_OPTIONS_HOST, profile.host.c_str());
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(session, SSH_OPTIONS_USER, profile.user.c_str());
    ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
    // 设备无 HOME/getpwuid: libssh 默认 sshdir 要展开 ~/.ssh, 拿不到家目录
    // 会以 "Failed to process system configuration files" 拒连(2026-10-29
    // 实测)。显式给个目录, /tmp/config 不存在 → 配置解析静默跳过。
    ssh_options_set(session, SSH_OPTIONS_SSH_DIR, "/tmp");

    stage("connect");
    if (ssh_connect(session) != SSH_OK) {
        error = ssh_get_error(session);
        ssh_free(session);
        return false;
    }

    stage("auth");
    if (ssh_userauth_password(session, nullptr, profile.password.c_str()) != SSH_AUTH_SUCCESS) {
        error = ssh_get_error(session);
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }
    ssh_set_blocking(session, 1);

    stage("channel");
    ssh_channel channel = ssh_channel_new(session);
    if (!channel) {
        error = "ssh_channel_new failed";
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }

    stage("pty_shell");
    if (ssh_channel_open_session(channel) != SSH_OK ||
        ssh_channel_request_pty_size(channel, "xterm", columns, rows) != SSH_OK ||
        ssh_channel_request_shell(channel) != SSH_OK) {
        error = ssh_get_error(session);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }

    ssh_set_blocking(session, 0);
    _session = session;
    _channel = channel;
    stage("ready");
    return true;
}

void SshClient::disconnect()
{
    if (_channel) {
        ssh_channel channel = static_cast<ssh_channel>(_channel);
        ssh_channel_send_eof(channel);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        _channel = nullptr;
    }
    if (_session) {
        ssh_session session = static_cast<ssh_session>(_session);
        ssh_disconnect(session);
        ssh_free(session);
        _session = nullptr;
    }
}

bool SshClient::connected() const
{
    return _session && _channel &&
           !ssh_channel_is_closed(static_cast<ssh_channel>(_channel));
}

int SshClient::read(char* buffer, size_t len)
{
    if (!connected()) return -1;
    int n = ssh_channel_read_nonblocking(static_cast<ssh_channel>(_channel),
                                         buffer, len, 0);
    if (n == SSH_AGAIN) return 0;
    return n == SSH_ERROR ? -1 : n;
}

bool SshClient::write(const uint8_t* data, size_t len)
{
    if (!connected()) return false;
    ssh_session session = static_cast<ssh_session>(_session);
    ssh_channel channel = static_cast<ssh_channel>(_channel);
    ssh_set_blocking(session, 0);
    size_t sent = 0;
    int64_t start = esp_timer_get_time() / 1000;
    while (sent < len && esp_timer_get_time() / 1000 - start < 30) {
        int n = ssh_channel_write(channel, data + sent, len - sent);
        if (n == SSH_AGAIN) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return sent == len;
}

bool SshClient::resizePty(int columns, int rows)
{
    if (!connected()) return false;
    return ssh_channel_change_pty_size(static_cast<ssh_channel>(_channel),
                                       columns, rows) == SSH_OK;
}

#else  // 桌面 stub

bool SshClient::connect(const SshProfile&, std::string& error, int, int)
{ error = "libssh 仅真机组件"; return false; }
void SshClient::disconnect() {}
bool SshClient::connected() const { return false; }
int  SshClient::read(char*, size_t) { return -1; }
bool SshClient::write(const uint8_t*, size_t) { return false; }
bool SshClient::resizePty(int, int) { return false; }

#endif
