#!/usr/bin/env python3
"""Lists every user-visible string of the UI that has no Turkish translation, and every translation that no longer matches any string.

The UI uses the English text as the translation key: T("...") and Tf("...") in src/ui/*.cpp, and a table of {"English", "Turkish"} pairs in
src/ui/i18n_tr.inc. This script reads both the way the C++ compiler does (adjacent literals are joined, escapes are decoded) so the keys
compare exactly.

  tools/check_i18n.py            report: untranslated strings, stale translations, format-specifier mismatches, dynamic T() calls
  tools/check_i18n.py --keys     print every key (one per line, JSON-escaped), for building the table
  tools/check_i18n.py --strict   exit status 1 when anything is missing or inconsistent (CI)
"""
import glob
import json
import os
import re
import signal
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "ui")
TABLE = os.path.join(SRC, "i18n_tr.inc")
# Files that hold translatable text without being UI pages: their N_("...") marks are read like T("..."). The controller files
# produce the explanations the UI shows for a disabled or refused setting (capability reasons, failure messages); the UI looks them up
# with TMsg(), which also matches the ones built with Fmt(N_("... %d ...")).
BACKEND = [os.path.join(ROOT, "src", "common", f) for f in
           ("controller.cpp", "ctl_cpu.cpp", "ctl_ec.cpp", "ctl_gpu.cpp", "ctl_misc.cpp", "ctl_storage.cpp")]
EXTRA = [os.path.join(ROOT, "src", "common", "keymap.cpp")] + BACKEND
SKIP = {"i18n.cpp"}   # the implementation itself

ESC = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "0": "\0", "a": "\a", "b": "\b", "f": "\f", "v": "\v", "?": "?"}


