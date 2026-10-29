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
