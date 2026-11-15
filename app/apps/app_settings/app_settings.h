/**
 * @file app_settings.h
 * @brief "工具" (Tools) app. The former settings (volume/brightness/WiFi/HA)
 *        moved to the home status-bar popups; this app is now a tools launcher.
 *        First tool: a full-screen calculator.
 *
 * NOTE: the class/dir name stays AppSettings/app_settings to avoid churn in the
 * installer + build globs; only the user-facing name ("工具") and UI changed.
 */
#pragma once
#include <mooncake.h>
#include <smooth_lvgl.h>
#include <functional>
#include <string>
#include <atomic>
#include <mutex>
#include <vector>

// Single shared email cache, populated by the worker in
// AppSettings::fetchEmail() and read by both the email sub-page and the
// AppHome status bar icon. Lives at file scope so the static keyword isn't
// required on each declaration; definitions are in app_settings.cpp.
struct EmailFolderInfo {
    std::string name;
    int unread  = 0;
    int flagged = 0;
};

// 一封邮件 (8768 email-status-server 新 schema: 每文件夹最近 30 封带
// uid/seen/flagged — 与 car-dash 邮件面板同源同能力, 支持旗标操作)
struct EmailItem {
    std::string uid, from, subject, date, preview, folder;
    bool seen    = false;
    bool flagged = false;
};

class AppSettings : public mooncake::AppAbility {
public:
    AppSettings();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

    // Called when the user backs out, so the launcher can restore its screen.
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    // AppLoraChat 的 mooncake app id, 由 app_installer 注入.
    void setLoraChatAppId(int id) { _lora_chat_id = id; }

    // AppStocks (自选股) 的 mooncake app id, 由 app_installer 注入.
    void setStocksAppId(int id) { _stocks_id = id; }
    void openStocks();  // 工具页 tile → 打开自选股 app
    void setCalendarAppId(int id) { _calendar_id = id; }
    void openCalendar();  // 工具页 tile → 打开万年历 app

    // AppReachy (Reachy 仪表盘) 的 mooncake app id, 由 app_installer 注入.
    void setReachyAppId(int id) { _reachy_id = id; }
    void openReachy();  // 工具页 tile → 打开 Reachy app

    // AppRfid (Unit NFC 读写器) 的 mooncake app id, 由 app_installer 注入.
    void setRfidAppId(int id) { _rfid_id = id; }

    // AppSsh (SSH 终端) 的 mooncake app id, 由 app_installer 注入.
    void setSshAppId(int id) { _ssh_id = id; }

    // AppHealth (健康) 的 mooncake app id, 由 app_installer 注入.
    void setHealthAppId(int id) { _health_id = id; }
    void openHealth();  // 工具页 tile → 打开健康 app

    // AppLottery (双色球) 的 mooncake app id, 由 app_installer 注入.
    void setLotteryAppId(int id) { _lottery_id = id; }
    void openLottery();  // 工具页 tile → 打开双色球 app

    // AppGallery (图库) 的 mooncake app id, 由 app_installer 注入.
    void setGalleryAppId(int id) { _gallery_id = id; }
    void openGallery();  // 工具页 tile → 打开图库 app

    // AppTimer (计时器) 的 mooncake app id, 由 app_installer 注入.
    // (2026-11-05: 计时器入口从主页迁到工具页, 主页按钮位让给「想法」)
    void setTimerAppId(int id) { _timer_id = id; }
    void openTimer();    // 工具页 tile → 打开计时器 app

    // AppGames (游戏合集: 翻牌/星星/扫雷/数独) 的 mooncake app id.
    void setGamesAppId(int id) { _games_id = id; }
    void openGames();    // 工具页 tile → 打开游戏 app

    // AppYt (油管: Pi 中继音频版) 的 mooncake app id.
    void setYtAppId(int id) { _yt_id = id; }
    void openYt();       // 工具页 tile → 打开油管 app