def decode_literal(body):
    """Decodes the inside of one C string literal into bytes (the source is UTF-8, so raw non-ASCII characters stay as they are)."""
    out = bytearray()
    i = 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out += c.encode("utf-8")
            i += 1
            continue
        i += 1
        n = body[i]
        if n == "x":
            j = i + 1
            while j < len(body) and body[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(int(body[i + 1:j], 16) & 0xFF)
            i = j
        elif n in "01234567":
            j = i
            while j < len(body) and j < i + 3 and body[j] in "01234567":
                j += 1
            out.append(int(body[i:j], 8) & 0xFF)
            i = j
        else:
            out += ESC.get(n, n).encode("utf-8")
            i += 1
    return bytes(out)


def skip_ws_comments(s, i):
    while i < len(s):
        if s[i].isspace():
            i += 1
        elif s.startswith("//", i):
            while i < len(s) and s[i] != "\n":
                i += 1
        elif s.startswith("/*", i):
            e = s.find("*/", i + 2)
            i = len(s) if e < 0 else e + 2
        else:
            break
    return i


def read_literals(s, i):
    """Reads adjacent string literals starting at s[i]; returns (list_of_bodies, index_after)."""
    bodies = []
    while True:
        i = skip_ws_comments(s, i)
        if i < len(s) and s[i] == '"':
            j = i + 1
            while j < len(s) and s[j] != '"':
                j += 2 if s[j] == "\\" else 1
            bodies.append(s[i + 1:j])
            i = j + 1
        else:
            return bodies, i


def strip_comments_keep_strings(s):
    return s  # comments are skipped while reading; text inside comments that looks like T("...") is rare and harmless


def arg_extent(text, i):
    """Index of the end of the first argument of a call whose '(' is at text[i-1]: stops at a top-level ',' or the closing ')'."""
    depth = 0
    j = i
    while j < len(text):
        c = text[j]
        if c == '"':
            j += 1
            while j < len(text) and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
        elif c == "'":
            j += 1
            while j < len(text) and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return j
            depth -= 1
        elif c == "," and depth == 0:
            return j
        j += 1
    return j


def literal_groups(arg):
    """Every run of adjacent string literals inside one argument expression, each joined into one key (so a ternary of two literals gives two keys)."""
    groups = []
    i = 0
    while i < len(arg):
        if arg[i] == '"':
            bodies, i = read_literals(arg, i)
            groups.append(b"".join(decode_literal(b) for b in bodies))
        elif arg[i] == "'":
            i += 1
            while i < len(arg) and arg[i] != "'":
                i += 2 if arg[i] == "\\" else 1
            i += 1
        else:
            i += 1
    return groups


def ui_keys():
    keys = {}      # key(bytes) -> list of "file:line"
    fmt = set()    # keys that are used as printf formats (Tf), so their format specifiers must match the translation
    dynamic = []   # T(...) whose argument holds no literal at all (the strings behind it must be marked with N_())
    marked = 0
    for path in sorted(glob.glob(os.path.join(SRC, "*.cpp"))) + EXTRA:
        if os.path.basename(path) in SKIP:
            continue
        rel = os.path.relpath(path, ROOT)
        text = open(path, encoding="utf-8").read()
        for m in re.finditer(r"(?<![A-Za-z0-9_.>])(Tf|T|N_)\(", text):
            line = text.count("\n", 0, m.start()) + 1
            before = text[max(0, m.start() - 24):m.start()]
            if re.search(r"(const\s+std::string&|float|double|int|auto|void)\s*$", before):
                continue            # a declaration or definition, e.g. "const std::string& T(const std::string& en)" or a local helper named T
            end = arg_extent(text, m.end())
            arg = text[m.end():end]
            groups = literal_groups(arg)
            if not groups:
                if m.group(1) != "N_":
                    dynamic.append((rel, line, text[m.start():m.start() + 70].replace("\n", " ")))
                continue
            if m.group(1) == "N_":
                marked += 1
            for g in groups:
                if g:
                    keys.setdefault(g, []).append("%s:%d" % (rel, line))
                    if m.group(1) == "Tf" or (path in BACKEND and b"%" in g):
                        fmt.add(g)   # a backend message with a specifier is a printf template (Fmt(N_("...")))
    return keys, dynamic, fmt


def table_pairs():
    text = open(TABLE, encoding="utf-8").read()
    pairs = {}
    dup = []
    i = 0
    while True:
        i = text.find("{", i)
        if i < 0:
            break
        en, j = read_literals(text, i + 1)
        j = skip_ws_comments(text, j)
        if not en or j >= len(text) or text[j] != ",":
            i += 1
            continue
        tr, j = read_literals(text, j + 1)
        if not tr:
            i += 1
            continue
        k = b"".join(decode_literal(b) for b in en)
        v = b"".join(decode_literal(b) for b in tr)
        if k in pairs:
            dup.append(k)
        pairs[k] = v
        i = j
    return pairs, dup


SPEC = re.compile(rb"%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:ll|l|h|z)?[diuxXfFeEgGcsp])")


def specs(b):
    return [s for s in SPEC.findall(b) if s != b"%%"]


def main():
    args = sys.argv[1:]
    keys, dynamic, fmt = ui_keys()
    if "--keys" in args:
        for k in sorted(keys):
            print(json.dumps(k.decode("utf-8"), ensure_ascii=False))
        return 0
    pairs, dup = table_pairs()
    missing = sorted(k for k in keys if k not in pairs)
    stale = sorted(k for k in pairs if k not in keys)
    # only strings that go through printf (Tf) have format specifiers; a plain T() text may contain a literal "%"
    bad = [k for k in keys if k in pairs and k in fmt and specs(k) != specs(pairs[k])]
    print("strings in the UI: %d, translated: %d, missing: %d, stale: %d" % (len(keys), len(keys) - len(missing), len(missing), len(stale)))
    for k in missing:
        print("  MISSING  %s   (%s)" % (json.dumps(k.decode("utf-8"), ensure_ascii=False), keys[k][0]))
    for k in stale:
        print("  STALE    %s" % json.dumps(k.decode("utf-8"), ensure_ascii=False))
    for k in bad:
        print("  FORMAT   %s -> %s" % (json.dumps(k.decode("utf-8"), ensure_ascii=False), json.dumps(pairs[k].decode("utf-8"), ensure_ascii=False)))
    for k in dup:
        print("  DUPLICATE %s" % json.dumps(k.decode("utf-8"), ensure_ascii=False))
    if dynamic:
        print("T() calls whose argument is not a plain literal (their strings must be listed in the table by hand):")
        for f, l, t in dynamic:
            print("  %s:%d  %s" % (f, l, t))
    if "--strict" in args and (missing or stale or bad or dup):
        return 1
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    sys.exit(main())
