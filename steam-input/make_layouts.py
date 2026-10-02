#!/usr/bin/env python3
"""Generate the nwpad Steam Input layouts (VDF).

  steam-input/make_layouts.py            # writes steam-input/*.vdf

The Xbox layout ports Robert's Deck layout, "Neverwinter Nights Console Port"
(workshop 3095778009), to the Xbox pad. It keeps its buttons, and both sticks
become gamepad joysticks, which nwpad needs: left stick moves, right stick
turns the camera. The Xbox pad has no trackpad, so holding LB turns the right
stick into the mouse (its click is the left click). RT opens nwpad's quickbar
picker (Scroll Lock), clicking the right stick uses the pick (Enter), View closes
it (Esc); LB and RB send [ and ] for its banks.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def q(s):
    return '"' + s + '"'


def kv(ind, k, v):
    return "\t" * ind + q(k) + "\t\t" + q(v) + "\n"


def block(ind, name, body):
    return "\t" * ind + q(name) + "\n" + "\t" * ind + "{\n" + body + "\t" * ind + "}\n"


def press(ind, source, binding):
    """One input with a Full_Press binding (or a list of them, all at once)."""
    bindings = binding if isinstance(binding, list) else [binding]
    act = block(ind + 3, "bindings", "".join(kv(ind + 4, "binding", b) for b in bindings))
    body = block(ind + 1, "activators", block(ind + 2, "Full_Press", act)) + block(ind + 1, "disabled_activators", "")
    return block(ind, source, body)


def group(gid, mode, inputs=(), settings=()):
    body = kv(2, "id", str(gid)) + kv(2, "mode", mode) + kv(2, "name", "") + kv(2, "description", "")
    body += block(2, "inputs", "".join(press(3, s, b) for s, b in inputs))
    if settings:
        body += block(2, "settings", "".join(kv(3, k, v) for k, v in settings))
    return block(1, "group", body)


def xbox():
    groups = [
        group(0, "four_buttons", [
            ("button_a", "key_press C, Character Panel, , "),
            ("button_b", "key_press B, Spellbook, , "),
            ("button_x", "key_press R, Rest, , "),
            ("button_y", "key_press I, Inventory, , "),
        ]),
        group(1, "dpad", [
            ("dpad_north", "key_press UP_ARROW, Zoom In, , "),
            ("dpad_south", "key_press DOWN_ARROW, Zoom Out, , "),
            ("dpad_east", "key_press J, Journal, , "),
            ("dpad_west", "key_press M, Show/Hide Map, , "),
        ]),
        # Both sticks as gamepad joysticks (a joystick_move group outputs the stick
        # on its own side). nwpad's quickbar picker, like Baldur's Gate 3's radial:
        # RT opens it (Scroll Lock), the right stick picks, clicking the right stick
        # uses the pick (Enter), View closes it (Esc). LS click is Space (pause).
        group(2, "joystick_move", [("click", "key_press SPACE, Pause, , ")]),
        group(3, "joystick_move", [("click", "key_press RETURN, Picker: use, , ")]),
        group(4, "trigger", [("click", "mouse_button RIGHT, , ")]),
        group(5, "trigger", [("click", "key_press SCROLL_LOCK, Quickbar picker, , ")]),
        group(6, "switches", [
            ("button_escape", "key_press ESCAPE, Esc, , "),
            ("button_menu", "key_press G, Play and Pause, , "),
            # While the picker is open, [ and ] change its bank (nwpad's picker-prev-key /
            # picker-next-key); otherwise they're the game's.
            ("left_bumper", ["mode_shift right_joystick 7", "key_press LEFT_BRACKET, Picker: previous bank, , "]),
            ("right_bumper", "key_press RIGHT_BRACKET, , "),
        ]),
        # Hold LB: the right stick is the mouse (the Deck's right trackpad).
        group(7, "joystick_mouse", [("click", "mouse_button LEFT, , ")],
              [("sensitivity", "118")]),
    ]
    preset = block(1, "preset", kv(2, "id", "0") + kv(2, "name", "Default") + block(2, "group_source_bindings", "".join([
        kv(3, "0", "button_diamond active"),
        kv(3, "1", "dpad active"),
        kv(3, "2", "joystick active"),
        kv(3, "3", "right_joystick active"),
        kv(3, "4", "left_trigger active"),
        kv(3, "5", "right_trigger active"),
        kv(3, "6", "switch active"),
        kv(3, "7", "right_joystick active modeshift"),
    ])))
    head = (kv(1, "version", "3") + kv(1, "revision", "3") +
            kv(1, "title", "NWN Console Port + nwpad (Xbox)") +
            kv(1, "description", "Left stick moves and right stick turns the camera through nwpad; "
                                 "hold LB to use the right stick as the mouse. Buttons from the "
                                 "Neverwinter Nights Console Port layout.") +
            kv(1, "creator", "76561198024955725") + kv(1, "controller_type", "controller_xboxone") +
            kv(1, "major_revision", "0") + kv(1, "minor_revision", "0"))
    settings = block(1, "settings", kv(2, "left_trackpad_mode", "0") + kv(2, "right_trackpad_mode", "0"))
    return '"controller_mappings"\n{\n' + head + "".join(groups) + preset + settings + "}\n"


def main():
    path = os.path.join(HERE, "nwpad_xbox.vdf")
    with open(path, "w") as f:
        f.write(xbox())
    print(f"wrote {path}")


if __name__ == "__main__":
    main()
