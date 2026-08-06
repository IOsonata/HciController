#!/usr/bin/env python3
"""Check that the Eclipse project lists every source and header in the tree.

nRF52840/ioc/.project names each file individually as a linked resource. A
file added to src/ or include/ and not added there is simply not built, and
nothing says so until the link fails on an undefined reference, which names
one symbol and not the file it came from.

That is what happened to src/hci_sdc_resources.cpp: the host tests compiled
and ran it, so every check here passed, and the target build linked without
it.

    python3 tests/project_files.py
    python3 tests/project_files.py /path/to/HciController

Exit status is 0 when the two agree and 1 when they do not.
"""

import os
import re
import sys


def repo_root(start):
    d = os.path.abspath(start)
    while True:
        if os.path.isfile(os.path.join(d, "include", "hci_h4.h")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            return None
        d = parent


def on_disk(root):
    """Sources and headers the firmware is made of, as project relative paths."""
    found = set()
    for folder, suffixes in (("include", (".h",)), ("src", (".cpp", ".c", ".h"))):
        path = os.path.join(root, folder)
        for name in sorted(os.listdir(path)):
            if name.endswith(suffixes):
                found.add(folder + "/" + name)
    return found


def in_project(project_path):
    """Every linked resource that points into the repository, by its target."""
    with open(project_path, "r", encoding="ascii") as handle:
        text = handle.read()

    listed = set()
    for uri in re.findall(r"<locationURI>([^<]+)</locationURI>", text):
        # PARENT-2-PROJECT_LOC/src/hci_app.cpp reaches the repository root.
        # PARENT-1-PROJECT_LOC and virtual: entries point elsewhere.
        if uri.startswith("PARENT-2-PROJECT_LOC/"):
            listed.add(uri[len("PARENT-2-PROJECT_LOC/"):])
    return listed


def main(argv):
    root = repo_root(argv[1] if len(argv) > 1 else __file__)
    if root is None:
        print("HciController root not found, pass it as an argument")
        return 1

    project = os.path.join(root, "nRF52840", "ioc", ".project")
    if not os.path.isfile(project):
        print("no %s, nothing to check" % project)
        return 0

    disk = on_disk(root)
    listed = in_project(project)

    missing = sorted(disk - listed)
    stale = sorted(listed - disk)

    for name in missing:
        print("[!!] %-40s in the tree, not in the Eclipse project" % name)
    for name in stale:
        print("[!!] %-40s in the Eclipse project, not in the tree" % name)

    if missing or stale:
        print("\n%d file(s) disagree. A source missing from the project is "
              "not compiled, and the target build fails at link time naming "
              "a symbol rather than a file." % (len(missing) + len(stale)))
        return 1

    print("[ok] Eclipse project lists all %d sources and headers." % len(disk))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