    // ── Shared email cache (status-bar poll reads these; 旧状态页数据源) ──
    // Worker in fetchEmail() writes; the AppHome status bar polls every 120s
    // and reads email_unread_total to decide whether to show the icon.
    // (2026-11-15 邮件子页改五层直连客户端后, email_list 仅剩兜底缓存价值,
    //  fetchEmail 管线保留 — 主页信封角标 + 潜在回退路径仍依赖。)
    static std::atomic<int>  email_unread_total;   // total, from j["count"]
    static std::atomic<int>  email_flagged_total;  // 红旗总数, j["flagged_total"]
    static std::atomic<bool> email_updated;        // list changed → UI rebuild
    static std::atomic<bool> email_fetching;       // in-flight guard
    static std::atomic<bool> email_error;          // last fetch failed
    static std::string       email_error_msg;
    static std::mutex        email_list_mutex;     // guards email_list/email_folders
    static std::vector<EmailItem> email_list;      // parsed messages (全文件夹最近 30 封)
    static std::vector<EmailFolderInfo> email_folders;  // 文件夹行 (与 email_list 同序)

    // Kick off an async fetch from hermes :8768. In-flight safe (the second
    // call while one is running is a no-op). Safe to call from anywhere —
    // AppHome's status bar calls it on a 60s timer. Static (no instance state
    // — the worker only touches the static cache above).
    static void fetchEmail();

    // Public entry: jump straight to the email sub-page (used by the home
    // status-bar mail icon). No-op if the app hasn't been opened yet — caller
    // should mooncake::openApp(settings_id) first if needed.
    void openEmailPage();

    // ── Email client (五层直连, 2026-11-15) 公开数据类型 (匿名空间 helper 也要用) ──
    // (2026-11-15 晚: 用户定稿两列布局 — Folders 独立层取消, 文件夹按钮并入
    //  List 层左列; @uf 聚合夹取消, 未读/旗标单独夹)
    enum class MailLv : int { Acct, Login, List, Detail, Compose };
    struct MailAcct   { std::string email; int unread = 0; };
    struct MailFolder { std::string raw, name; int unread = -1, total = -1; bool virt = false; };
    struct MailMsg    { std::string uid, fld, fldn, from, subject, date, preview;
                        bool seen = false, flagged = false; };
    struct MailMeta   { std::string msg_id, refs, from_addr, date, subject; };

private:
    std::function<void()> _close_cb;
    int _lora_chat_id = -1;
    int _stocks_id    = -1;
    int _calendar_id  = -1;
    int _reachy_id    = -1;
    int _rfid_id      = -1;
    int _ssh_id       = -1;
    int _health_id    = -1;
    int _lottery_id   = -1;
    int _gallery_id   = -1;
    int _timer_id     = -1;
    int _games_id     = -1;
    int _yt_id        = -1;

    lv_indev_t* _gesture_indev = nullptr;
    lv_obj_t*   _scr        = nullptr;
    lv_obj_t*   _tools_page = nullptr;  // tools launcher (calculator tile)
    lv_obj_t*   _calc_page  = nullptr;  // calculator (built on first open)

    // ── Calculator ─────────────────────────────────────────────────────────────
    lv_obj_t*   _calc_expr_lbl = nullptr;  // small expression line
    lv_obj_t*   _calc_res_lbl  = nullptr;  // big result line
    std::string _entry = "0";   // number currently being typed
    double      _acc   = 0.0;   // accumulated left-hand value
    char        _op    = 0;     // pending operator: '+','-','*','/'  (0 = none)
    bool        _fresh = true;  // next digit starts a new entry
    std::string _lhs_disp;      // formatted lhs for the expression line

    // ── Currency converter (汇率) ───────────────────────────────────────────────
    lv_obj_t*   _fx_page    = nullptr;
    lv_obj_t*   _fx_from_dd = nullptr;
    lv_obj_t*   _fx_to_dd   = nullptr;
    lv_obj_t*   _fx_amt_lbl = nullptr;  // amount being entered (FROM)
    lv_obj_t*   _fx_res_lbl = nullptr;  // converted result (TO)
    lv_obj_t*   _fx_rate_lbl = nullptr; // "1 美元 = 7.25 人民币"
    std::string _fx_entry   = "1";      // amount string

    // ── Unit converter (单位换算) ────────────────────────────────────────────────
    lv_obj_t*   _unit_page    = nullptr;
    lv_obj_t*   _unit_cat_dd  = nullptr;  // category dropdown
    lv_obj_t*   _unit_from_dd = nullptr;  // from unit dropdown
    lv_obj_t*   _unit_to_dd   = nullptr;  // to unit dropdown
    lv_obj_t*   _unit_amt_lbl = nullptr;  // amount being entered (FROM)
    lv_obj_t*   _unit_res_lbl = nullptr;  // converted result (TO)
    lv_obj_t*   _unit_eq_lbl  = nullptr;  // "1 米 = 100 厘米"
    std::string _unit_entry   = "1";      // amount string
    int         _unit_cat_idx = 0;        // current category

