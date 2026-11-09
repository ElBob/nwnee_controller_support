#!/usr/bin/env python3
"""Make the A button use the picker's highlighted button, in a Deck layout that
deck_add_picker.py already changed, in place, keeping a backup.

  deck_a_confirms.py <controller_neptune.vdf>

A sends Enter, nwpad's confirm key: while the picker is open it uses the highlighted
button; otherwise Enter is the game's (the chat bar). In the Console Port layout A
was a raw controller button (xinput A), which the game never sees, so nothing is
lost. The right stick's click (also Enter) is left as deck_add_picker.py set it.
"""
import re
import shutil
import sys
import time

path = sys.argv[1]
text = open(path, encoding="utf-8").read()
OLD = '"xinput_button A, Character Panel, , "'
NEW = '"key_press RETURN, Picker: use, , "'
if text.count(OLD) != 1:
    sys.exit(f"expected one {OLD}, found {text.count(OLD)} (already changed, or another layout)")
shutil.copy2(path, f"{path}.bak-nwpad-a-{time.strftime('%Y%m%d%H%M%S')}")
text = text.replace(OLD, NEW)
text = re.sub(r'("revision"\t\t")(\d+)(")', lambda m: m.group(1) + str(int(m.group(2)) + 1) + m.group(3), text, count=1)
assert text.count("{") == text.count("}")
open(path, "w", encoding="utf-8").write(text)
print("A -> Enter (Picker: use)")
