import importlib.util
import re
import unittest
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "var/reachy/icon_convert.py"
ICON = ROOT / "icon_tab5.png"
GENERATED = ROOT / "app/apps/app_reachy/reachy_icon.c"


class ReachyIconTests(unittest.TestCase):
    def test_generated_icon_matches_tab5_png(self):
        spec = importlib.util.spec_from_file_location("reachy_icon_convert", SCRIPT)
        converter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(converter)
        self.assertEqual(Path(converter.SRC), ICON)

        image = Image.open(ICON)
        image.thumbnail((converter.TILE, converter.TILE), Image.LANCZOS)
        expected = image.convert("RGBA").tobytes()

        source = GENERATED.read_text()
        pixel_array = source.split("reachy_icon_pixel_data[67600] = {", 1)[1]
        pixel_array = pixel_array.split("};", 1)[0]
        actual = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", pixel_array))
        self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main()
