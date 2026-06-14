import unittest
from pathlib import Path


BOARD = Path("platforms/tab5/components/xiaozhi_core/tab5_bridge_board.cc").read_text()
BOARD_H = Path("platforms/tab5/components/xiaozhi_core/tab5_bridge_board.h").read_text()
APP = Path("platforms/tab5/components/xiaozhi_core/xiaozhi-esp32/main/application.cc").read_text()
CMAKE = Path("platforms/tab5/components/xiaozhi_core/CMakeLists.txt").read_text()
LCD = Path("platforms/tab5/components/xiaozhi_core/tab5_bridge_lcd.cc").read_text()
LCD_H = Path("platforms/tab5/components/xiaozhi_core/tab5_bridge_lcd.h").read_text()


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


if __name__ == "__main__":
    unittest.main()
