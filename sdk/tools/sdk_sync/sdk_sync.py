#!/usr/bin/env python3
"""Keeps the sdk/ copies in registered game repos and the SDK master in step. Python 3.11+, stdlib only, never runs git.

  sdk_sync.py [--games sdk_games.toml] [--sdk <master>] report --out <folder>
  sdk_sync.py [...] upstream <game> [--force]
  sdk_sync.py [...] downstream <game> [--force]

report      diffs each game's sdk/ against the master, checks its pin, scans its sources and writes <folder>/sdk-sync-<date>.md
upstream    copies a game's sdk/ changes into the master working tree; game specific files and overrides need --force
downstream  copies master files into a game's sdk/ (not over newer game edits without --force) and pins its sdk.toml
"""
import argparse
import difflib
import hashlib
import re
import shutil
import sys
import tomllib
from datetime import date
from pathlib import Path

HERE = Path(__file__).resolve().parent
SKIP_DIRS = {".git", "__pycache__", "out", "build", ".vs", ".vscode", ".idea"}
SKIP_FILES = {"sdk_games.toml"}  # the local registry, not part of the SDK
SRC_EXT = {".cpp", ".cc", ".c", ".h", ".hpp", ".inl", ".cmake", ".py"}
OVERRIDE_RE = re.compile(r"(?:X86_FUNC\(\s*GUEST\(\s*|HLE_(?:STDCALL|FASTCALL|CDECL)\(\s*)(\w+)")
LOG_FIRST_RE = re.compile(r"\bLOG_FIRST\s*\(")
TEMP_RE = re.compile(r"\bTEMPORARY\b")
DIFF_LINES = 400  # per file in the report


def die(msg):
    print(msg, file=sys.stderr)
    sys.exit(1)


def load_toml(p):
    with open(p, "rb") as f:
        return tomllib.load(f)


def load_games(path):
    if not path.exists():
        die(f"{path} not found; copy sdk_games.example.toml to sdk_games.toml and edit it, or pass --games")
    games = []
    for g in load_toml(path).get("game", []):
        root = (path.parent / g["path"]).resolve()
        pin = load_toml(root / "sdk.toml") if (root / "sdk.toml").exists() else {}
        s = pin.get("sdk", {})
        games.append({
            "name": g["name"], "root": root, "images": g.get("images", []), "harness": g.get("harness", ""),
            "names": g.get("names", []) + [g["name"]], "pin_file": root / "sdk.toml", "has_pin": bool(pin),
            "pin": s.get("version", ""), "copy": root / s.get("path", "sdk"), "overrides": set(pin.get("overrides", [])),
        })
    return games


def pick(games, name):
    for g in games:
        if g["name"].lower() == name.lower():
            return g
    die(f"no game named {name}; registered: {', '.join(g['name'] for g in games)}")


def tree(root):
    out = {}
    if not root.is_dir():
        return out
    for p in root.rglob("*"):
        rel = p.relative_to(root)
        if p.is_file() and not SKIP_DIRS.intersection(rel.parts[:-1]) and p.name not in SKIP_FILES:
            out[rel.as_posix()] = p
    return out


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def text(p):
    if p is None:
        return ""
    b = p.read_bytes()
    if b"\0" in b:
        return None
    try:
        return b.decode("utf-8")
    except UnicodeDecodeError:
        return None


def compare(master, copy):
    """(rel, state, master path, game path) for every file that differs"""
    m, c = tree(master), tree(copy)
    out = []
    for rel in sorted(set(m) | set(c)):
        a, b = m.get(rel), c.get(rel)
        if a and b:
            if sha(a) != sha(b):
                out.append((rel, "changed", a, b))
        else:
            out.append((rel, "only in SDK" if a else "only in game", a, b))
    return out


def unified(rel, a, b):
    ta, tb = text(a), text(b)
    if ta is None or tb is None:
        return None
    return list(difflib.unified_diff(ta.splitlines(), tb.splitlines(), f"sdk/{rel}", f"game/{rel}", lineterm=""))


