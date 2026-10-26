#pragma once
#include "../utils/theme/theme.h"

/**
 * AppRfid — RFID Reader 工具 App, 读写 M5Stack Unit NFC U216 (ST25R3916, I2C 0x50).
 *
 * 接线: Unit NFC → Tab5 PORT A (Grove 红口, SDA=GPIO53, SCL=GPIO54, 5V).
 *   与「灯阵」/「自选股 LED」/「邮件 LED」共用 GPIO53, 打开时通过
 *   AppEmailLed::setPortAOwnedByApp(true) 让后台邮件通知器先释放, 关闭归还.
 *
 * 通信: 软件 I2C (bitbang) on GPIO53/54. 不用硬件控制器 —— ESP32-P4 只有
 *   2 个 HP I2C 控制器, 启动时已被内部总线 (port0, GPIO31/32) 和键盘
 *   (port1, GPIO0/1, 见 hal_tab5_kb) 占满, i2c_new_master_bus 报 "no free bus".
 *
 * 驱动: 移植自 M5Stack 官方 M5Unit-NFC 库 (unit_ST25R3916*), 精简为
 *   单卡 NFC-A 场景: REQA → 防冲突(无碰撞循环) → 选卡 →
 *   MIFARE Classic: 软件 Crypto1 三次握手认证 + 加密读写 (默认 KeyA FF×6)
 *   Ultralight/NTAG: 免认证 0x30 读 / 0xA2 页写
 *   保护: Classic 块 0 (厂商块) 与扇区尾块禁止写入
 *
 * 退出: 上拨手势 (与 自选股/灯阵/LoRa 一致). mooncake openApp(工具页) 不会
 * 触发本 app 的 onClose, 所以硬件收尾必须在 _requestClose 里显式做.
 */
#include <mooncake.h>
#include <lvgl.h>
#include <functional>
#include <string>
#include <atomic>
#include <memory>
#include <cstdint>
#include <cstring>

class AppRfid : public mooncake::AppAbility {
public:
    AppRfid();
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate()  override;
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;

private:
    // ── Layout / 配色 (与 app_stocks 一致) ───────────────────────────────────
    static constexpr int SCREEN_W = 1280;
    static constexpr int SCREEN_H = 720;
    static constexpr int HEADER_H = 72;
    // 配色: 实时读全局主题 (utils/theme/theme.h)
    static uint32_t C_BG()     { return 0x000000; }
    static uint32_t C_HEADER() { return theme::cur().card[1]; }
    static uint32_t C_TEXT()   { return theme::cur().text_hi; }
    static uint32_t C_DIM()    { return theme::cur().text_dim; }
    static uint32_t C_OK()     { return theme::cur().ok; }
    static uint32_t C_ERR()    { return theme::cur().danger; }

    static constexpr uint8_t NFC_I2C_ADDR = 0x50;  // Unit NFC U216 (ST25R3916)

    // ── State ────────────────────────────────────────────────────────────────
    std::function<void()> _close_cb;
    std::atomic<bool>     _app_open{false};
    bool                  _reader_ok    = false;
    bool                  _card_present = false;
    int                   _absent_streak = 0;      // 连续寻卡失败次数 (防抖)
    uint8_t               _uid[10]  = {0};
    int                   _uid_len  = 0;
    uint8_t               _sak = 0, _atqa[2] = {0, 0};
    int                   _block = 4;          // 当前选中的块地址
    bool                  _classic = false;    // 是否 MIFARE Classic (需要认证)

    // ── LVGL handles ─────────────────────────────────────────────────────────
    lv_obj_t*   _scr        = nullptr;
    lv_obj_t*   _status_lbl = nullptr;
    lv_obj_t*   _hint_lbl   = nullptr;
    lv_obj_t*   _uid_lbl    = nullptr;
    lv_obj_t*   _type_lbl   = nullptr;
    lv_obj_t*   _proto_lbl  = nullptr;
    lv_obj_t*   _block_lbl  = nullptr;
    lv_obj_t*   _data_ta    = nullptr;
    lv_obj_t*   _hex_lbl    = nullptr;
    lv_obj_t*   _msg_lbl    = nullptr;
    lv_obj_t*   _offuid_lbl = nullptr;  // "关灯卡: XX XX ..." 小标签
    lv_obj_t*   _kb         = nullptr;
    lv_timer_t* _poll_timer = nullptr;

