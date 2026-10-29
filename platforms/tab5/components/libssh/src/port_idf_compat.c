/* ESP-IDF 移植补丁 (2026-10-29 vendored):
 * - waitpid: IDF newlib 无此符号, esp32-arduino 由 syscalls 提供。设备上
 *   唯一引用方是 config.c ssh_exec_shell(config "exec" 指令) 与 socket.c
 *   proxy 子进程清理(被 ESP32 宏编掉) —— 运行时永不走到, 桩返回 -1。
 * - ESP32=1 编译定义在 CMakeLists 给全组件(与 ewpa/Arduino 环境一致),
 *   用于 socket.c 的 proxy 分支守卫。
 */
#include <sys/types.h>
#include <errno.h>

__attribute__((weak))
pid_t waitpid(pid_t pid, int *status, int options)
{
    (void)pid; (void)status; (void)options;
    errno = ECHILD;
    return -1;
}

/* POSIX 用户数据库/主机名桩(2026-10-29 Phase 2 链接补): libssh 用它们取
 * 默认用户名/主机名(设备无 /etc/passwd, 运行时不可达 —— app_ssh 显式
 * 设置 SSH_OPTIONS_USER/HOST, 这些只在不显式设置时才被调用)。
 * esp32-arduino 由其 syscalls 层提供同名桩, 纯 IDF 构建需自带。 */
#include <unistd.h>
#include <pwd.h>

__attribute__((weak)) uid_t getuid(void) { return 0; }

__attribute__((weak)) struct passwd* getpwnam(const char* name)
{
    (void)name;
    errno = ENOENT;
    return NULL;
}

__attribute__((weak)) int getpwuid_r(uid_t uid, struct passwd* pwd,
                                     char* buf, size_t buflen,
                                     struct passwd** result)
{
    (void)uid; (void)pwd; (void)buf; (void)buflen;
    if (result) *result = NULL;
    return ENOENT;
}

__attribute__((weak)) int gethostname(char* name, size_t len)
{
    (void)name; (void)len;
    errno = ENODEV;
    return -1;
}
