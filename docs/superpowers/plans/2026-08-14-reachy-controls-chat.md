# Tab5 Reachy Controls and Chat Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add native Reachy audio controls, a dedicated robot-control tab, a chat-history tab, and confirmed XIAOZHI/QWEN switching to the existing Tab5 firmware.

**Architecture:** Extend the existing HAL with the single missing HTTP verb, then keep every YRobot route and payload explicit in `reachy_client.h`. `AppReachy` owns one detached worker, typed caches, seven LVGL tabs, and a small backend-switch state machine; workers never mutate LVGL directly.

**Tech Stack:** C++17, ESP-IDF 5.5.2, LVGL 9.2.2, Mooncake AppAbility, nlohmann/json, Python 3 standard-library `unittest`.

**Spec:** `docs/superpowers/specs/2026-08-14-reachy-controls-chat-design.md`

## Global Constraints

- Modify only `M5Tab5-UserDemo`; do not modify or deploy YRobot.
- Do not add third-party dependencies.
- Use only fixed YRobot paths and fixed JSON fields; never accept an arbitrary action, path, service, or backend.
- Run all HTTP calls through `GetHAL()->tryRunDetached()`; no synchronous HTTP on the LVGL thread.
- Keep at most one Reachy network worker active.
- Never update LVGL from a worker or after the app loses the foreground.
- QWEN failure must remain visible and must not silently switch to XIAOZHI.
- Mode switch, daemon sleep, and daemon restart require confirmation.
- Flash only `/dev/cu.usbmodem112401` after an ESP32-P4 probe and explicit user approval.

## File Map

- `app/hal/hal.h`: add the `httpPut` virtual boundary.
- `platforms/desktop/hal/hal_desktop.h`, `platforms/desktop/hal/hal_desktop.cpp`: libcurl PUT implementation.
- `platforms/tab5/main/hal/hal_esp32.h`, `platforms/tab5/main/hal/components/hal_http.cpp`: ESP-IDF PUT implementation.
- `app/apps/app_reachy/reachy_client.h`: typed control state, chat parsing, and fixed API calls.
- `app/apps/app_reachy/app_reachy.h`: seven-tab UI handles and async operation state.
- `app/apps/app_reachy/app_reachy.cpp`: audio/control/chat UI, confirmation flows, polling, and rendering.
- `test/test_reachy_http_put.py`: HTTP PUT implementation contract.
- `test/test_reachy_client_contract.py`: fixed routes, payloads, and parser fixtures.
- `test/test_reachy_controls_ui.py`: UI ownership and interaction contract.
- `test/test_reachy_chat.py`: chat filtering, ordering, and bounded history.
- `test/test_reachy_backend_switch.py`: confirmation and no-fallback state-machine contract.
- Existing `test/test_reachy_lifecycle.py` and `test/test_reachy_icon.py`: regression gates.

---

### Task 1: Add HTTP PUT to the HAL

**Files:**
- Modify: `app/hal/hal.h:389-404`
- Modify: `platforms/desktop/hal/hal_desktop.h:60-72`
- Modify: `platforms/desktop/hal/hal_desktop.cpp:220-295`
- Modify: `platforms/tab5/main/hal/hal_esp32.h:100-108`
- Modify: `platforms/tab5/main/hal/components/hal_http.cpp:36-110`
- Create: `test/test_reachy_http_put.py`

**Interfaces:**
- Produces: `HalBase::HttpResponse_t httpPut(const std::string&, const std::string&, const Headers&)`.
- Consumes: existing `HttpResponse_t`, header vector, libcurl, and `esp_http_client` behavior used by `httpPost`.

- [ ] **Step 1: Write the failing HAL contract test**

```python
def test_all_hal_layers_expose_put():
    assert "virtual HttpResponse_t httpPut" in HAL_BASE.read_text()
    assert "HttpResponse_t httpPut" in DESKTOP_HEADER.read_text()
    assert "HalDesktop::httpPut" in DESKTOP_SOURCE.read_text()
    assert "HttpResponse_t httpPut" in ESP_HEADER.read_text()
    assert "HalEsp32::httpPut" in ESP_SOURCE.read_text()

def test_put_uses_real_put_methods():
    assert "CURLOPT_CUSTOMREQUEST, \"PUT\"" in DESKTOP_SOURCE.read_text()
    assert "HTTP_METHOD_PUT" in ESP_SOURCE.read_text()
```

