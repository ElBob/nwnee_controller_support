#!/usr/bin/env python3
"""Add nwpad's quickbar-picker bindings to a Deck layout that
deck_from_console_port.py already converted, in place, keeping a backup.

  deck_add_picker.py <controller_neptune.vdf>

Same bindings as the Xbox layout (make_layouts.py):
  RT            Scroll Lock   open / close the picker
  LB / RB       [ and ]       previous / next quickbar bank (while it is open)
  Right stick   click: Enter  use the highlighted button (the chat bar otherwise)
View is already Escape in the Console Port layout (it closes the picker).
LT, LB and RB were xinput buttons, which the game never sees (nwpad filters
controller events), so nothing is lost. Run it with Steam closed.
"""
import re
import shutil
import sys
import time

path = sys.argv[1]
text = open(path, encoding="utf-8").read()
if "+ nwpad" not in text:
    sys.exit("run deck_from_console_port.py first")
if "SCROLL_LOCK" in text:
    sys.exit("already has a Scroll Lock binding")
shutil.copy2(path, f"{path}.bak-nwpad-picker-{time.strftime('%Y%m%d%H%M%S')}")


def group_span(gid):
    m = re.search(r'\t"group"\n\t\{\n\t\t"id"\t\t"%s"\n' % gid, text)
    if not m:
        sys.exit(f"group {gid} not found")
    return m.start(), text.index("\n\t}\n", m.start()) + 3


def active_group(source):
    preset = text[text.index('\t"preset"'):]
    m = re.search(r'\t\t\t"(\d+)"\t\t"%s active"' % source, preset)
    if not m:
        sys.exit(f"no active {source} group in the preset")
    return m.group(1)


def rebind(gid, old, new):
    """Replace one binding string inside group gid."""
    global text
    s, e = group_span(gid)
    grp = text[s:e]
    if grp.count(old) != 1:
        sys.exit(f"group {gid}: expected one {old}, found {grp.count(old)}")
    text = text[:s] + grp.replace(old, new) + text[e:]


trigger = active_group("right_trigger")
switch = active_group("switch")
left_stick = active_group("joystick")
right_stick = active_group("right_joystick")

rebind(trigger, '"xinput_button TRIGGER_RIGHT, , "', '"key_press SCROLL_LOCK, Quickbar picker, , "')
rebind(switch, '"xinput_button SHOULDER_LEFT, , "', '"key_press LEFT_BRACKET, Picker: previous bank, , "')
rebind(switch, '"xinput_button SHOULDER_RIGHT, , "', '"key_press RIGHT_BRACKET, Picker: next bank, , "')

# The right stick's click: the layout's own click block (the left stick's), with Enter.
s, e = group_span(left_stick)
click = re.search(r'\t\t\t"click"\n\t\t\t\{.*?\n\t\t\t\}\n', text[s:e], re.S)
if not click:
    sys.exit("the left stick has no click block to copy the format from")
block = re.sub(r'"key_press [A-Z_0-9]+, [^"]*"', '"key_press RETURN, Picker: use, , "', click.group(0))
s, e = group_span(right_stick)
grp = text[s:e]
empty = '\t\t"inputs"\n\t\t{\n\t\t}\n'
if empty not in grp:
    sys.exit(f"group {right_stick}: its inputs are not empty (the stick's click is bound already)")
text = text[:s] + grp.replace(empty, '\t\t"inputs"\n\t\t{\n' + block + '\t\t}\n') + text[e:]

text = re.sub(r'("revision"\t\t")(\d+)(")', lambda m: m.group(1) + str(int(m.group(2)) + 1) + m.group(3), text, count=1)
assert text.count("{") == text.count("}")
open(path, "w", encoding="utf-8").write(text)
print(f"RT (group {trigger}) -> Scroll Lock; LB / RB (group {switch}) -> [ / ]; "
      f"right stick click (group {right_stick}) -> Enter")