    // ── Email client (邮件, 2026-11-15 oneapp 同款五层直连客户端) ──────────────
    // 协议层在 Pi email-status-server /api/mail/* (mail_client.py = oneapp
    // mail.py 移植): 多账号授权码 IMAP 收 + SMTP 发。固件只做层间导航 +
    // HTTP/JSON。主页信封角标仍走旧 fetchEmail() (/api/email/status) 不变。
    bool        _email_open_pending = false;  // app 未打开时点了主页邮件图标, onOpen 建屏后补开
    lv_obj_t*   _email_page   = nullptr;
    lv_obj_t*   _mail_title   = nullptr;    // 顶栏标题 (每层 set_text)
    lv_obj_t*   _mail_status  = nullptr;    // 顶栏状态/反馈行 (LONG_DOT)
    lv_obj_t*   _mail_actions = nullptr;    // 顶栏右侧动作钮容器 (每层重建)
    lv_obj_t*   _mail_content = nullptr;    // 层内容容器 (每层 lv_obj_clean 重建)
    lv_obj_t*   _mail_sidebar = nullptr;    // 左列文件夹按钮 (仅 List 层显示, 两列布局)
    lv_obj_t*   _mail_hint    = nullptr;    // 内容区居中提示 (加载中/失败/空)
    lv_obj_t*   _mail_kb      = nullptr;    // 软键盘 (login/compose, 惰性创建)

    MailLv      _mail_lv = MailLv::Acct;
    std::string _mail_acct;                 // 当前账号 email (登录后)
    std::string _mail_folder;               // 当前夹 raw 名 (@uf/@unread/@flagged/raw)
    std::string _mail_folder_disp;          // 当前夹显示名 (顶栏标题用)
    int         _mail_total = 0;            // 服务端命中总数 (N/M 封)
    bool        _mail_got = false;          // 当前夹收到过列表结果 (区分空/加载中)
    lv_obj_t*   _mail_ta_email = nullptr;   // 登录表单
    lv_obj_t*   _mail_ta_auth  = nullptr;
    lv_obj_t*   _mail_ta_to   = nullptr;    // 写信三字段
    lv_obj_t*   _mail_ta_subj = nullptr;
    lv_obj_t*   _mail_ta_body = nullptr;
    lv_obj_t*   _mail_body_lbl = nullptr;   // 详情正文 label (body done 后填文本)
    std::string _mail_quote;                // 回复引用块 (显示+发送拼接, 不可编辑)
    bool        _mail_reply = false;        // 回复态 (完成回 Detail, 新邮件回 List)
    bool        _mail_sending = false;      // SMTP 发送中 (busy 闸)
    std::string _mail_del_arm;              // 删除二段确认: 已点过一次的 email
    uint32_t    _mail_del_arm_ts = 0;
    lv_obj_t*   _mail_swipe_open = nullptr;   // 当前滑开的行 (单开原则)
    uint32_t    _mail_swipe_ts = 0;           // 滑开时刻 → 5s 无操作自动合拢
    lv_timer_t* _mail_swipe_timer = nullptr;  // 500ms 巡检自动合拢