- [ ] **Step 2: Run the test and verify RED**

Run: `python3 test/test_reachy_http_put.py`

Expected: FAIL because `httpPut` is absent from the base HAL and both platforms.

- [ ] **Step 3: Add the minimal common interface**

```cpp
virtual HttpResponse_t httpPut(const std::string& url, const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& headers = {})
{
    return {};
}
```

- [ ] **Step 4: Implement desktop PUT using the existing POST shape**

Use the same URL, headers, response callback, body, timeout, status handling, and cleanup as `httpPost`; replace POST mode with:

```cpp
curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
curl_easy_setopt(curl, CURLOPT_POSTFIELDS, put_data.c_str());
curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)put_data.size());
```

- [ ] **Step 5: Implement ESP32 PUT using the existing POST shape**

Use the same event handler, headers, JSON content type, timeout, response status, and cleanup as `httpPost`; set:

```cpp
esp_http_client_set_method(client, HTTP_METHOD_PUT);
esp_http_client_set_post_field(client, put_data.data(), put_data.size());
```

- [ ] **Step 6: Verify GREEN and compile both declarations**

Run:

```bash
python3 test/test_reachy_http_put.py
cmake -S platforms/desktop -B platforms/desktop/build
cmake --build platforms/desktop/build -j2
```

Expected: test PASS, CMake configures the desktop target, and the desktop target builds.

- [ ] **Step 7: Commit the HAL slice**

```bash
git add app/hal/hal.h platforms/desktop/hal/hal_desktop.h \
  platforms/desktop/hal/hal_desktop.cpp platforms/tab5/main/hal/hal_esp32.h \
  platforms/tab5/main/hal/components/hal_http.cpp test/test_reachy_http_put.py
git commit -m "feat: add HTTP PUT to Tab5 HAL"
```

---

### Task 2: Add typed YRobot control and chat client APIs

**Files:**
- Modify: `app/apps/app_reachy/reachy_client.h:307-412`
- Create: `test/test_reachy_client_contract.py`
- Create: `test/test_reachy_chat.py`

**Interfaces:**
- Consumes: `GetHAL()->httpGet/httpPut/httpPost`, `nlohmann::json`.
- Produces: `ControlState`, `ChatMessage`, `fetchControlState()`, `fetchChat()`, `setVolume()`, `setMicEnabled()`, `setVad()`, `setBackend()`, `setVideoEnabled()`, `setVoice()`, `setCameraRunning()`, `daemonAction()`, and `restartYRobot()`.

- [ ] **Step 1: Write failing fixed-route and payload tests**

Use literal expectations so a wrong route or field fails:

```python
EXPECTED = {
    "setVolume": ("/api/volume", "percent"),
    "setMicEnabled": ("/api/audio/input", "enabled"),
    "setVad": ("/api/audio/vad", "rms_min"),
    "setBackend": ("/api/conversation/backend", "backend"),
    "setVideoEnabled": ("/api/conversation/video", "enabled"),
    "setVoice": ("/api/conversation/voice", "voice"),
    "setCameraRunning": ("/api/camera/state", "running"),
    "daemonAction": ("/api/reachy-daemon/action", "action"),
}
```

The test extracts each named function body and asserts it calls `httpPut` except `daemonAction`, which calls `httpPost`, and always includes `Content-Type: application/json`.

- [ ] **Step 2: Write failing chat fixture tests**

Use one literal response object containing XIAOZHI, QWEN, tool, and unrelated log lines. Expected messages:

```python
[
    ("user", "你好"),
    ("assistant", "你好呀"),
    ("user", "天气如何"),
    ("assistant", "今天晴朗"),
]
```

Assert chronological ordering and a maximum of six user turns plus their following assistant fragments.

- [ ] **Step 3: Run both tests and verify RED**

