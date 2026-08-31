#!/usr/bin/env python3
"""Dependency-free structural smoke test for a QML file.

Quickshell's QML dialect can't be linted in CI without pulling in the whole
shell, so this just catches the class of mistake that has actually broken this
widget before: unbalanced braces from a botched merge or hand edit. It is
string- and comment-aware so brace characters inside literals don't count.

Usage: qml-smoke.py <file.qml> [<file.qml> ...]
Exit code is non-zero if any file fails.
"""
import sys

PAIRS = {"{": "}", "(": ")", "[": "]"}
CLOSERS = {v: k for k, v in PAIRS.items()}


def check(path):
    src = open(path, encoding="utf-8").read()
    stack = []          # (opener_char, line)
    line = 1
    i = 0
    n = len(src)
    state = "code"       # code | line_comment | block_comment | dq | sq | template
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "\n":
            line += 1
            if state == "line_comment":
                state = "code"
            i += 1
            continue
        if state == "line_comment":
            i += 1
            continue
        if state == "block_comment":
            if c == "*" and nxt == "/":
                state = "code"
                i += 2
                continue
            i += 1
            continue
        if state in ("dq", "sq", "template"):
            quote = {"dq": '"', "sq": "'", "template": "`"}[state]
            if c == "\\":
                i += 2
                continue
            if c == quote:
                state = "code"
            i += 1
            continue
        # state == code
        if c == "/" and nxt == "/":
            state = "line_comment"
            i += 2
            continue
        if c == "/" and nxt == "*":
            state = "block_comment"
            i += 2
            continue
        if c == '"':
            state = "dq"
            i += 1
            continue
        if c == "'":
            state = "sq"
            i += 1
            continue
        if c == "`":
            state = "template"
            i += 1
            continue
        if c in PAIRS:
            stack.append((c, line))
        elif c in CLOSERS:
            if not stack:
                return f"{path}:{line}: unmatched closing '{c}'"
            opener, oline = stack.pop()
            if PAIRS[opener] != c:
                return (
                    f"{path}:{line}: '{c}' closes '{opener}' opened at "
                    f"line {oline}"
                )
        i += 1

    if state == "block_comment":
        return f"{path}: unterminated block comment"
    if state in ("dq", "sq", "template"):
        return f"{path}: unterminated string literal"
    if stack:
        opener, oline = stack[-1]
        return f"{path}: '{opener}' opened at line {oline} is never closed"
    return None


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    failed = False
    for path in argv[1:]:
        try:
            err = check(path)
        except OSError as exc:
            err = f"{path}: {exc}"
        if err:
            print(f"FAIL {err}")
            failed = True
        else:
            print(f"ok   {path}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