    // Worker 结果 (fetchEmail 同款管线: worker 只写静态+翻旗, onRunning 消费)
    static std::mutex        mailc_mutex;
    static std::vector<MailAcct>   mailc_accts;      // 账号+角标
    static std::vector<MailFolder> mailc_folders;    // 虚拟夹置顶 + 真实夹
    static std::vector<MailMsg>    mailc_msgs;       // 当前夹列表
    static int      mailc_total;                     // 服务端命中总数
    static MailMsg  mailc_cur_msg;                   // 详情对应的信 (乐观旗标)
    static std::string mailc_body;                   // 详情正文
    static std::string mailc_body_uid, mailc_body_fld;  // 正文回执对号
    static MailMeta mailc_meta;                      // 回信元数据 (msg_id/refs/from_addr/date)
    static std::string mailc_err;                    // accts/folders/list 最近错误
    static std::string mailc_folders_email, mailc_list_folder;  // 回执对号 (快速换账号/夹弃旧结果)
    static std::string mailc_act_action, mailc_act_uid, mailc_act_fld;  // 失败回滚定位
    static std::string mailc_act_tip, mailc_act_err, mailc_send_err;
    static std::string mailc_login_email;
    static int      mailc_login_unread;
    static std::atomic<bool> mailc_accts_done, mailc_accts_ok;
    static std::atomic<bool> mailc_folders_done, mailc_folders_ok;
    static std::atomic<bool> mailc_list_done, mailc_list_ok;
    static std::atomic<bool> mailc_body_done, mailc_body_ok;
    static std::atomic<bool> mailc_act_done, mailc_act_ok;
    static std::atomic<bool> mailc_send_done, mailc_send_ok;
    static std::atomic<bool> mailc_login_done, mailc_login_ok;
    static std::atomic<bool> mailc_del_done, mailc_del_ok;

    void _buildToolsPage();
    void _openCalculator();
    void _closeCalculator();
    void _calcReset();
    void _calcInput(const char* key);
    void _calcRefresh();

    void _openFx();
    void _closeFx();
    void _fxInput(const char* key);
    void _fxRecompute();
    void _fxFetch();   // async pull of live rates from the HA server /rate proxy

    void _openUnit();
    void _closeUnit();
    void _unitInput(const char* key);
    void _unitRecompute();
    void _unitPopulateUnits();  // fill from/to dropdowns when category changes

    void _openEmail();
    void _closeEmail();
    void _mailRender();          // UI 线程: 按当前层重建 (顶栏动作 + 内容)
    void _mailRenderAcct();
    void _mailRenderLogin();
    void _mailRenderSidebar();   // 左列文件夹按钮 (List 层专属, 两列布局)
    void _mailRenderList();
    void _mailRenderDetail();
    void _mailRenderCompose();
    void _mailSetLv(MailLv lv);  // 切层 (收键盘) + 按需拉数
    void _mailEnterList(const std::string& raw, const std::string& disp);  // 选夹置状态 (不切层)
    MailLv _mailBackTarget(bool* close);  // 逐层回退目标; close=true=账号层(关邮件页)
    void _mailStatus(const std::string& s);
    void _mailKbShow(lv_obj_t* ta, bool voice);
    void _mailKbHide();
    // 事件回调里不许同步删事件目标的祖先 (_mail_content/_mail_actions 子树,
    // LVGL 事件分发期删目标=UAF, 万年历 dropdown 双删同源) — 层切换/重渲染/
    // 关页一律记 pending, 下一帧 onRunning 执行
    bool        _mail_pending = false;
    bool        _mail_pending_render = false;
    bool        _mail_pending_close = false;
    MailLv      _mail_pending_lv = MailLv::Acct;
    void _mailCollapseSwipe(bool anim);          // 合拢滑开的行 (切层/操作后/5s 超时)
    lv_obj_t* _mailMakeRow(const MailMsg& m, bool agg);  // 一行 = 左右操作区 + 可横滑面卡 (点面卡进详情; agg=聚合视图标来源夹)
    static void mailcFetchAccts();               // GET  /api/mail/accounts (角标)
    static void mailcLogin(const std::string& email, const std::string& auth);
    static void mailcFetchFolders(const std::string& email);
    static void mailcFetchList(const std::string& email, const std::string& folder);
    static void mailcFetchBody(const std::string& email, const std::string& folder,
                               const std::string& uid);
    static bool mailcAction(const std::string& email, const std::string& actions_csv,
                            const std::string& folder, const std::string& uid,
                            const char* tip);
    static void mailcSend(const std::string& email, const std::string& to,
                          const std::string& subject, const std::string& body,
                          const std::string& in_reply_to, const std::string& references);
    static void mailcDelAcct(const std::string& email);
    static void mailcApplyFlags(const std::string& actions_csv, const std::string& uid,
                                const std::string& fld, bool forward);  // 乐观旗标 (csv 多动作)
    static void mailc_apply_one(const std::string& action, const std::string& uid,
                                const std::string& fld, bool forward);
    void mail_do_action(const std::string& action, const std::string& uid,
                        const std::string& fld, const char* tip);  // 乐观+发 worker
    static void _mailBack_cb(lv_event_t* e);
    static void _mailRefreshBtn_cb(lv_event_t* e);
    static void _mailComposeBtn_cb(lv_event_t* e);
    static void _mailSendBtn_cb(lv_event_t* e);
    static void _mailAcctRow_cb(lv_event_t* e);      // 账号行 → 文件夹层
    static void _mailAcctDel_cb(lv_event_t* e);      // 删除 chip (二段确认)
    static void _mailAddAcct_cb(lv_event_t* e);      // ＋添加账号 → 登录层
    static void _mailLoginGo_cb(lv_event_t* e);      // 登录钮
    static void _mailFolderBtn_cb(lv_event_t* e);    // 左列文件夹按钮 → 切换右列列表
    static void _mailMsgOpen_cb(lv_event_t* e);      // 面卡 CLICKED → 详情层
    static void _mailMarkBtn_cb(lv_event_t* e);      // 详情: 标已读/未读 (乐观)
    static void _mailFlagBtn_cb(lv_event_t* e);      // 详情: 红旗/取旗 (乐观)
    static void _mailReplyBtn_cb(lv_event_t* e);     // 详情: 回复 → 写信层
    static void _mailTa_cb(lv_event_t* e);           // ta FOCUSED/DEFOCUSED → 软键盘+语音
    static void _mailKb_cb(lv_event_t* e);           // 软键盘 READY/CANCEL 收起
    static void _mailAct_cb(lv_event_t* e);
    static void _mailActDel_cb(lv_event_t* e);       // MailActCtx 释放
    static void _mailRowPressed_cb(lv_event_t* e);   // 按新行时合拢旧行 (单开)
    static void _mailRowScroll_cb(lv_event_t* e);    // SCROLL_END 吸合 + 单开登记
    static void _mailSwipeTimer_cb(lv_timer_t* e);

