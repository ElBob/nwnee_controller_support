#!/usr/bin/env python3
"""Write nwpadtest.dlg, a conversation for nwpad's dialog tests (our own text, so it
can live in the repo): markup and tokens, a very long line, many replies, a long
reply, and an end. The shapes match the extremes of the official campaigns
(dialog plan: up to ~1000-character lines, 83 replies, 444-character replies).

  tools/make_test_dlg.py OUT.dlg
"""
import struct
import sys

# GFF field types
BYTE, DWORD, EXOSTRING, RESREF, LOCSTRING, LIST = 0, 4, 10, 11, 12, 15

LONG_LINE = " ".join(["This is sentence %d of a very long line, written to test how nwpad's conversation "
                      "window copes with text that runs on and on." % i for i in range(1, 9)])
LONG_REPLY = ("A long reply: " + "the quick brown fox jumps over the lazy dog, " * 9).strip(", ") + "."
MANY = 83


def node(text, links):
    return {"text": text, "links": links}


def build():
    # entries[i] = NPC lines; replies[j] = player lines; links are indices into the other list
    entries, replies = [], []

    def reply(text, to=None):
        replies.append(node(text, [] if to is None else [to]))
        return len(replies) - 1

    entries.append(None)  # 0: the start, filled below
    entries.append(node(LONG_LINE, [reply("Back.", 0)]))  # 1
    many = [reply("Option %d of %d." % (i + 1, MANY), 0) for i in range(MANY)]
    entries.append(node("Pick one of many.", many))  # 2
    entries.append(node("Short line, long reply below.", [reply(LONG_REPLY, 0), reply("Back.", 0)]))  # 3
    entries[0] = node("<StartAction>[Nods]</Start> Hello, <FirstName>. <StartCheck>[Persuade]</Start> "
                      "and <StartHighlight>highlighted</Start> text.",
                      [reply("The long line, please.", 1), reply("Many replies.", 2),
                       reply("<StartCheck>[Intimidate]</Start> A long reply.", 3), reply("Goodbye.")])
    return entries, replies


class Gff:
    def __init__(self):
        self.structs, self.fields, self.labels, self.data, self.indices, self.lists = [], [], [], b"", b"", b""

    def label(self, name):
        if name not in self.labels:
            self.labels.append(name)
        return self.labels.index(name)

    def struct(self, stype, fields):
        """fields: [(type, label, value)]; returns the struct index."""
        si = len(self.structs)
        self.structs.append(None)
        idx = []
        for ftype, name, value in fields:
            if ftype in (BYTE, DWORD):
                v = value
            elif ftype == EXOSTRING:
                b = value.encode("cp1252")
                v = len(self.data)
                self.data += struct.pack("<I", len(b)) + b
            elif ftype == RESREF:
                b = value.encode()
                v = len(self.data)
                self.data += struct.pack("<B", len(b)) + b
            elif ftype == LOCSTRING:
                b = value.encode("cp1252")
                body = struct.pack("<III", 0xFFFFFFFF, 1, 0) + struct.pack("<I", len(b)) + b
                v = len(self.data)
                self.data += struct.pack("<I", len(body)) + body
            elif ftype == LIST:
                v = len(self.lists)
                self.lists += struct.pack("<I", len(value)) + b"".join(struct.pack("<I", s) for s in value)
            self.fields.append(struct.pack("<III", ftype, self.label(name), v))
            idx.append(len(self.fields) - 1)
        if len(idx) == 1:
            data = idx[0]
        else:
            data = len(self.indices)
            self.indices += b"".join(struct.pack("<I", i) for i in idx)
        self.structs[si] = struct.pack("<III", stype, data, len(idx))
        return si

    def bytes(self, ftype):
        parts = [b"".join(self.structs), b"".join(self.fields),
                 b"".join(n.encode().ljust(16, b"\0") for n in self.labels), self.data, self.indices, self.lists]
        offset, header = 56, []
        for p, count in zip(parts, (len(self.structs), len(self.fields), len(self.labels), len(self.data),
                                    len(self.indices), len(self.lists))):
            header += [offset, count]
            offset += len(p)
        return ftype + b"V3.2" + struct.pack("<12I", *header) + b"".join(parts)


def links(g, targets):
    return [g.struct(0, [(DWORD, "Index", t), (RESREF, "Active", ""), (BYTE, "IsChild", 0)]) for t in targets]


def write(path):
    entries, replies = build()
    g = Gff()
    g.struct(0xFFFFFFFF, [])  # the top struct, filled in last (it must be struct 0)
    common = lambda text: [(DWORD, "Animation", 0), (BYTE, "AnimLoop", 1), (LOCSTRING, "Text", text),
                           (RESREF, "Script", ""), (DWORD, "Delay", 0xFFFFFFFF), (EXOSTRING, "Comment", ""),
                           (RESREF, "Sound", ""), (EXOSTRING, "Quest", "")]
    es = [g.struct(i, [(EXOSTRING, "Speaker", "")] + common(e["text"]) +
                   [(LIST, "RepliesList", links(g, e["links"]))]) for i, e in enumerate(entries)]
    rs = [g.struct(i, common(r["text"]) + [(LIST, "EntriesList", links(g, r["links"]))]) for i, r in enumerate(replies)]
    start = g.struct(0, [(DWORD, "Index", 0), (RESREF, "Active", "")])
    top = Gff()  # rebuild with the top struct first
    top.__dict__.update(g.__dict__)
    fields = [(DWORD, "DelayEntry", 0), (DWORD, "DelayReply", 0), (DWORD, "NumWords", 0),
              (RESREF, "EndConversation", ""), (RESREF, "EndConverAbort", ""), (BYTE, "PreventZoomIn", 0),
              (LIST, "EntryList", es), (LIST, "ReplyList", rs), (LIST, "StartingList", [start])]
    si = g.struct(0xFFFFFFFF, fields)
    g.structs[0], g.structs[si] = g.structs[si], None  # move the top struct into slot 0
    g.structs.pop()
    open(path, "wb").write(g.bytes(b"DLG "))


if __name__ == "__main__":
    write(sys.argv[1])
