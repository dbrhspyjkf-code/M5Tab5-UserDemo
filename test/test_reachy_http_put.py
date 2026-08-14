import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HAL_BASE = ROOT / "app/hal/hal.h"
DESKTOP_HEADER = ROOT / "platforms/desktop/hal/hal_desktop.h"
DESKTOP_SOURCE = ROOT / "platforms/desktop/hal/hal_desktop.cpp"
ESP_HEADER = ROOT / "platforms/tab5/main/hal/hal_esp32.h"
ESP_SOURCE = ROOT / "platforms/tab5/main/hal/components/hal_http.cpp"


class ReachyHttpPutTests(unittest.TestCase):
    def test_all_hal_layers_expose_put(self):
        self.assertIn("virtual HttpResponse_t httpPut", HAL_BASE.read_text())
        self.assertIn("HttpResponse_t httpPut", DESKTOP_HEADER.read_text())
        self.assertIn("HalDesktop::httpPut", DESKTOP_SOURCE.read_text())
        self.assertIn("HttpResponse_t httpPut", ESP_HEADER.read_text())
        self.assertIn("HalEsp32::httpPut", ESP_SOURCE.read_text())

    def test_put_uses_real_put_methods(self):
        self.assertIn('CURLOPT_CUSTOMREQUEST, "PUT"', DESKTOP_SOURCE.read_text())
        self.assertIn("HTTP_METHOD_PUT", ESP_SOURCE.read_text())


if __name__ == "__main__":
    unittest.main()
