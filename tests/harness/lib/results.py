#!/usr/bin/env python3
"""Result accounting shared by BLE test harnesses."""

from dataclasses import dataclass


PASS = "PASS"
FAIL = "FAIL"
NA = "N/A"
INCOMPLETE = "INCOMPLETE"


@dataclass
class Result:
    group: str
    name: str
    status: str
    detail: str = ""


class ResultBook:
    """Collect harness results and make release semantics explicit."""

    def __init__(self, title):
        self.title = title
        self.results = []

    def add(self, group, name, status, detail=""):
        self.results.append(Result(group, name, status, detail))

    def passed(self, group, name, detail=""):
        self.add(group, name, PASS, detail)

    def failed(self, group, name, detail=""):
        self.add(group, name, FAIL, detail)

    def not_applicable(self, group, name, detail=""):
        self.add(group, name, NA, detail)

    def incomplete(self, group, name, detail=""):
        self.add(group, name, INCOMPLETE, detail)

    def has_failures(self):
        return any(r.status == FAIL for r in self.results)

    def has_incomplete(self):
        return any(r.status == INCOMPLETE for r in self.results)

    def release_passed(self):
        return not self.has_failures() and not self.has_incomplete()

    def print_report(self):
        print()
        print(self.title)
        print("=" * len(self.title))
        current = None
        for result in self.results:
            if result.group != current:
                current = result.group
                print()
                print(current)
            suffix = ("  " + result.detail) if result.detail else ""
            print("  %-38s %-10s%s" % (result.name, result.status, suffix))

        counts = {}
        for result in self.results:
            counts[result.status] = counts.get(result.status, 0) + 1

        print()
        print(
            "Summary: %d PASS, %d FAIL, %d N/A, %d INCOMPLETE"
            % (
                counts.get(PASS, 0),
                counts.get(FAIL, 0),
                counts.get(NA, 0),
                counts.get(INCOMPLETE, 0),
            )
        )
        print("RESULT: %s" % (PASS if self.release_passed() else FAIL))

    def exit_code(self):
        return 0 if self.release_passed() else 1
