#!/usr/bin/env python3
"""Create, bind or destroy a client-side NUI window through the control socket
(debug builds; quickbar plan). For iterating on layouts.

  tools/nui_try.py create TOKEN ID FILE.json     # window definition
  tools/nui_try.py bind TOKEN NAME 'JSON value'
  tools/nui_try.py destroy TOKEN
"""
import json
import os
import socket
import sys

SOCK = os.environ.get("NWPAD_SOCKET", f"/run/user/{os.getuid()}/nwpad.sock")


def call(req):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    s.sendall((json.dumps(req, separators=(",", ":")) + "\n").encode())
    data = b""
    while not data.endswith(b"\n"):
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    return json.loads(data)


def main():
    op = sys.argv[1]
    token = int(sys.argv[2], 0)
    if op == "create":
        definition = json.load(open(sys.argv[4]))
        r = call({"cmd": "nui_create", "token": token, "id": sys.argv[3],
                  "json": json.dumps(definition, separators=(",", ":"))})
    elif op == "bind":
        r = call({"cmd": "nui_bind", "token": token, "name": sys.argv[3], "json": sys.argv[4]})
    else:
        r = call({"cmd": "nui_destroy", "token": token})
    print(json.dumps(r))
    return 0 if r.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
