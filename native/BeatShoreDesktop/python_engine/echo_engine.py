#!/usr/bin/env python3
"""Minimal proof-of-concept child process for PythonEngine.h/ChildProcessEngine.h --
NOT a real model, NOT wired into BeatShoreDesktop's actual request routing.
Its only job is to prove the IPC mechanism (line-based NDJSON over stdin/
stdout, same shape node's analyze.js already uses in production) genuinely
works end-to-end through a real python.exe, the way MockNodeProcess.exe
already proved it for a generic child process and analyze.js proved it for
Node specifically.

Protocol, deliberately matching the existing READY convention
(main.cpp's startAndValidateNode() checks for exactly this on Node's
first line too):
  On startup:        {"type": "READY"}
  Request:            {"type": "PING", "payload": <anything>}
  Response:            {"type": "PONG", "payload": <the same value echoed back>}
  Request:            {"type": "SHUTDOWN"}
  (process exits cleanly, code 0)

Run unbuffered (-u flag, or PYTHONUNBUFFERED=1) -- see PythonEngine.h's
own header comment for why: without it, print() output sits in an
internal buffer and never reaches the parent process's pipe read in real
time, since Python only line-buffers stdout when it detects an actual
terminal, never a redirected pipe.
"""
import json
import sys


def send(message: dict) -> None:
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()  # belt-and-suspenders even under -u


def main() -> int:
    send({"type": "READY"})

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            send({"type": "ERROR", "message": f"invalid JSON: {line!r}"})
            continue

        msg_type = msg.get("type")
        if msg_type == "PING":
            send({"type": "PONG", "payload": msg.get("payload")})
        elif msg_type == "SHUTDOWN":
            return 0
        else:
            send({"type": "ERROR", "message": f"unknown message type: {msg_type!r}"})

    return 0


if __name__ == "__main__":
    sys.exit(main())
