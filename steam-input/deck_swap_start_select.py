#!/usr/bin/env python3
"""Swap what the Deck's two small buttons send, in a layout deck_pad_buttons.py already
converted, in place, keeping a backup.

  deck_swap_start_select.py <controller_neptune.vdf>

An earlier deck_pad_buttons.py sent button_menu as the pad's Start and button_escape as
Select, which came out the wrong way round on the Deck (Robert, 2026-11-10: Start should
show the game menu, Select the map). This swaps the two bindings in a layout converted by
that version; deck_pad_buttons.py now writes them the right way round. Run it with the game
closed.
"""
import re
import shutil
import sys
import time

if len(sys.argv) != 2:
    sys.exit(__doc__)
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
preset = text[text.index('\t"preset"'):]
m = re.search(r'\t\t\t"(\d+)"\t\t"switch active"', preset)
if not m:
    sys.exit("no active switch group in the preset")
start = re.search(r'\t"group"\n\t\{\n\t\t"id"\t\t"%s"\n' % m.group(1), text).start()
end = text.index("\n\t}\n", start) + 3
grp = text[start:end]


def block(name):
    b = re.search(r'\t\t\t"%s"\n\t\t\t\{.*?\n\t\t\t\}\n' % name, grp, re.S)
    if not b:
        sys.exit(f"{name} not found in the switch group")
    return b.group(0)


menu, view = block("button_menu"), block("button_escape")
want = {menu: "xinput_button START, , ", view: "xinput_button SELECT, , "}
for blk, binding in want.items():
    if blk.count('"%s"' % binding) != 1:
        sys.exit(f"expected {binding!r} in {blk.split(chr(10))[0].strip()} (run deck_pad_buttons.py first, "
                 "or it is already swapped: swapping back needs the other bindings)")
new_menu = menu.replace('"xinput_button START, , "', '"xinput_button SELECT, , "')
new_view = view.replace('"xinput_button SELECT, , "', '"xinput_button START, , "')
grp = grp.replace(menu, new_menu).replace(view, new_view)
shutil.copy2(path, f"{path}.bak-nwpad-swapstart-{time.strftime('%Y%m%d%H%M%S')}")
text = text[:start] + grp + text[end:]
text = re.sub(r'("revision"\t\t")(\d+)(")', lambda mm: mm.group(1) + str(int(mm.group(2)) + 1) + mm.group(3), text, count=1)
assert text.count("{") == text.count("}")
open(path, "w", encoding="utf-8").write(text)
print("button_menu -> SELECT, button_escape -> START")