Run:

```bash
python3 test/test_reachy_client_contract.py
python3 test/test_reachy_chat.py
```

Expected: FAIL because the types and functions do not exist.

- [ ] **Step 4: Add typed state**

```cpp
struct ControlState {
    bool ok = false;
    std::string configured_backend;
    std::string running_backend;
    std::string connection_state;
    std::string backend_error;
    bool video_enabled = false;
    std::string configured_voice;
    std::vector<std::string> available_voices;
    bool camera_running = false;
};

struct ChatMessage {
    enum class Role { User, Assistant };
    Role role = Role::Assistant;
    std::string text;
};

struct OperationResult {
    bool ok = false;
    int status = 0;
    std::string error;
};
```

- [ ] **Step 5: Add exact write functions**

Each function serializes one JSON object and returns `OperationResult`. `daemonAction` validates the action before sending:

```cpp
if (action != "wake" && action != "sleep" && action != "restart")
    return {false, 0, "invalid daemon action"};
```

Do not add a generic URL or method parameter.

- [ ] **Step 6: Parse control and chat responses**

`fetchControlState()` performs GETs for backend, video, voice, and camera. `fetchChat(200, 6)` reads `result.logs[*].message`, extracts only the four approved markers, strips the prefix, drops empty text and `% tool...` assistant entries, preserves order, and retains the last six user-led turns.

- [ ] **Step 7: Verify GREEN**

Run:

```bash
python3 test/test_reachy_client_contract.py
python3 test/test_reachy_chat.py
python3 test/test_reachy_lifecycle.py
```

Expected: all PASS.

- [ ] **Step 8: Commit the client slice**

```bash
git add app/apps/app_reachy/reachy_client.h \
  test/test_reachy_client_contract.py test/test_reachy_chat.py
git commit -m "feat: add YRobot control and chat client"
```

---

### Task 3: Turn the Audio tab into native controls

**Files:**
- Modify: `app/apps/app_reachy/app_reachy.h:95-100,130-160`
- Modify: `app/apps/app_reachy/app_reachy.cpp:575-649`
- Create: `test/test_reachy_controls_ui.py`

**Interfaces:**
- Consumes: `fetchAudio()`, `setVolume(int)`, `setMicEnabled(bool)`, `setVad(float)`.
- Produces: `_queueOperation(Operation)`, `_renderAudio()`, `_audioEventCb(lv_event_t*)` and LVGL control handles.

- [ ] **Step 1: Write failing audio interaction tests**

Assert the Audio page uses `lv_slider` for volume and VAD, an `LV_EVENT_RELEASED` handler for each slider, and click handlers for Mic and mute. Assert it does not call `reachy_client` directly inside an LVGL callback; callbacks must enqueue an operation.

- [ ] **Step 2: Run and verify RED**

Run: `python3 test/test_reachy_controls_ui.py`

Expected: FAIL because the current Audio page has a read-only `lv_bar` and labels.

- [ ] **Step 3: Add the operation model**

```cpp
enum class OperationKind {
    None, SetVolume, SetMic, SetVad, SetBackend, SetVideo,
    SetVoice, SetCamera, DaemonWake, DaemonSleep, DaemonRestart
};

struct Operation {
    OperationKind kind = OperationKind::None;
    int int_value = 0;
    float float_value = 0.f;
    bool bool_value = false;
    std::string text_value;
};
```

Store one pending operation under the existing cache mutex and reject a new write while `_fetch_inflight` is true.

- [ ] **Step 4: Build the Audio controls**

- Volume slider range: 0–100.
- VAD slider range: 1–500, displayed and submitted as slider value divided by 1000.
- Mic and mute are buttons with explicit text state, not ambiguous icon-only controls.
- Track `_last_nonzero_volume`, updating it whenever a fetched or committed volume is greater than zero.

- [ ] **Step 5: Handle release and click events**

Slider value changes update only the adjacent label. `LV_EVENT_RELEASED` queues one operation. Mute queues 0 when current volume is nonzero; restore queues `_last_nonzero_volume` or 50.

- [ ] **Step 6: Execute writes in the existing detached worker**

