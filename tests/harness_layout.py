#!/usr/bin/env python3
"""Pin the official test layout and prevent legacy harness paths returning."""

from pathlib import Path
import sys


def fail(message):
    raise SystemExit("[!!] " + message)


def main(argv):
    if len(argv) != 2:
        print("usage: harness_layout.py REPO_ROOT")
        return 2

    root = Path(argv[1]).resolve()
    tests = root / "tests"
    harness = tests / "harness"
    legacy_dir = tests / "hardware"
    if legacy_dir.exists():
        fail("legacy tests directory still exists: %s" % legacy_dir)

    old_path = "tests" + "/hardware"
    old_symbol = "_" + "HARDWARE_DIR"
    offenders = []

    for path in root.rglob("*"):
        if not path.is_file():
            continue
        if ".git" in path.parts or "build" in path.parts:
            continue
        if path.suffix not in (".py", ".md", ".txt", ".mk", "") and path.name not in (
                "Makefile", "GNUmakefile"):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        if old_path in text or old_symbol in text:
            offenders.append(str(path.relative_to(root)))

    if offenders:
        fail("legacy test path references remain:\n  " + "\n  ".join(sorted(offenders)))

    syntax_errors = []
    for path in harness.rglob("*.py"):
        try:
            source = path.read_text(encoding="utf-8")
            compile(source, str(path), "exec")
        except (OSError, UnicodeDecodeError, SyntaxError) as err:
            syntax_errors.append("%s: %s" % (path.relative_to(root), err))

    if syntax_errors:
        fail("official harness Python syntax errors:\n  "
             + "\n  ".join(sorted(syntax_errors)))

    required = (
        harness / "lib" / "hci_transport.py",
        harness / "lib" / "pair_transport.py",
        harness / "lib" / "hci_commands.py",
        harness / "hcicontroller" / "release_test.py",
        harness / "hcicontroller" / "cis_usb_pair_test.py",
    )
    missing = [str(path.relative_to(root)) for path in required if not path.is_file()]
    if missing:
        fail("official harness files missing:\n  " + "\n  ".join(missing))

    print("[ok] tests/harness is the only hardware/release test system")
    print("[ok] official harness Python sources compile")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
