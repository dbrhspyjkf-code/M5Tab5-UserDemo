#!/usr/bin/env python3
"""生成版本化资产文件名源文件。用法: gen_assets_name.py <name> <out.cc>"""
import sys
name, out = sys.argv[1], sys.argv[2]
open(out, "w").write(
    f'/* auto-generated — do not edit */\n'
    f'extern "C" const char* tab5_web_assets_name = "{name}";\n')
print(f"assets name: {name}")