When a pending operation exists, execute it before periodic reads, store `OperationResult`, then request one fresh `fetchAudio()` so rendered state always comes from YRobot.

- [ ] **Step 7: Verify GREEN**

Run:

```bash
python3 test/test_reachy_controls_ui.py
python3 test/test_reachy_lifecycle.py
```

Expected: PASS.

- [ ] **Step 8: Commit the Audio tab**

```bash
git add app/apps/app_reachy/app_reachy.h app/apps/app_reachy/app_reachy.cpp \
  test/test_reachy_controls_ui.py
git commit -m "feat: add Reachy audio controls"
```

---

### Task 4: Add the dedicated Control tab and confirmations

**Files:**
- Modify: `app/apps/app_reachy/app_reachy.h:55-160`
- Modify: `app/apps/app_reachy/app_reachy.cpp:180-250,650-820`
- Extend: `test/test_reachy_controls_ui.py`
- Create: `test/test_reachy_backend_switch.py`

**Interfaces:**
- Consumes: Task 2 control calls and Task 3 operation queue.
- Produces: seven-tab navigation, `_buildControlPage()`, `_renderControl()`, `_confirmOperation(Operation)`, and backend-switch progress state.

- [ ] **Step 1: Write failing seven-tab and safety tests**

Assert exact order:

```python
['状态', '运动', '音频', '控制', '聊天', '系统', '日志']
```

Assert backend, daemon sleep, and daemon restart route through `_confirmOperation`; wake, video, voice, and camera queue directly.

- [ ] **Step 2: Write failing backend state-machine tests**

Assert the code performs this ordered sequence:

```text
setBackend(target) -> restartYRobot() -> poll fetchStatus() ->
configured_backend == target && runtime_backend == target
```

Assert a 30-second timeout and no call that sets the opposite backend.

- [ ] **Step 3: Run and verify RED**

Run:

```bash
python3 test/test_reachy_controls_ui.py
python3 test/test_reachy_backend_switch.py
```

Expected: FAIL because the Control tab and switch state do not exist.

- [ ] **Step 4: Expand tab arrays from five to seven**

Change `Tab` to `Status, Motion, Audio, Control, Chat, System, Logs, Count`; size arrays from `(int)Tab::Count`, not literal 7, and build pages in the same order as labels.

- [ ] **Step 5: Build Control UI**

- Backend: two labeled buttons with configured and running state.
- Video and camera: labeled two-state buttons.
- Voice: LVGL dropdown populated only from `available_voices`; selection submits an exact returned value.
- Daemon: three text buttons, with sleep/restart styled as destructive actions.
- A single status label reports `处理中`, success, restart progress, timeout, or server error.

- [ ] **Step 6: Reuse one confirmation dialog**

Store the pending confirmed operation in the app. Confirm queues it; cancel clears it. The dialog message names the target, for example `切换到 QWEN 并重启 YRobot？` or `让 Reachy Daemon 休眠？`.

- [ ] **Step 7: Implement backend switch polling inside one worker**

After save and restart succeed, poll `/api/status` at one-second intervals for at most 30 seconds. Do not hold an LVGL lock. Store the final configured backend, running backend, connection state, and error for the UI thread.

- [ ] **Step 8: Verify GREEN**

Run:

```bash
python3 test/test_reachy_controls_ui.py
python3 test/test_reachy_backend_switch.py
python3 test/test_reachy_lifecycle.py
```

Expected: PASS.

- [ ] **Step 9: Commit the Control tab**

```bash
git add app/apps/app_reachy/app_reachy.h app/apps/app_reachy/app_reachy.cpp \
  test/test_reachy_controls_ui.py test/test_reachy_backend_switch.py
git commit -m "feat: add Reachy control tab"
```

---

### Task 5: Add bounded chat history UI

**Files:**
- Modify: `app/apps/app_reachy/app_reachy.h:100-160`
- Modify: `app/apps/app_reachy/app_reachy.cpp`
- Extend: `test/test_reachy_chat.py`

