"""Quickbar reading (quickbar plan Q1, re-notes F31): the test character's buttons
come back with the type, name and icon the game shows."""


def test_quickbar_contents(ctl):
    q = ctl("quickbar")
    assert q["ok"], q
    assert q["bank"] == 0
    slots = {s["slot"]: s for s in q["slots"]}
    assert len(slots) == 36
    expect = {  # the test character's default quickbar (Contest Of Champions 0492)
        0: (7, "Attack", "IR_ATTACK"),
        2: (40, "Examine", "IR_EXAMINE"),
        3: (10, "Stealth Mode", "isk_movsilent"),
        4: (1, "Light Crossbow", ""),
        6: (1, "Potion of Cure Light Wounds", ""),
        7: (4, "Summon Familiar", "ife_familiar"),
        8: (2, "Mage Armor", "is_MageArm"),
        11: (2, "Light", "is_Light"),
        12: (2, "Ray of Frost", "is_RayFrost"),
    }
    for slot, (kind, name, icon) in expect.items():
        s = slots[slot]
        assert (s["type"], s["name"], s["icon"]) == (kind, name, icon), s
    assert slots[8]["data"] & 0xFFFF == 0x66  # Mage Armor's spell id
    assert sum(1 for s in slots.values() if s["type"] == 0) == 23