    // Swipe-up-to-exit (与 app_stocks 相同的 alive-flag 防竞态模式)
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    lv_indev_t* _gesture_indev = nullptr;
    void*       _gesture_ctx   = nullptr;

    // ── Methods ──────────────────────────────────────────────────────────────
    void _buildUi();
    void _destroyUi();
    void _installSwipeGesture();
    void _removeSwipeGesture();
    void _requestClose();       // 退出唯一出口 (收尾必须在这里做)
    void _teardownHardware();   // 停轮询 + 释放 GPIO + 归还 PORT A (幂等)

    void _hwInit();             // GPIO 配置 + ST25R3916 初始化 + 开场
    void _setStatus(const char* text, bool ok);
    void _setMsg(const char* text, uint32_t color);
    void _clearCardInfo();
    void _showCardInfo();
    void _updateBlockLabel();
    void _readBlockNow();       // 读当前块 → hex/ascii 显示 + textarea
    void _writeBlockNow();      // textarea 内容 (≤16 字节) 写当前块
    const char* _cardTypeName();

    // ── 「嘀卡关灯」配置 (后台服务用, NVS 持久化) ────────────────────────────
    void _saveOffCard();        // 把当前卡 UID 追加进关灯卡列表 (NVS rfid_off_uids, 去重)
    void _clearOffCards();      // 清空关灯卡列表
    void _updateOffUidLabel();  // 刷新"关灯卡: N 张 ..."小标签

    // ── 关灯设备选择页 (拉 HA /api/states 里的 switch/light/fan, 勾选即存) ──
    struct PickerEntity { std::string entity_id; std::string name; bool checked; };
    lv_obj_t* _picker_page   = nullptr;
    lv_obj_t* _picker_list   = nullptr;   // 滚动容器, 每行一个 checkbox
    lv_obj_t* _picker_status = nullptr;
    std::vector<PickerEntity> _picker_entities;
    std::atomic<bool> _picker_updated{false};   // worker 拉完数据, 等 UI 重建
    std::atomic<bool> _picker_fetching{false};
    std::atomic<bool> _picker_error{false};

    void _openPicker();
    void _closePicker();
    void _fetchPickerEntities();     // detached worker: GET /api/states → _picker_entities
    void _rebuildPickerList();       // UI 线程按 _picker_entities 重建 checkbox 行
    void _savePickedEntities();      // 勾选状态 → NVS rfid_off_ents
    static void _pickerBtn_cb(lv_event_t* e);
    static void _pickerItem_cb(lv_event_t* e);

    static void _pollTick(lv_timer_t* t);          // 400ms 寻卡轮询
    static void _blockMinus_cb(lv_event_t* e);
    static void _blockPlus_cb(lv_event_t* e);
    static void _readBtn_cb(lv_event_t* e);
    static void _writeBtn_cb(lv_event_t* e);
    static void _clearBtn_cb(lv_event_t* e);
    static void _setOffCard_cb(lv_event_t* e);
    static void _clearOffCard_cb(lv_event_t* e);


#ifndef PLATFORM_BUILD_DESKTOP
    // ── ST25R3916 低层 (基于 bitbang I2C) ────────────────────────────────────
    // 操作码: 写寄存器 0x00|reg, 读寄存器 0x40|reg, 装 FIFO 0x80, 读 FIFO 0x9F,
    // Space-B 前缀 0xFB, 直接命令 0xC0|cmd (具体命令值见 cpp)
    bool    _stWriteA(uint8_t reg, uint8_t val);
    bool    _stReadA(uint8_t reg, uint8_t* out, size_t n);
    uint8_t _stReadA8(uint8_t reg);
    bool    _stWriteB(uint8_t reg, uint8_t val);
    bool    _stCmd(uint8_t cmd);
    bool    _stModifyA(uint8_t reg, uint8_t set_mask, uint8_t clear_mask);
    bool    _stSetBitsA(uint8_t reg, uint8_t bits)   { return _stModifyA(reg, bits, 0); }
    bool    _stClearBitsA(uint8_t reg, uint8_t bits) { return _stModifyA(reg, 0, bits); }
    void    _stClearIrqs();                          // 读 0x1A-0x1D 即清
    bool    _stFifoLoad(const uint8_t* data, size_t n);
    bool    _stFifoRead(uint8_t* out, size_t n);
    bool    _stFifoSize(uint16_t& bytes, uint8_t& bits);
    bool    _stWaitRxe(uint32_t timeout_ms, bool* collision = nullptr);
    void    _stWriteNtx(uint16_t bytes, uint8_t bits);  // 发送字节数/末字节位数
    void    _stWriteNrt(uint32_t ms);                   // no-response 定时器