**Interfaces:**
- Consumes: `std::vector<reachy_client::ChatMessage> fetchChat(200, 6)`.
- Produces: `_buildChatPage()`, `_renderChat()`, `_chat_signature`, and a three-second active-tab poll.

- [ ] **Step 1: Write failing UI behavior tests**

Assert Chat has its own page, uses a scrollable message container, creates left/right bubbles according to role, calls `lv_obj_scroll_to_y(..., LV_ANIM_OFF)`, and compares a content signature before rebuilding.

- [ ] **Step 2: Run and verify RED**

Run: `python3 test/test_reachy_chat.py`

Expected: FAIL because no Chat page exists.

- [ ] **Step 3: Build the Chat page**

Use one scrollable column container. User bubbles align right with `C_CARD_PR`; assistant bubbles align left with `C_CARD`. Both use `zh_font_sm()`, wrap text, and cap width below the full page width.

- [ ] **Step 4: Add chat caching and redraw suppression**

Worker stores parsed messages. UI computes a signature from role plus text; if equal to `_chat_signature`, return without cleaning or rebuilding the container. After rebuild, scroll to the bottom.

- [ ] **Step 5: Add tab-specific poll interval**

Use 3000 ms only when `_active == Tab::Chat`; all other tabs retain 5000 ms. Non-active tabs never poll.

- [ ] **Step 6: Verify GREEN**

Run:

```bash
python3 test/test_reachy_chat.py
python3 test/test_reachy_lifecycle.py
```

Expected: PASS.

- [ ] **Step 7: Commit the Chat tab**

```bash
git add app/apps/app_reachy/app_reachy.h app/apps/app_reachy/app_reachy.cpp \
  test/test_reachy_chat.py
git commit -m "feat: add Reachy chat history tab"
```

---

### Task 6: Integrate lifecycle, error rendering, and focused regression gates

**Files:**
- Modify: `app/apps/app_reachy/app_reachy.h`
- Modify: `app/apps/app_reachy/app_reachy.cpp`
- Extend: `test/test_reachy_lifecycle.py`
- Extend: `test/test_reachy_backend_switch.py`

**Interfaces:**
- Consumes: all previous task interfaces.
- Produces: one coherent foreground-only scheduler and operation-result renderer.

- [ ] **Step 1: Add failing stale-result and serialization tests**

Assert every worker captures an `_open_generation` value and the UI applies its result only when the generation still matches and `lv_screen_active() == _scr`. Assert `_fetch_inflight` guards both reads and writes.

- [ ] **Step 2: Run and verify RED**

Run:

```bash
python3 test/test_reachy_lifecycle.py
python3 test/test_reachy_backend_switch.py
```

Expected: FAIL until generation and serialization guards exist.

- [ ] **Step 3: Add lifecycle generation**

Increment `_open_generation` in `onOpen` and before leaving via `_requestClose`. A worker copies the generation before starting; `onRunning` discards completion data from older generations.

- [ ] **Step 4: Centralize operation result display**

Use one `_operation_status` string plus success/error flag. Each active page renders it in its own status label; no worker calls `lv_label_set_text` or `lv_async_call` with `this`.

- [ ] **Step 5: Remove the old direct Logs worker callback**

Refactor `_renderLogs()` so its worker stores text in cache and the LVGL thread consumes it. This closes the remaining path that can touch `_lg_text` after the app leaves the foreground.

- [ ] **Step 6: Verify focused regression suite**

Run:

```bash
python3 test/test_reachy_http_put.py
python3 test/test_reachy_client_contract.py
python3 test/test_reachy_controls_ui.py
python3 test/test_reachy_chat.py
python3 test/test_reachy_backend_switch.py
python3 test/test_reachy_lifecycle.py
python3 test/test_reachy_icon.py
```

Expected: all PASS with zero failures.

- [ ] **Step 7: Commit lifecycle integration**

```bash
git add app/apps/app_reachy/app_reachy.h app/apps/app_reachy/app_reachy.cpp \
  test/test_reachy_lifecycle.py test/test_reachy_backend_switch.py
git commit -m "fix: serialize Reachy UI operations"
```

---

### Task 7: Build and perform live read-only API validation

