#!/usr/bin/env python3
"""Builds emoticons.pk3 for the chat emoticons (cl_emoticons 1).

Downloads the emoji from Google's Noto Emoji set, resizes them to 64x64 and writes a pk3
(a zip) with textures/emoticons/emoticons.json. Needs Pillow.

    python3 build_pack.py [output.pk3]

Put the result next to the other pk3 files, for example in the qw directory.
To add your own emoticon, add a PNG and an entry {"code": "name", "file": "name.png"}
to textures/emoticons/emoticons.json inside the pk3 (or a loose copy of those files).
"""
import io
import json
import sys
import urllib.request
import zipfile

from PIL import Image

BASE = "https://raw.githubusercontent.com/googlefonts/noto-emoji/main/2D/png/128/emoji_u%s.png"
SIZE = 64

# code -> (unicode codepoint, text triggers)
EMOTICONS = [
    ("smile", "1f642", [":)", ":-)"]), ("grin", "1f604", [":D", ":-D"]), ("wink", "1f609", [";)", ";-)"]),
    ("tongue", "1f61b", [":P", ":p", ":-P"]), ("sad", "1f641", [":(", ":-("]), ("cry", "1f622", [":'("]),
    ("heart", "2764", ["<3"]), ("surprised", "1f62e", [":O", ":o"]), ("cool", "1f60e", []),
    ("kiss", "1f618", [":*"]), ("neutral", "1f610", [":|"]), ("angry", "1f620", [">:("]),
    ("happy", "1f60a", ["^^"]), ("laugh", "1f606", ["XD", "xD"]), ("meh", "1f611", ["-_-"]),
    ("thumbsup", "1f44d", []), ("thumbsdown", "1f44e", []), ("clap", "1f44f", []), ("fire", "1f525", []),
    ("skull", "1f480", []), ("rocket", "1f680", []), ("bomb", "1f4a3", []), ("trophy", "1f3c6", []),
    ("hundred", "1f4af", []), ("eyes", "1f440", []), ("thinking", "1f914", []), ("rofl", "1f923", []),
    ("joy", "1f602", []), ("sob", "1f62d", []), ("sweat", "1f605", []), ("rage", "1f621", []),
    ("poop", "1f4a9", []), ("ghost", "1f47b", []), ("party", "1f389", []), ("crown", "1f451", []),
    ("zap", "26a1", []), ("snail", "1f40c", []), ("wave", "1f44b", []), ("muscle", "1f4aa", []),
    ("pray", "1f64f", []), ("ok", "1f44c", []), ("victory", "270c", []), ("facepalm", "1f926", []),
    ("shrug", "1f937", []), ("sleeping", "1f634", []), ("clown", "1f921", []), ("nerd", "1f913", []),
    ("devil", "1f608", []), ("angel", "1f607", []), ("hearteyes", "1f60d", []), ("confused", "1f615", []),
    ("scream", "1f631", []), ("wow", "1f929", []), ("unamused", "1f612", []), ("pensive", "1f614", []),
    ("hug", "1f917", []), ("gun", "1f52b", []), ("boom", "1f4a5", []), ("target", "1f3af", []),
    ("medal", "1f3c5", []), ("swords", "2694", []),
]

README = """Chat emoticons for unezQuake (cl_emoticons 1).
Images: Noto Emoji by Google, resized to 64x64.
  Images are under the Apache License 2.0, the fonts under the SIL Open Font License 1.1.
  https://github.com/googlefonts/noto-emoji
Type :name: in chat (for example :fire:). Text triggers such as :) are listed as "aliases".
The scoreboard flags (scoreboard_flags.pk3) are available as :br: :us: ... too.
"""


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "emoticons.pk3"
    entries = []
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for code, codepoint, aliases in EMOTICONS:
            with urllib.request.urlopen(BASE % codepoint) as r:
                image = Image.open(io.BytesIO(r.read())).convert("RGBA").resize((SIZE, SIZE), Image.LANCZOS)
            png = io.BytesIO()
            image.save(png, "PNG", optimize=True)
            z.writestr("textures/emoticons/%s.png" % code, png.getvalue())
            entry = {"code": code, "file": "%s.png" % code}
            if aliases:
                entry["aliases"] = aliases
            entries.append(entry)
        z.writestr("textures/emoticons/emoticons.json", json.dumps(entries, indent=1))
        z.writestr("textures/emoticons/README.txt", README)
    print("wrote %s with %d emoticons" % (out, len(entries)))


if __name__ == "__main__":
    main()
