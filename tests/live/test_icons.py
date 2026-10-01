"""Palette-texture item icons (re-notes F35): armor (CArmorIcon) and cloaks/helmets
(CLayeredIcon) are rendered by nwpad to ordinary images, served through the game's
resource manager, so NUI can draw them."""
import os
import time

from PIL import Image

# CreateItemOnObject fails in the test module; items are created at the player's feet,
# picked up and equipped. Magic items must be identified to be equipped.
EQUIP = ('object pc=GetFirstPC(); location l=GetLocation(pc);'
         ' object h=CreateObject(OBJECT_TYPE_ITEM,"nw_arhe001",l);'
         ' object c=CreateObject(OBJECT_TYPE_ITEM,"x2_it_mcloak001",l); SetIdentified(c,TRUE);'
         ' AssignCommand(pc, ActionPickUpItem(h)); AssignCommand(pc, ActionEquipItem(h, INVENTORY_SLOT_HEAD));'
         ' AssignCommand(pc, ActionPickUpItem(c)); AssignCommand(pc, ActionEquipItem(c, INVENTORY_SLOT_CLOAK));')
USERDIR = os.path.expanduser("~/.nwpad/userdir")


def _icon(ctl, bit):
    return ctl("equipped_icon", slot_bit=bit)["icon"]


def test_plt_icons_render_and_serve(game, ctl):
    armor = _icon(ctl, 0x2)  # the test character's own Performer's Outfit
    assert armor["class"] == "armor", armor
    ctl("script_chunk", code=EQUIP)
    deadline = time.monotonic() + 15
    while _icon(ctl, 0x40).get("class") != "layered":
        assert time.monotonic() < deadline, "helmet and cloak never equipped"
        time.sleep(0.5)
    for bit, kind, size in ((0x2, "armor", (64, 128)), (0x1, "layered", (64, 64)), (0x40, "layered", (64, 128))):
        icon = _icon(ctl, bit)
        assert icon["class"] == kind and icon["rendered"].startswith("nwpd"), icon
        assert icon["served"] == 18 + size[0] * size[1] * 4, icon  # TGA through CExoResMan::Get
        img = Image.open(os.path.join(USERDIR, "tempclient", "nwpadicons", icon["rendered"] + ".tga"))
        assert img.size == size, (icon, img.size)
        opaque = sum(img.convert("RGBA").getchannel("A").histogram()[1:])
        assert opaque > size[0] * size[1] // 5, (icon, opaque)  # a real picture, not blank