    void _installSwipeGesture();
    void _removeSwipeGesture();

    static void _toolBtn_cb(lv_event_t* e);  // tools tile → open calculator
    static void _calcKey_cb(lv_event_t* e);  // a calculator key
    static void _back_cb(lv_event_t* e);     // calculator → tools page
    static void _toolLoraChat_cb(lv_event_t* e);  // tools tile → open LoRa 聊天

    static void _toolFx_cb(lv_event_t* e);   // tools tile → open converter
    static void _fxKey_cb(lv_event_t* e);    // a converter keypad key
    static void _fxDd_cb(lv_event_t* e);     // currency dropdown changed
    static void _fxSwap_cb(lv_event_t* e);   // swap from/to
    static void _fxBack_cb(lv_event_t* e);   // converter → tools page

    static void _toolUnit_cb(lv_event_t* e); // tools tile → open unit converter
    static void _unitKey_cb(lv_event_t* e);  // a unit converter keypad key
    static void _unitCatDd_cb(lv_event_t* e); // category dropdown changed
    static void _unitFromDd_cb(lv_event_t* e); // from unit dropdown changed
    static void _unitToDd_cb(lv_event_t* e);   // to unit dropdown changed
    static void _unitSwap_cb(lv_event_t* e);   // swap from/to
    static void _unitBack_cb(lv_event_t* e);   // converter → tools page

    static void _toolMail_cb(lv_event_t* e);   // tools tile → open email page
    static void _toolStocks_cb(lv_event_t* e); // tools tile → open 自选股
    static void _toolCalendar_cb(lv_event_t* e); // tools tile → open 万年历
    static void _toolReachy_cb(lv_event_t* e); // tools tile → open Reachy app
    static void _toolRfid_cb(lv_event_t* e);   // tools tile → open RFID 读写器
    static void _toolSsh_cb(lv_event_t* e);    // tools tile → open SSH 终端
    static void _toolHealth_cb(lv_event_t* e); // tools tile → open 健康
    static void _toolLottery_cb(lv_event_t* e); // tools tile → open 双色球
    static void _toolGallery_cb(lv_event_t* e); // tools tile → open 图库
    static void _toolTimer_cb(lv_event_t* e);   // tools tile → open 计时器 (2026-11-05 自主页迁入)
    static void _toolGames_cb(lv_event_t* e);   // tools tile → open 游戏合集 (2026-11-06)
    static void _toolYt_cb(lv_event_t* e);      // tools tile → open 油管 (2026-11-06)
};