def classify(game, rel, a, b):
    """(class, reason) from the path and what changed"""
    lines = unified(rel, a, b)
    changed = "\n".join(l[1:] for l in lines[2:] if l[:1] in "+-") if lines else ""
    for n in game["names"]:
        if n and n.lower() in changed.lower():
            return "game specific", f"mentions {n}"
    top = rel.split("/")[0]
    if rel in game["overrides"]:
        return "game specific", "listed in the game's sdk.toml overrides"
    if top in ("runtime", "tools", "cmake"):
        return "generic", f"under {top}/"
    if top == "xdk":
        return "xdk data", "under xdk/"
    return "unclear", "outside runtime, tools, cmake and xdk"


def newer(a, b):
    if not a or not b:
        return ""
    return "game" if b.stat().st_mtime > a.stat().st_mtime else "SDK"


def scan(roots, rx, skip=()):
    """file:line text for each match in source files under roots"""
    hits = []
    for base, label in roots:
        for rel, p in tree(base).items():
            if Path(rel).suffix not in SRC_EXT or any(rel.startswith(s) for s in skip):
                continue
            t = text(p) or ""
            for i, line in enumerate(t.splitlines(), 1):
                if rx.search(line):
                    hits.append((f"{label}/{rel}", i, line.strip()[:160]))
    return hits


def pin_state(game, version):
    if not game["has_pin"]:
        return "no sdk.toml"
    if game["pin"] == version:
        return f"{game['pin']} (current)"
    return f"{game['pin']} (SDK is {version})"


def report_game(g, master, version):
    out = [f"## {g['name']}", "", f"- Repository: `{g['root']}`", f"- SDK copy: `{g['copy']}`",
           f"- Pinned: {pin_state(g, version)}"]
    cv = g["copy"] / "SDK_VERSION"
    out.append(f"- Copy's SDK_VERSION: {cv.read_text().strip() if cv.exists() else 'missing'}")
    for img in g["images"]:
        out.append(f"- Image {img}: {'present' if (g['root'] / 'assets' / img).exists() else 'not in assets'}")
    out.append(f"- Overrides: {', '.join(sorted(g['overrides'])) or 'none'}")
    out.append("")
    if not g["copy"].is_dir():
        return out + ["No SDK copy found.", ""]

    diffs = compare(master, g["copy"])
    out += [f"### Differences ({len(diffs)})", ""]
    if diffs:
        out += ["| File | State | Class | Reason | Newer |", "|---|---|---|---|---|"]
        for rel, state, a, b in diffs:
            cls, why = classify(g, rel, a, b)
            out.append(f"| `{rel}` | {state} | {cls} | {why} | {newer(a, b)} |")
        out.append("")
        for rel, state, a, b in diffs:
            lines = unified(rel, a, b)
            out.append(f"#### `{rel}` ({state})")
            out.append("")
            if lines is None:
                out += ["Binary, hashes differ.", ""]
                continue
            cut = lines[:DIFF_LINES]
            out += ["```diff", *cut, "```"]
            if len(lines) > DIFF_LINES:
                out.append(f"{len(lines) - DIFF_LINES} more lines not shown.")
            out.append("")
    else:
        out += ["The copy matches the SDK master.", ""]

    sdk_names = {m.group(1) for _, _, l in scan([(master / "runtime", "runtime")], OVERRIDE_RE) for m in OVERRIDE_RE.finditer(l)}
    ovr = [(f, n, l) for f, n, l in scan([(g["root"] / "src", "src")], OVERRIDE_RE)
           if any(m.group(1) in sdk_names for m in OVERRIDE_RE.finditer(l))]
    out += [f"### Strong overrides of SDK symbols ({len(ovr)})", ""]
    out += [f"- `{f}:{n}` {l}" for f, n, l in ovr] or ["None: no game source replaces a function the SDK runtime implements."]
    out.append("")

    roots = [(g["root"] / "src", "src"), (g["copy"], "sdk")]
    skip = ("tools/sdk_sync/",)
    for title, rx in (("LOG_FIRST fallbacks", LOG_FIRST_RE), ("TEMPORARY markers", TEMP_RE)):
        hits = [h for h in scan(roots, rx, skip) if not h[0].endswith("core/log.h")]
        out += [f"### {title} ({len(hits)})", ""]
        out += [f"- `{f}:{n}` {l}" for f, n, l in hits] or ["None."]
        out.append("")
    return out


