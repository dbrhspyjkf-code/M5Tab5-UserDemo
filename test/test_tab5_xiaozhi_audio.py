import unittest
from pathlib import Path


BOARD = Path("platforms/tab5/components/xiaozhi_core/tab5_bridge_board.cc").read_text()
BOARD_H = Path("platforms/tab5/components/xiaozhi_core/tab5_bridge_board.h").read_text()
APP = Path("platforms/tab5/components/xiaozhi_core/xiaozhi-esp32/main/application.cc").read_text()
CMAKE = Path("platforms/tab5/components/xiaozhi_core/CMakeLists.txt").read_text()
LCD = Path("platforms/tab5/components/xiaozhi_core/tab5_bridge_lcd.cc").read_text()
LCD_H = Path("platforms/tab5/components/xiaozhi_core/tab5_bridge_lcd.h").read_text()
INSTALLER = Path("app/apps/app_installer.h").read_text()
IDLE_SCREEN_H = Path("app/apps/app_idle_screen/app_idle_screen.h").read_text()
IDLE_SCREEN = Path("app/apps/app_idle_screen/app_idle_screen.cpp").read_text()


class Tab5XiaoZhiAudioTests(unittest.TestCase):
    def test_audio_codec_does_not_claim_tab5_i2s_or_codec_hardware(self):
        self.assertIn("dummy_audio_codec.h", BOARD)
        self.assertIn("DummyAudioCodec", BOARD)
        self.assertIn("return &dummy_codec", BOARD)
        self.assertNotIn("Tab5AudioCodec", BOARD)
        self.assertNotIn("NoAudioCodecSimplex", BOARD)
        self.assertNotIn("i2c_master_probe", BOARD)

    def test_bridge_reuses_hosted_wifi_instead_of_starting_wifi_manager(self):
        self.assertIn("StartNetwork() override", BOARD_H)
        start_network = BOARD.split("void Tab5BridgeBoard::StartNetwork()", 1)[1].split("AudioCodec* Tab5BridgeBoard::GetAudioCodec()", 1)[0]
        self.assertIn("OnNetworkEvent(NetworkEvent::Connected", start_network)
        self.assertNotIn("WifiBoard::StartNetwork", start_network)

    def test_tab5_offline_mode_skips_xiaozhi_cloud_activation(self):
        self.assertIn("CONFIG_TAB5_XIAOZHI_OFFLINE_ONLY=1", CMAKE)
        self.assertIn("#if CONFIG_TAB5_XIAOZHI_OFFLINE_ONLY", APP)
        offline_block = APP.split("#if CONFIG_TAB5_XIAOZHI_OFFLINE_ONLY", 1)[1].split("#else", 1)[0]
        self.assertIn("offline mode", offline_block)
        activation_task = APP.split("void Application::ActivationTask()", 1)[1].split("void Application::CheckAssetsVersion()", 1)[0]
        self.assertIn("MAIN_EVENT_ACTIVATION_DONE", activation_task)
        self.assertNotIn("CheckNewVersion", offline_block)
        self.assertNotIn("InitializeProtocol", offline_block)

    def test_xiaozhi_lcd_setup_does_not_leave_bridge_screen_active(self):
        self.assertIn("void SetupUI() override", LCD_H)
        constructor = LCD.split("Tab5BridgeLcdDisplay::Tab5BridgeLcdDisplay", 1)[1].split("void Tab5BridgeLcdDisplay::SetupUI()", 1)[0]
        self.assertNotIn("lv_screen_load(scr_)", constructor)
        setup_ui = LCD.split("void Tab5BridgeLcdDisplay::SetupUI()", 1)[1].split("Tab5BridgeLcdDisplay::~Tab5BridgeLcdDisplay", 1)[0]
        self.assertIn("DisplayLockGuard lock(this)", setup_ui)
        self.assertIn("lv_screen_active()", setup_ui)
        self.assertIn("lv_screen_load(scr_)", setup_ui)
        self.assertIn("LcdDisplay::SetupUI()", setup_ui)
        self.assertIn("lv_screen_load(previous_screen)", setup_ui)

    def test_xiaozhi_lcd_screen_switches_are_lvgl_locked(self):
        constructor = LCD.split("Tab5BridgeLcdDisplay::Tab5BridgeLcdDisplay", 1)[1].split("void Tab5BridgeLcdDisplay::SetupUI()", 1)[0]
        activate_screen = LCD.split("void Tab5BridgeLcdDisplay::ActivateScreen()", 1)[1]
        self.assertIn("DisplayLockGuard lock(this)", constructor)
        self.assertIn("DisplayLockGuard lock(this)", activate_screen)

    def test_wake_word_rearm_does_not_recreate_afe_handle(self):
        audio_service = Path("platforms/tab5/components/xiaozhi_core/xiaozhi-esp32/main/audio/audio_service.cc").read_text()
        wake_word_h = Path("platforms/tab5/components/xiaozhi_core/xiaozhi-esp32/main/audio/wake_word.h").read_text()
        afe_wake_word = Path("platforms/tab5/components/xiaozhi_core/xiaozhi-esp32/main/audio/wake_words/afe_wake_word.cc").read_text()
        self.assertNotIn("ResetAfe", audio_service)
        self.assertNotIn("ResetAfe", wake_word_h)
        self.assertNotIn("void AfeWakeWord::ResetAfe", afe_wake_word)
        self.assertIn("wake_word_->Stop();", audio_service)
        self.assertIn("wake_word_->Start();", audio_service)

    def test_audio_service_init_does_not_start_full_pipeline(self):
        initialize = APP.split("void Application::Initialize()", 1)[1].split("void Application::Run()", 1)[0]
        self.assertIn("audio_service_.Initialize(codec);", initialize)
        self.assertNotIn("audio_service_.Start();", initialize)

        ctl = Path("platforms/tab5/components/xiaozhi_core/xiaozhi_ctl.cc").read_text()
        task_fn = ctl.split("static void xiaozhi_task_fn", 1)[1].split("extern \"C\" void xiaozhi_start_task", 1)[0]
        self.assertIn("Start(StartMode::kInputOnly)", task_fn)

    def test_opening_audio_channel_lazily_starts_output_pipeline(self):
        ctl = Path("platforms/tab5/components/xiaozhi_core/xiaozhi_ctl.cc").read_text()
        ensure_output = ctl.split("extern \"C\" void xiaozhi_ensure_output(void)", 1)[1].split("extern \"C\" void xiaozhi_stop_output(void)", 1)[0]
        self.assertIn("Start(StartMode::kAll)", ensure_output)

        continue_open = APP.split("void Application::ContinueOpenAudioChannel", 1)[1].split("void Application::HandleStartListeningEvent()", 1)[0]
        continue_wake = APP.split("void Application::ContinueWakeWordInvoke", 1)[1].split("void Application::HandleStateChangedEvent()", 1)[0]
        self.assertIn("xiaozhi_ensure_output();", continue_open)
        self.assertIn("xiaozhi_ensure_output();", continue_wake)

    def test_output_only_stop_does_not_lock_audio_queue_twice(self):
        audio_service = Path("platforms/tab5/components/xiaozhi_core/xiaozhi-esp32/main/audio/audio_service.cc").read_text()
        stop_fn = audio_service.split("void AudioService::Stop(StopMode mode)", 1)[1].split("void AudioService::SetMicEnabled", 1)[0]
        output_only = stop_fn.split("if (mode == StopMode::kOutputOnly)", 1)[1].split("return;", 1)[0]
        self.assertNotIn("lock2(audio_queue_mutex_)", output_only)

    def test_wake_word_dismisses_idle_screen_before_opening_xiaozhi(self):
        self.assertIn("void dismiss()", IDLE_SCREEN_H)
        dismiss = IDLE_SCREEN.split("void AppIdleScreen::dismiss()", 1)[1].split("void AppIdleScreen::onPause()", 1)[0]
        self.assertIn("lv_display_trigger_activity(NULL);", dismiss)
        self.assertIn("_hide();", dismiss)

        callback = INSTALLER.split("xiaozhi_register_wake_callback", 1)[1].split("// ── Register apps", 1)[0]
        self.assertIn("idle->dismiss();", callback)
        self.assertIn("openApp(ctx->xz_id)", callback)
        self.assertLess(callback.index("idle->dismiss();"),
                        callback.index("openApp(ctx->xz_id)"))


if __name__ == "__main__":
    unittest.main()
