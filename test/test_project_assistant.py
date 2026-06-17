import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GATEWAY = ROOT / "scripts" / "claude_project_gateway.py"
INSTALLER = ROOT / "app" / "apps" / "app_installer.h"
REVIEW_SCRIPT = ROOT / "scripts" / "project_request_review.py"


def load_gateway():
    spec = importlib.util.spec_from_file_location("claude_project_gateway", GATEWAY)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class ProjectAssistantGatewayTests(unittest.TestCase):
    def test_parse_request_accepts_text_and_project(self):
        gw = load_gateway()
        req = gw.parse_request(json.dumps({
            "text": "分析当前 M5Tab5 项目结构",
            "project": "M5Tab5",
            "mode": "ask",
        }).encode())
        self.assertEqual(req.text, "分析当前 M5Tab5 项目结构")
        self.assertEqual(req.project, "M5Tab5")
        self.assertEqual(req.mode, "ask")

    def test_parse_request_accepts_context_mode(self):
        gw = load_gateway()
        req = gw.parse_request(json.dumps({
            "text": "交代当前项目",
            "project": "M5Tab5",
            "mode": "context",
        }).encode())
        self.assertEqual(req.mode, "context")

    def test_parse_request_accepts_plan_mode(self):
        gw = load_gateway()
        req = gw.parse_request(json.dumps({
            "text": "制定下一步实现计划",
            "project": "M5Tab5",
            "mode": "plan",
        }).encode())
        self.assertEqual(req.mode, "plan")

    def test_parse_request_rejects_empty_text(self):
        gw = load_gateway()
        with self.assertRaisesRegex(ValueError, "text is required"):
            gw.parse_request(b'{"text":"   "}')

    def test_write_intent_is_blocked(self):
        gw = load_gateway()
        blocked = [
            "帮我修改代码并 commit",
            "run idf.py flash",
            "apply patch to app.cpp",
            "rm -rf build",
        ]
        for text in blocked:
            self.assertTrue(gw.is_write_intent(text), text)

    def test_read_only_prompt_mentions_project_and_safety(self):
        gw = load_gateway()
        prompt = gw.build_claude_prompt("分析入口", "M5Tab5")
        self.assertIn("M5Tab5", prompt)
        self.assertIn("read-only", prompt)
        self.assertIn("Do not modify files", prompt)
        self.assertIn("Do not use emoji", prompt)

    def test_context_prompt_includes_project_snapshot(self):
        gw = load_gateway()
        context = "Branch: main\nGit status:\n M file.cpp"
        prompt = gw.build_claude_prompt("交代项目", "M5Tab5", mode="context", context=context)
        self.assertIn("Project context snapshot", prompt)
        self.assertIn("Branch: main", prompt)
        self.assertIn("current project state, risks, and next steps", prompt)

    def test_plan_prompt_includes_context_and_plan_requirements(self):
        gw = load_gateway()
        context = "Branch: phase3\nGit status:\n M app.cpp"
        prompt = gw.build_claude_prompt("给我实施计划", "M5Tab5", mode="plan", context=context)
        self.assertIn("Project context snapshot", prompt)
        self.assertIn("Branch: phase3", prompt)
        self.assertIn("implementation plan", prompt)
        self.assertIn("files likely to change", prompt)
        self.assertIn("verification commands", prompt)
        self.assertIn("Do not execute the plan", prompt)

    def test_collect_project_context_is_bounded_and_read_only(self):
        gw = load_gateway()
        context = gw.collect_project_context()
        self.assertIn("Repository:", context)
        self.assertIn("Branch:", context)
        self.assertIn("Git status:", context)
        self.assertIn("Key files:", context)
        self.assertLessEqual(len(context), gw.MAX_CONTEXT_LEN)

    def test_context_gateway_default_timeout_allows_long_claude_analysis(self):
        gw = load_gateway()
        self.assertGreaterEqual(gw.DEFAULT_ASSISTANT_TIMEOUT, 180)
        self.assertGreater(gw.DEVICE_HTTP_TIMEOUT_SECONDS, gw.DEFAULT_ASSISTANT_TIMEOUT)

    def test_gateway_sanitizes_display_symbols(self):
        gw = load_gateway()
        self.assertEqual(gw.sanitize_display_text("你好 👋 ✅\ufe0f ok"), "你好 ok")

    def test_latest_result_is_empty_by_default(self):
        gw = load_gateway()
        with tempfile.TemporaryDirectory() as tmp:
            gw.RESULTS_DIR = Path(tmp)
            gw.LATEST_RESULT_FILE = Path(tmp) / "latest.json"
            gw.LATEST_RESULT = None
            latest = gw.latest_result_response()
            self.assertTrue(latest["ok"])
            self.assertFalse(latest["has_result"])

    def test_record_latest_result_keeps_read_only_summary(self):
        gw = load_gateway()
        with tempfile.TemporaryDirectory() as tmp:
            gw.RESULTS_DIR = Path(tmp)
            gw.LATEST_RESULT_FILE = Path(tmp) / "latest.json"
            req = gw.AssistantRequest(text="给我计划", project="M5Tab5", mode="plan")
            gw.record_latest_result(req, "第一步: 写测试")
            latest = gw.latest_result_response()
            self.assertTrue(latest["ok"])
            self.assertTrue(latest["has_result"])
            self.assertEqual(latest["mode"], "plan")
            self.assertEqual(latest["project"], "M5Tab5")
            self.assertEqual(latest["text"], "给我计划")
            self.assertEqual(latest["reply"], "第一步: 写测试")
            self.assertIn("updated_at", latest)

    def test_latest_result_persists_to_disk_and_recovers_after_restart(self):
        gw = load_gateway()
        with tempfile.TemporaryDirectory() as tmp:
            gw.RESULTS_DIR = Path(tmp)
            gw.LATEST_RESULT_FILE = Path(tmp) / "latest.json"
            req = gw.AssistantRequest(text="查看计划", project="M5Tab5", mode="plan")

            gw.record_latest_result(req, "持久化计划")
            self.assertTrue(gw.LATEST_RESULT_FILE.exists())

            gw.LATEST_RESULT = None
            latest = gw.latest_result_response()
            self.assertTrue(latest["ok"])
            self.assertTrue(latest["has_result"])
            self.assertEqual(latest["reply"], "持久化计划")
            self.assertEqual(latest["mode"], "plan")

    def test_project_request_writes_pending_json_with_context_hash(self):
        gw = load_gateway()
        with tempfile.TemporaryDirectory() as tmp:
            gw.REQUESTS_DIR = Path(tmp)
            req = gw.parse_project_request(json.dumps({
                "text": "请排队实现 Project Assistant 的一个测试任务",
                "project": "M5Tab5",
                "mode": "request",
            }).encode())
            pending = gw.create_pending_request(req, context="Branch: phase3b\nGit status: clean")

            request_file = Path(pending["path"])
            self.assertTrue(request_file.exists())
            saved = json.loads(request_file.read_text())
            self.assertEqual(saved["id"], pending["id"])
            self.assertEqual(saved["status"], "pending")
            self.assertEqual(saved["project"], "M5Tab5")
            self.assertEqual(saved["mode"], "request")
            self.assertEqual(saved["text"], "请排队实现 Project Assistant 的一个测试任务")
            self.assertEqual(saved["context"], "Branch: phase3b\nGit status: clean")
            self.assertIn("context_hash", saved)
            self.assertIn("created_at", saved)

    def test_project_request_blocks_direct_execution_intents(self):
        gw = load_gateway()
        blocked = [
            "执行命令 git reset --hard",
            "run idf.py flash",
            "commit and git push",
        ]
        for text in blocked:
            req = gw.ProjectRequest(text=text, project="M5Tab5", mode="request")
            with self.assertRaises(PermissionError):
                gw.create_pending_request(req, context="ctx")

    def test_project_requests_can_be_listed_by_status_newest_first(self):
        gw = load_gateway()
        with tempfile.TemporaryDirectory() as tmp:
            gw.REQUESTS_DIR = Path(tmp)
            first = gw.create_pending_request(
                gw.ProjectRequest(text="第一个任务", project="M5Tab5"),
                context="ctx-one",
            )
            second = gw.create_pending_request(
                gw.ProjectRequest(text="第二个任务", project="M5Tab5"),
                context="ctx-two",
            )
            gw.update_project_request_status(first["id"], "rejected", note="skip")

            pending = gw.list_project_requests(status="pending")
            rejected = gw.list_project_requests(status="rejected")

            self.assertEqual([item["id"] for item in pending], [second["id"]])
            self.assertEqual([item["id"] for item in rejected], [first["id"]])

    def test_project_request_status_updates_write_audit_trail(self):
        gw = load_gateway()
        with tempfile.TemporaryDirectory() as tmp:
            gw.REQUESTS_DIR = Path(tmp)
            pending = gw.create_pending_request(
                gw.ProjectRequest(text="等待桌面确认", project="M5Tab5"),
                context="ctx",
            )

            approved = gw.update_project_request_status(
                pending["id"],
                "approved",
                note="用户确认",
                actor="desktop",
            )
            self.assertEqual(approved["status"], "approved")
            self.assertEqual(approved["audit"][-1]["status"], "approved")
            self.assertEqual(approved["audit"][-1]["note"], "用户确认")
            self.assertEqual(approved["audit"][-1]["actor"], "desktop")

            done = gw.update_project_request_status(pending["id"], "done", note="只标记完成")
            self.assertEqual(done["status"], "done")
            self.assertEqual(done["audit"][-1]["status"], "done")

    def test_project_request_status_rejects_invalid_transitions(self):
        gw = load_gateway()
        with tempfile.TemporaryDirectory() as tmp:
            gw.REQUESTS_DIR = Path(tmp)
            pending = gw.create_pending_request(
                gw.ProjectRequest(text="等待桌面确认", project="M5Tab5"),
                context="ctx",
            )

            with self.assertRaises(ValueError):
                gw.update_project_request_status(pending["id"], "done")
            gw.update_project_request_status(pending["id"], "rejected", note="no")
            with self.assertRaises(ValueError):
                gw.update_project_request_status(pending["id"], "approved")

    def test_execution_packet_requires_approved_request_and_writes_markdown(self):
        gw = load_gateway()
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            gw.REQUESTS_DIR = base / "requests"
            gw.EXECUTION_DIR = base / "execution"
            pending = gw.create_pending_request(
                gw.ProjectRequest(text="实现一个安全功能", project="M5Tab5"),
                context="Branch: phase4b\nGit status: clean",
            )

            with self.assertRaises(ValueError):
                gw.create_execution_packet(pending["id"])

            gw.update_project_request_status(pending["id"], "approved", note="确认执行")
            packet = gw.create_execution_packet(pending["id"])
            packet_path = Path(packet["packet_path"])
            self.assertTrue(packet_path.exists())
            content = packet_path.read_text()
            self.assertIn("实现一个安全功能", content)
            self.assertIn("Branch: phase4b", content)
            self.assertIn("Show diff before completion", content)
            self.assertIn("Run verification before completion", content)
            self.assertIn("Do not flash", content)
            updated = gw.load_project_request(pending["id"])
            self.assertEqual(updated["audit"][-1]["status"], "execution_packet")

    def test_record_request_result_marks_done_and_updates_latest_result(self):
        gw = load_gateway()
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            gw.REQUESTS_DIR = base / "requests"
            gw.RESULTS_DIR = base / "results"
            gw.LATEST_RESULT_FILE = gw.RESULTS_DIR / "latest.json"
            gw.LATEST_RESULT = None
            pending = gw.create_pending_request(
                gw.ProjectRequest(text="完成一个桌面任务", project="M5Tab5"),
                context="ctx",
            )
            gw.update_project_request_status(pending["id"], "approved", note="确认执行")

            result = gw.record_request_result(
                pending["id"],
                summary="已完成桌面确认任务",
                files_changed=["scripts/example.py"],
                verification=["python3 test/test_project_assistant.py PASS"],
                next_step="等待下一条请求",
            )

            self.assertEqual(result["status"], "done")
            self.assertEqual(result["result"]["summary"], "已完成桌面确认任务")
            self.assertEqual(result["audit"][-1]["status"], "done")
            latest = gw.latest_result_response()
            self.assertTrue(latest["has_result"])
            self.assertEqual(latest["mode"], "request_result")
            self.assertIn("已完成桌面确认任务", latest["reply"])

    def test_desktop_review_script_is_non_executing_approval_tool(self):
        self.assertTrue(REVIEW_SCRIPT.exists())
        src = REVIEW_SCRIPT.read_text()
        self.assertIn("def list_requests", src)
        self.assertIn("def approve_request", src)
        self.assertIn("def reject_request", src)
        self.assertIn("def mark_done", src)
        self.assertIn("def create_packet", src)
        self.assertIn("def record_result", src)
        self.assertIn("argparse", src)
        self.assertNotIn("subprocess", src)
        self.assertNotIn("os.system", src)
        self.assertNotIn("idf.py", src)
        self.assertNotIn("git push", src)

    def test_desktop_review_script_help_runs(self):
        proc = subprocess.run(
            [sys.executable, str(REVIEW_SCRIPT), "--help"],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0)
        self.assertIn("Review Tab5 Project Assistant requests", proc.stdout)
        self.assertIn("packet", proc.stdout)
        self.assertIn("result", proc.stdout)


