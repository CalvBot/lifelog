# The example command's script. lifelog writes one JSON object to standard input and reads the
# last line printed: {"ok": ..., "banner": ..., "message": ...}. Only the standard library is used.

import json
import sys


def answer(ok, banner="", message=""):
    print(json.dumps({"ok": ok, "banner": banner, "message": message}))


def main():
    request = json.load(sys.stdin)
    action = request["action"]
    slots = request["slots"]

    if action == "hello":
        name = slots.get("name", "").title()
        answer(True, banner=f"Hello, {name}!" if name else "Hello!")
    elif action == "echo":
        answer(True, banner=slots["text"].capitalize())
    else:
        answer(False, message=f"I don't know the action {action!r}")


main()