    // ── 高层流程 ─────────────────────────────────────────────────────────────
    bool _stInit();        // 芯片初始化 (begin 精简版)
    bool _stConfigNfcA();  // 配置 ISO14443A 106k + 开 RF 场
    bool _stTransceive(const uint8_t* tx, size_t tx_n, uint8_t* rx, size_t rx_cap,
                       size_t* rx_n, uint8_t* rx_last_bits, uint32_t timeout_ms);  // 带 CRC
    bool _stReqWupa(uint8_t atqa[2], bool wupa);  // REQA/WUPA → ATQA
    bool _stSelectCard(uint8_t* uid, int* uid_len, uint8_t* sak);  // 防冲突(单卡)+选卡
    void _stHaltA();                               // HLTA, 让卡回 HALT 态 (下轮 WUPA 重唤醒)
    bool _wakeAndSelect();                         // 手动读写前确保卡已激活 (按需 WUPA+选卡)

    // MIFARE Classic (软件 Crypto1)
    bool _mfAuthKeyA(uint8_t block);
    bool _mfSendEncrypted(const uint8_t* tx, size_t tx_n);
    bool _mfTransceiveEncrypted(uint8_t* rx, size_t* rx_n, size_t rx_cap,
                                const uint8_t* tx, size_t tx_n,
                                uint32_t timeout_ms, bool include_crc, bool decrypt);

    // 块读写 (按 _classic 分发; Ultralight 读 16 字节/写 4×4 页)
    bool _cardReadBlock(uint8_t block, uint8_t out[16]);
    bool _cardWriteBlock(uint8_t block, const uint8_t data[16]);

    static void _crcA(const uint8_t* data, size_t n, uint8_t out[2]);  // ISO14443 CRC_A

    bool _encrypted = false;  // Crypto1 会话状态 (认证成功后为 true)
    bool _card_active = false;  // 卡处于 ACTIVE 态 (HLTA 后为 false)

    // ── 后台「嘀卡关灯」服务 (onCreate 启动, 与 UI 互斥使用硬件) ────────────
    // 架构与 M5Dial-UserDemo 的 rfid_service 一致: 开机常驻任务拥有读卡器,
    // 匹配 NVS "rfid_off_uids" 的卡进场时对 NVS "rfid_off_ents" (逗号分隔,
    // 缺省用内置默认列表) 逐个调 HA turn_off. UI 打开时通过 s_ui_owns 让位.
    // 注意: NVS 键名上限 15 字符, 超长的键读写会静默失败.
    static AppRfid* s_instance;
    static std::atomic<bool> s_ui_owns;    // true = UI 正在独占 GPIO/芯片
    static std::atomic<bool> s_bg_reinit;  // UI 关闭后置位, 后台任务重新 init 引脚
    void _bgStart();                       // 建后台任务 + 让邮件 LED 退休 PORT A
    static void _bgTaskEntry(void* arg);
    void _bgLoop();                        // 常驻寻卡 + 触发关灯 (不返回)
    bool _bgHwInit();                      // 后台路径的引脚配置 + 芯片初始化 (不碰 UI)
    void _bgTurnOffAll();                  // 匹配成功: 蜂鸣 + HA 逐个 turn_off
#endif
};