class ProjectAssistantFirmwareTests(unittest.TestCase):
    def test_app_is_registered_on_home_screen(self):
        installer = INSTALLER.read_text()
        self.assertIn('#include "app_project_assistant/app_project_assistant.h"', installer)
        self.assertIn("std::make_unique<AppProjectAssistant>()", installer)
        self.assertIn('home->addApp("Claude"', installer)

    def test_desktop_http_bypasses_proxy_for_local_gateway(self):
        http = (ROOT / "platforms" / "desktop" / "hal" / "components" / "hal_http.cpp").read_text()
        self.assertIn("CURLOPT_NOPROXY", http)
        self.assertIn("127.0.0.1,localhost,192.168.0.0/16,10.0.0.0/8,172.16.0.0/12", http)

    def test_claude_gateway_posts_have_long_timeout(self):
        desktop_http = (ROOT / "platforms" / "desktop" / "hal" / "components" / "hal_http.cpp").read_text()
        esp_http = (ROOT / "platforms" / "tab5" / "main" / "hal" / "components" / "hal_http.cpp").read_text()
        self.assertIn("_is_claude_gateway_url", desktop_http)
        self.assertIn("CURLOPT_TIMEOUT, _is_claude_gateway_url(url) ? 240L : 8L", desktop_http)
        self.assertIn("_is_claude_gateway_url", esp_http)
        self.assertIn("_is_claude_gateway_url(url) ? 240000 : 10000", esp_http)

    def test_project_assistant_uses_xiaozhi_puhui_font(self):
        conf = (ROOT / "lv_conf.h").read_text()
        src = (ROOT / "app" / "apps" / "app_project_assistant" / "app_project_assistant.cpp").read_text()
        cmake = (ROOT / "platforms" / "tab5" / "main" / "CMakeLists.txt").read_text()
        desktop_cmake = (ROOT / "platforms" / "desktop" / "CMakeLists.txt").read_text()
        self.assertIn("#define LV_FONT_SIMSUN_16_CJK            1", conf)
        self.assertIn("#define LV_FONT_FMT_TXT_LARGE 1", conf)
        self.assertIn("LV_FONT_DECLARE(font_puhui_20_4)", src)
        self.assertIn("return &font_puhui_20_4", src)
        self.assertIn("78__xiaozhi-fonts", cmake)
        self.assertIn("font_puhui_20_4.c", desktop_cmake)
        self.assertIn("project_text_font()", src)
        self.assertIn("lv_obj_set_style_text_font(_input, project_text_font(), 0)", src)
        self.assertIn("lv_obj_set_style_text_font(_reply, project_text_font(), 0)", src)

    def test_project_assistant_keyboard_opens_on_input_double_click(self):
        src = (ROOT / "app" / "apps" / "app_project_assistant" / "app_project_assistant.cpp").read_text()
        header = (ROOT / "app" / "apps" / "app_project_assistant" / "app_project_assistant.h").read_text()
        self.assertIn("DOUBLE_CLICK_MS = 350", src)
        self.assertIn("REPLY_PANEL_KEYBOARD_HIDDEN_H = 360", src)
        self.assertIn("REPLY_PANEL_KEYBOARD_VISIBLE_H = 260", src)
        self.assertIn("lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN)", src)
        self.assertIn("uint32_t _last_input_click_ms = 0", header)
        self.assertIn("lv_obj_t* _reply_panel = nullptr", header)
        self.assertIn("code == LV_EVENT_CLICKED", src)
        self.assertIn("self->_last_input_click_ms != 0", src)
        self.assertIn("lv_obj_has_flag(self->_keyboard, LV_OBJ_FLAG_HIDDEN)", src)
        self.assertIn("self->_showKeyboard()", src)
        self.assertIn("self->_hideKeyboard()", src)
        self.assertIn("lv_obj_set_height(_reply_panel, REPLY_PANEL_KEYBOARD_VISIBLE_H)", src)
        self.assertIn("lv_obj_set_height(_reply_panel, REPLY_PANEL_KEYBOARD_HIDDEN_H)", src)
        self.assertNotIn("code == LV_EVENT_FOCUSED", src)

    def test_project_assistant_has_context_quick_action(self):
        src = (ROOT / "app" / "apps" / "app_project_assistant" / "app_project_assistant.cpp").read_text()
        self.assertIn('{"Context"', src)
        self.assertIn('"context"', src)
        self.assertIn("请根据当前项目上下文", src)
        self.assertIn("[this, text, mode]", src)
        self.assertIn('{"mode", mode}', src)

    def test_project_assistant_has_plan_and_latest_quick_actions(self):
        src = (ROOT / "app" / "apps" / "app_project_assistant" / "app_project_assistant.cpp").read_text()
        header = (ROOT / "app" / "apps" / "app_project_assistant" / "app_project_assistant.h").read_text()
        self.assertIn('{"Plan"', src)
        self.assertIn('"plan"', src)
        self.assertIn("实施计划", src)
        self.assertIn('{"Latest"', src)
        self.assertIn('"latest"', src)
        self.assertIn("/api/project/results/latest", src)
        self.assertIn("_latestResultUrl()", src)
        self.assertIn("_fetchLatestResult()", src)
        self.assertIn("void _fetchLatestResult();", header)

    def test_project_assistant_has_request_quick_action(self):
        src = (ROOT / "app" / "apps" / "app_project_assistant" / "app_project_assistant.cpp").read_text()
        header = (ROOT / "app" / "apps" / "app_project_assistant" / "app_project_assistant.h").read_text()
        self.assertIn('{"Request"', src)
        self.assertIn('"request"', src)
        self.assertIn("/api/project/request", src)
        self.assertIn("_projectRequestUrl()", src)
        self.assertIn("_submitProjectRequest(", src)
        self.assertIn("Pending request", src)
        self.assertIn("void _submitProjectRequest(const std::string& text);", header)

    def test_tab5_factory_partition_fits_project_assistant_font(self):
        partitions = (ROOT / "platforms" / "tab5" / "partitions.csv").read_text()
        self.assertIn("factory,app,factory,0x10000,11M,", partitions)


if __name__ == "__main__":
    unittest.main()
