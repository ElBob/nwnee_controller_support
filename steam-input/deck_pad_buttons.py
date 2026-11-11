#!/usr/bin/env python3
"""Pass LB, RB, A, B, Start, View, RT and the d-pad's left / right through as plain gamepad buttons in a Deck layout that
deck_add_picker.py and deck_a_confirms.py already changed, in place, keeping a backup.

  deck_pad_buttons.py <controller_neptune.vdf>           # buttons: nwpad reads the pad
  deck_pad_buttons.py --keys <controller_neptune.vdf>    # back to keystrokes

nwpad reads LB / RB (the picker's wheels) and A (use the highlighted button; answer in a
conversation) from the controller itself, so the layout needn't send [ ] and Enter.
View stays Escape: that is also the game's menu key, and nwpad's cancel works from it
either way. The keys keep working in nwpad whichever you choose. Run it with the game
closed.
"""
import re
import shutil
import sys
import time

args = [a for a in sys.argv[1:] if not a.startswith("--")]
keys = "--keys" in sys.argv[1:]
if len(args) != 1:
    sys.exit(__doc__)
path = args[0]
text = open(path, encoding="utf-8").read()

# (group source, input) -> (as a pad button, as a key)
CHANGES = [
    ("switch", "left_bumper", "xinput_button SHOULDER_LEFT, , ", "key_press LEFT_BRACKET, Picker: previous bank, , "),
    ("switch", "right_bumper", "xinput_button SHOULDER_RIGHT, , ", "key_press RIGHT_BRACKET, Picker: next bank, , "),
    ("button_diamond", "button_a", "xinput_button A, , ", "key_press RETURN, Picker: use, , "),
    # The rest of the pad scheme (README): B cancels, Start is the game menu, View the map,
    # RT the shortcuts wheel, the d-pad's left / right step through targets. Up / down stay
    # zoom keys. X and Y keep their key bindings.
    ("button_diamond", "button_b", "xinput_button B, , ", "key_press B, Spellbook, , "),
    ("switch", "button_menu", "xinput_button START, , ", "key_press G, Play and Pause, , "),
    ("switch", "button_escape", "xinput_button SELECT, , ", "key_press ESCAPE, Esc, , "),
    ("right_trigger", "click", "xinput_button TRIGGER_RIGHT, , ", "mouse_button RIGHT, Quickbar picker, , "),
    ("dpad", "dpad_east", "xinput_button DPAD_RIGHT, , ", "key_press J, Journal, , "),
    ("dpad", "dpad_west", "xinput_button DPAD_LEFT, , ", "key_press M, Show/Hide Map, , "),
]


def active_group(source):
    preset = text[text.index('\t"preset"'):]
    m = re.search(r'\t\t\t"(\d+)"\t\t"%s active"' % source, preset)
    if not m:
        sys.exit(f"no active {source} group in the preset")
    return m.group(1)


def group_span(gid):
    m = re.search(r'\t"group"\n\t\{\n\t\t"id"\t\t"%s"\n' % gid, text)
    if not m:
        sys.exit(f"group {gid} not found")
    return m.start(), text.index("\n\t}\n", m.start()) + 3


changed = []
for source, name, pad, key in CHANGES:
    old, new = (pad, key) if keys else (key, pad)
    s, e = group_span(active_group(source))
    grp = text[s:e]
    m = re.search(r'\t\t\t"%s"\n\t\t\t\{.*?\n\t\t\t\}\n' % name, grp, re.S)
    if not m:
        sys.exit(f"{name} not found in its group")
    blk = m.group(0)
    if blk.count('"%s"' % old) != 1:
        if blk.count('"%s"' % new) == 1:
            continue  # already done
        sys.exit(f"{name}: expected one {old!r} binding, found {re.findall(chr(34) + 'binding' + chr(34) + chr(9) + chr(9) + chr(34) + '([^' + chr(34) + ']*)', blk)}")
    grp = grp.replace(blk, blk.replace('"%s"' % old, '"%s"' % new))
    text = text[:s] + grp + text[e:]
    changed.append(f"{name}: {old.split(',')[0]} -> {new.split(',')[0]}")
if not changed:
    sys.exit("nothing to change (already " + ("keys" if keys else "pad buttons") + ")")
shutil.copy2(path, f"{path}.bak-nwpad-padbuttons-{time.strftime('%Y%m%d%H%M%S')}")
text = re.sub(r'("revision"\t\t")(\d+)(")', lambda m: m.group(1) + str(int(m.group(2)) + 1) + m.group(3), text, count=1)
assert text.count("{") == text.count("}")
open(path, "w", encoding="utf-8").write(text)
print("\n".join(changed))
