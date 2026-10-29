#pragma once
/*
 * ssh_client — libssh 薄封装 (移植自 Tab5_SSH_Client 的 SshClient.cpp,
 * Arduino String → std::string, millis/delay → esp_timer/vTaskDelay)。
 * 只做客户端: 密码认证 + PTY shell + 非阻塞读写 + resize。
 * SCP/agent 未移植 (Phase 3)。桌面模拟器为 stub。
 *
 * 线程约束: libssh 0.11 vendored 版的锁后端是 threads/noop.c(无锁),
 * 同一 session 的 read/write 必须在同一个任务里串行 —— app_ssh 的会话
 * 任务单循环同时做收包与按键写入, 键盘事件经 FreeRTOS 队列传进来。
 */
#include <string>

struct SshProfile {
    std::string host;
    std::string user;
    std::string password;
    int port = 22;
};

class SshClient {
public:
    bool connect(const SshProfile& profile, std::string& error,
                 int columns = 91, int rows = 25);
    void disconnect();
    bool connected() const;
    int  read(char* buffer, size_t len);       // >=0 字节数, 0=暂无
    bool write(const uint8_t* data, size_t len);
    bool resizePty(int columns, int rows);

private:
#ifndef PLATFORM_BUILD_DESKTOP
    void* _session = nullptr;
    void* _channel = nullptr;
#endif
};