**Files:**
- No production changes expected.
- Build artifact: `platforms/tab5/build/m5stack_tab5.bin`

**Interfaces:**
- Consumes: completed implementation.
- Produces: verified desktop/firmware build and a pre-flash evidence record.

- [ ] **Step 1: Verify current live API shapes without writing state**

Run:

```bash
curl -fsS --max-time 5 http://192.168.1.14:8042/api/status | jq '{ok, runtime:.status.runtime, conversation:.status.conversation, audio:.status.audio}'
curl -fsS --max-time 5 http://192.168.1.14:8042/api/conversation/voice | jq '{configured_voice, available_voices}'
curl -fsS --max-time 5 'http://192.168.1.14:8042/api/logs?filter=chat&limit=5' | jq '{logs}'
```

Expected: HTTP success; do not issue any PUT/POST during this step.

- [ ] **Step 2: Configure and build desktop target**

Run:

```bash
cmake -S platforms/desktop -B platforms/desktop/build
cmake --build platforms/desktop/build -j2
```

Expected: successful configure and link.

- [ ] **Step 3: Build ESP32-P4 firmware**

Run:

```bash
source ~/.local/bin/idf_env.sh
ninja -C platforms/tab5/build
```

Expected: `m5stack_tab5.bin` generated and partition size check passes. Report the remaining app-partition percentage.

- [ ] **Step 4: Run final clean checks**

Run:

```bash
git diff --check
python3 test/test_reachy_http_put.py
python3 test/test_reachy_client_contract.py
python3 test/test_reachy_controls_ui.py
python3 test/test_reachy_chat.py
python3 test/test_reachy_backend_switch.py
python3 test/test_reachy_lifecycle.py
python3 test/test_reachy_icon.py
```

Expected: zero failures and no whitespace errors.

- [ ] **Step 5: Commit any build-only correction separately**

If compilation required a source correction, rerun Steps 2–4 and commit only that correction with a message naming the corrected build contract. If no correction was needed, do not create an empty commit.

---

### Task 8: Flash and verify on the real Tab5

**Files:**
- Flash artifact: `platforms/tab5/build/m5stack_tab5.bin`

**Interfaces:**
- Consumes: user approval and verified firmware image.
- Produces: physical acceptance evidence.

- [ ] **Step 1: Ask for explicit flash approval**

State that app-partition flashing will reset the Tab5 and wait for an explicit yes.

- [ ] **Step 2: Probe the exact target**

Run:

```bash
source ~/.local/bin/idf_env.sh
python -m esptool --chip esp32p4 -p /dev/cu.usbmodem112401 chip_id
```

Expected: ESP32-P4. Stop if the port is absent or reports another chip.

- [ ] **Step 3: Flash only the app partition**

Run:

```bash
python -m esptool --chip esp32p4 -p /dev/cu.usbmodem112401 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x10000 platforms/tab5/build/m5stack_tab5.bin
```

Expected: write completes, hash verifies, and device hard-resets.

- [ ] **Step 4: Capture boot evidence**

Run: `idf.py -p /dev/cu.usbmodem112401 monitor`

Expected: app loads from `0x10000`, reaches `app_main()`, initializes display and touch, and shows no panic/reset loop. Exit monitor with Ctrl+].

- [ ] **Step 5: Perform physical acceptance**

Verify on device:

1. Open and exit Reachy repeatedly without flashing or lockout.
2. Change Mic, volume, mute, and VAD; compare with live Dashboard values.
3. Toggle video and camera; select a QWEN voice.
4. Confirm daemon wake is immediate and sleep/restart require confirmation.
5. Confirm XIAOZHI/QWEN change requires confirmation, restarts YRobot, and reports actual running backend.
6. Speak in both modes and confirm the Chat tab refreshes, filters tool lines, and keeps six turns.
7. Leave the app open and closed long enough to confirm no periodic screen flash.

- [ ] **Step 6: Report hardware and software evidence separately**

Report automated tests/build, flash hash/boot logs, and the user's physical UI confirmation as separate results. Do not claim physical success from build or serial output alone.
