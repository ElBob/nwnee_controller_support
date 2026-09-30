#!/usr/bin/env python3
"""Convert a Deck copy of "Neverwinter Nights Console Port" (workshop 3095778009)
into an nwpad layout, in place, keeping a backup.

  deck_from_console_port.py <controller_neptune.vdf>

Both sticks become gamepad joysticks, which nwpad needs: the left stick uses the
layout's own (unused) joystick_move group, keeping the left stick's click binding,
and the right stick gets a new joystick_move group. Everything else is unchanged.
The workshop layout itself isn't in this repository; this only edits your copy.
Run it with Steam closed.
"""
import re
import shutil
import sys
import time

path = sys.argv[1]
text = open(path, encoding="utf-8").read()
if "+ nwpad" in text:
    sys.exit("already converted")
shutil.copy2(path, f"{path}.bak-nwpad-{time.strftime('%Y%m%d%H%M%S')}")


def group_span(gid):
    m = re.search(r'\t"group"\n\t\{\n\t\t"id"\t\t"%s"\n' % gid, text)
    if not m:
        sys.exit(f"group {gid} not found")
    return m.start(), text.index("\n\t}\n", m.start()) + 3


def preset_binding(source, state):
    """Group ids bound to e.g. ('joystick', 'active') in the preset."""
    p = text[text.index('\t"preset"'):]
    return re.findall(r'\t\t\t"(\d+)"\t\t"%s %s"' % (source, state), p)


left_old = preset_binding("joystick", "active")
right_old = preset_binding("right_joystick", "active")
move_groups = [g for g in re.findall(r'"id"\t\t"(\d+)"\n\t\t"mode"\t\t"joystick_move"', text)]
assert left_old and right_old and move_groups, (left_old, right_old, move_groups)
left_new = move_groups[0]

# Carry the old left stick's click binding over to the joystick_move group.
s, e = group_span(left_old[0])
click = re.search(r'\t\t\t"click"\n\t\t\t\{.*?\n\t\t\t\}\n', text[s:e], re.S)
s2, e2 = group_span(left_new)
grp = text[s2:e2]
if click:
    grp = re.sub(r'\t\t"inputs"\n\t\t\{\n\t\t\}\n', '\t\t"inputs"\n\t\t{\n' + click.group(0) + '\t\t}\n', grp)
text = text[:s2] + grp + text[e2:]

# A new joystick_move group for the right stick, after the last group.
ids = [int(g) for g in re.findall(r'\t"group"\n\t\{\n\t\t"id"\t\t"(\d+)"', text)]
right_new = str(max(ids) + 1)
last = text.rindex('\t"preset"')
new_group = ('\t"group"\n\t{\n\t\t"id"\t\t"%s"\n\t\t"mode"\t\t"joystick_move"\n\t\t"name"\t\t""\n'
             '\t\t"description"\t\t""\n\t\t"inputs"\n\t\t{\n\t\t}\n\t}\n' % right_new)
text = text[:last] + new_group + text[last:]

# Preset: the old stick groups go inactive, the joystick groups active.
p0 = text.index('\t"preset"')
pre, post = text[:p0], text[p0:]
post = post.replace('"%s"\t\t"joystick active"' % left_old[0], '"%s"\t\t"joystick inactive"' % left_old[0], 1)
post = post.replace('"%s"\t\t"joystick inactive"' % left_new, '"%s"\t\t"joystick active"' % left_new, 1)
post = post.replace('"%s"\t\t"right_joystick active"' % right_old[0], '"%s"\t\t"right_joystick inactive"' % right_old[0], 1)
post = re.sub(r'(\t\t"group_source_bindings"\n\t\t\{\n)', r'\1\t\t\t"%s"\t\t"right_joystick active"\n' % right_new, post, count=1)
text = pre + post

text = re.sub(r'("title"\t\t")([^"]*)(")', lambda m: m.group(1) + m.group(2) + " + nwpad" + m.group(3), text, count=1)
text = re.sub(r'("revision"\t\t")(\d+)(")', lambda m: m.group(1) + str(int(m.group(2)) + 1) + m.group(3), text, count=1)
assert text.count("{") == text.count("}")
open(path, "w", encoding="utf-8").write(text)
print(f"left stick: group {left_old[0]} -> {left_new} (joystick_move); "
      f"right stick: group {right_old[0]} -> {right_new} (new joystick_move)")
