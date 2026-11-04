#!/usr/bin/env python3
"""Read or set a Steam game's launch options in localconfig.vdf.

Steam must not be running: it rewrites localconfig.vdf on exit.

  set-launch-options.py APPID              print the current launch options
  set-launch-options.py APPID "OPTIONS"    set them ("" clears them)
"""
import glob
import os
import re
import sys

TOKEN = re.compile(r'"((?:[^"\\]|\\.)*)"|([{}])')


def find_app_block(text, appid):
    """Return (start, end) of the body of apps/<appid>, braces excluded."""
    path, last_key = [], None
    for m in TOKEN.finditer(text):
        if m.group(2) == "{":
            path.append(last_key)
            if len(path) >= 2 and path[-1] == appid and path[-2].lower() == "apps":
                depth, i = 1, m.end()
                while depth:
                    c = TOKEN.search(text, i)
                    if c.group(2) == "{":
                        depth += 1
                    elif c.group(2) == "}":
                        depth -= 1
                    i = c.end()
                return m.end(), c.start()
            last_key = None
        elif m.group(2) == "}":
            path.pop()
            last_key = None
        else:
            last_key = m.group(1) if last_key is None else None
    return None


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    appid = sys.argv[1]
    configs = glob.glob(os.path.expanduser(
        "~/Library/Application Support/Steam/userdata/*/config/localconfig.vdf"))
    if not configs:
        sys.exit("no localconfig.vdf found")
    for config in configs:
        text = open(config, encoding="utf-8").read()
        span = find_app_block(text, appid)
        if not span:
            print(f"{config}: app {appid} not found")
            continue
        start, end = span
        body = text[start:end]
        current = re.search(r'\n(\s*)"LaunchOptions"\s+"((?:[^"\\]|\\.)*)"', body)
        if len(sys.argv) == 2:
            print(current.group(2) if current else "")
            continue
        value = sys.argv[2].replace("\\", "\\\\").replace('"', '\\"')
        if current:
            body = body[:current.start()] + f'\n{current.group(1)}"LaunchOptions"\t\t"{value}"' + body[current.end():]
        else:
            indent = re.search(r'\n(\s*)"', body)
            indent = indent.group(1) if indent else "\t\t\t\t\t\t"
            body = f'\n{indent}"LaunchOptions"\t\t"{value}"' + body
        with open(config, "w", encoding="utf-8") as f:
            f.write(text[:start] + body + text[end:])
        print(f"{config}: launch options for {appid} set to: {sys.argv[2]}")


if __name__ == "__main__":
    main()