def cmd_report(args, games, master, version):
    if not args.out:
        die("report needs --out <folder>")
    folder = Path(args.out).resolve()
    if any(folder == g["root"] or g["root"] in folder.parents for g in games) or folder == master or master in folder.parents:
        die(f"{folder} is inside a repository; pass a folder outside the SDK and game repos")
    folder.mkdir(parents=True, exist_ok=True)
    lines = [f"# SDK sync report {date.today().isoformat()}", "", f"SDK master `{master}`, version {version}.", "",
             "Classes are a heuristic: runtime/, tools/ and cmake/ are generic, xdk/ is XDK data, a change that names the "
             "game is game specific, anything else is unclear. Newer compares file times only.", ""]
    for g in games:
        lines += report_game(g, master, version)
    path = folder / f"sdk-sync-{date.today().isoformat()}.md"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {path}")


def copy_file(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def cmd_upstream(args, games, master, version):
    g = pick(games, args.game)
    copied = refused = 0
    for rel, state, a, b in compare(master, g["copy"]):
        if state == "only in SDK":
            print(f"  kept     {rel}: only in the SDK master, delete it there by hand if the game removed it on purpose")
            continue
        if rel == "SDK_VERSION":
            print("  skipped  SDK_VERSION: the master's version is bumped by hand with a CHANGELOG entry")
            continue
        cls, why = classify(g, rel, a, b)
        if (cls == "game specific" or rel in g["overrides"]) and not args.force:
            print(f"  refused  {rel}: {cls}, {why} (--force copies it)")
            refused += 1
            continue
        copy_file(b, master / rel)
        print(f"  copied   {rel} ({state}, {cls})")
        copied += 1
    print(f"{copied} copied into {master}, {refused} refused")
    if copied:
        print("Next: review the changes in the master, add a CHANGELOG.md entry, bump SDK_VERSION, then commit there.")


def set_pin(g, version):
    p = g["pin_file"]
    if not p.exists():
        p.write_text(f'[sdk]\nversion = "{version}"\npath = "sdk"\nupstream = "../XboxXBE-Recomp"\n\noverrides = []\n',
                     encoding="utf-8", newline="\n")
        return
    t = p.read_text(encoding="utf-8")
    t, n = re.subn(r'(?m)^(version\s*=\s*)"[^"]*"', rf'\g<1>"{version}"', t, count=1)
    if not n:
        die(f"{p} has no version line")
    p.write_text(t, encoding="utf-8", newline="\n")


def cmd_downstream(args, games, master, version):
    g = pick(games, args.game)
    copied = refused = 0
    for rel, state, a, b in compare(master, g["copy"]):
        if state == "only in game":
            print(f"  kept     {rel}: only in the game's copy, upstream it or delete it by hand")
            continue
        if rel in g["overrides"]:
            print(f"  kept     {rel}: listed in the game's overrides")
            continue
        if state == "changed" and newer(a, b) == "game" and not args.force:
            print(f"  refused  {rel}: the game's copy is newer, upstream it or list it in overrides (--force overwrites it)")
            refused += 1
            continue
        copy_file(a, g["copy"] / rel)
        print(f"  copied   {rel} ({state})")
        copied += 1
    old = g["pin"] or "none"
    if refused:
        print(f"{copied} copied into {g['copy']}, {refused} refused; sdk.toml pin left at {old}")
    else:
        set_pin(g, version)
        print(f"{copied} copied into {g['copy']}; sdk.toml pin {old} -> {version}")
    print("Next, run these yourself:")
    print(f'  cd /d "{g["root"]}" && build.bat')
    if g["harness"]:
        print(f"  {g['harness']}   (from {g['root']}, on Linux)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--games", default=str(HERE / "sdk_games.toml"), help="registry of game repos")
    ap.add_argument("--sdk", default=str(HERE.parents[1]), help="SDK master root, default the repo holding this script")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("report")
    r.add_argument("--out", required=True, help="folder for the report, outside every repository")
    u = sub.add_parser("upstream")
    u.add_argument("game")
    u.add_argument("--force", action="store_true")
    d = sub.add_parser("downstream")
    d.add_argument("game")
    d.add_argument("--force", action="store_true")
    args = ap.parse_args()
    master = Path(args.sdk).resolve()
    vf = master / "SDK_VERSION"
    if not vf.exists():
        die(f"{vf} not found; --sdk must be the SDK master root")
    version = vf.read_text().strip()
    games = load_games(Path(args.games).resolve())
    {"report": cmd_report, "upstream": cmd_upstream, "downstream": cmd_downstream}[args.cmd](args, games, master, version)


if __name__ == "__main__":
    main()
