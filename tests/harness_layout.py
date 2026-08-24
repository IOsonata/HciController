#!/usr/bin/env python3
"""Pin the public Python library and official hardware-test layout."""

import os
from pathlib import Path
import subprocess
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
    runners = harness / "hcicontroller"
    package = root / "python" / "hcicontroller"
    checker = Path(__file__).resolve()

    legacy_hardware = tests / "hardware"
    if legacy_hardware.exists():
        fail("legacy tests directory still exists: %s" % legacy_hardware)

    legacy_lib = harness / "lib"
    if legacy_lib.exists():
        fail("legacy harness library still exists: %s" % legacy_lib)

    old_hardware_path = "tests" + "/hardware"
    old_hardware_symbol = "_" + "HARDWARE_DIR"
    old_harness_path = "tests/harness" + "/lib"
    stale_python = (
        "from " + "lib ",
        "from " + "lib.",
        "import " + "lib",
        "_" + "LIB_DIR",
        '"harness", "lib"',
        "'harness', 'lib'",
    )
    ignored_parts = {".git", ".venv", "venv", "__pycache__", "build"}
    offenders = []

    for path in root.rglob("*"):
        if not path.is_file():
            continue
        if path.resolve() == checker:
            continue
        if any(part in ignored_parts for part in path.parts):
            continue
        if path.suffix not in (".py", ".md", ".txt", ".mk", "") and path.name not in (
                "Makefile", "GNUmakefile"):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue

        if old_hardware_path in text or old_hardware_symbol in text:
            offenders.append("%s: legacy tests/hardware reference" % path.relative_to(root))
            continue

        if path.suffix == ".py":
            if any(token in text for token in stale_python):
                offenders.append("%s: legacy harness import/path" % path.relative_to(root))
            continue

        if old_harness_path in text:
            offenders.append("%s: legacy harness/lib reference" % path.relative_to(root))

    if offenders:
        fail("legacy test references remain:\n  " + "\n  ".join(sorted(offenders)))

    syntax_errors = []
    for base in (package, runners):
        for path in base.rglob("*.py"):
            try:
                source = path.read_text(encoding="utf-8")
                compile(source, str(path), "exec")
            except (OSError, UnicodeDecodeError, SyntaxError) as err:
                syntax_errors.append("%s: %s" % (path.relative_to(root), err))

    if syntax_errors:
        fail("Python syntax errors:\n  " + "\n  ".join(sorted(syntax_errors)))

    required = (
        package / "hci_transport.py",
        package / "pair_transport.py",
        package / "hci_commands.py",
        package / "target_profile.py",
        runners / "_bootstrap.py",
        runners / "release_test.py",
        runners / "probe_test.py",
        runners / "cis_usb_pair_test.py",
    )
    missing = [str(path.relative_to(root)) for path in required if not path.is_file()]
    if missing:
        fail("required Python/harness files missing:\n  " + "\n  ".join(missing))

    # Import every primary entry point exactly as users run it. --help exits
    # before hardware discovery, but only after all top-level imports succeed.
    # Remove PYTHONPATH: source-tree bootstrapping is part of the interface.
    entrypoints = (
        "pair_smoke_test.py",
        "release_test.py",
        "probe_test.py",
        "cis_pair_test.py",
        "cis_usb_pair_test.py",
    )
    env = os.environ.copy()
    env.pop("PYTHONPATH", None)
    import_errors = []
    for name in entrypoints:
        path = runners / name
        result = subprocess.run(
            [sys.executable, str(path), "--help"],
            cwd=str(root),
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode != 0:
            detail = result.stderr.strip() or "exit %u" % result.returncode
            import_errors.append("%s: %s" % (path.relative_to(root), detail))

    if import_errors:
        fail("harness entry points do not run directly:\n  "
             + "\n  ".join(import_errors))

    print("[ok] tests/harness is the only hardware/release test system")
    print("[ok] python/hcicontroller is the only reusable harness library")
    print("[ok] public library and official harness Python sources compile")
    print("[ok] primary harness entry points run without PYTHONPATH")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
