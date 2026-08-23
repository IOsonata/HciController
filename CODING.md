# HciController coding and review rules

HciController follows the IOsonata coding standard. The rules in this file are
HciController-specific additions and release guidance; they do not override the
shared standard.

HciController source remains MPL-2.0. New or substantially rewritten file
headers use the standard Doxygen layout while retaining the HciController
MPL-2.0 license notice. Do not change a file's license as part of style cleanup.

## Source rules

- Source and project documentation are ASCII only.
- New HciController code uses tabs for indentation. Imported vendor code keeps
  its upstream style, and edits inside imported code match the surrounding
  vendor style.
- Do not add em-dashes. Use a plain hyphen, a colon, or rewrite the sentence.
- Run the prohibited-word check from the IOsonata coding standard on every
  added line.
- Brace balance must remain zero in every touched source file.
- Do not put `#include` directives inside an `extern "C"` block. A header owns
  its own linkage; mark individual declarations or definitions when C linkage
  is required.
- Use `msDelay` for millisecond delays.
- Do not add direct `printf`-family diagnostics to controller or driver paths.
  Use `HciTrace` and the system-log path where logging is safe. The system-log
  formatter is the formatting implementation, not a packet-path debug sink.
- Keep comments technical and specific. A comment that claims behavior must be
  checked against the code or hardware it describes.
- Follow the naming already used by the subsystem. Public HciController symbols
  use the `Hci...` prefix, C state types use the `_t` suffix, and implementation
  state remains private where it does not belong in the public API.
- Before introducing a define, enum, type, or constant, search the tree for the
  same concept by name, value, member names, and nearby comment text.
- Do not add large macro frameworks or broad conditional-compilation designs
  when a normal function, table, target operation, or board policy is enough.

Do not mass-reformat an unrelated legacy file during release cleanup. Apply the
current rules to new code and to lines being materially changed; a tree-wide
format migration is a separate change with its own review.

## File headers and external source provenance

New source files start with a Doxygen file block containing the file name, a
one-line summary, the non-obvious implementation facts a maintainer needs, the
author/date, and the project MPL-2.0 license notice.

When HciController replaces, adapts, or mirrors part of an external source,
record all of the following next to the local implementation:

- upstream repository;
- exact upstream commit reviewed;
- upstream file path;
- a note requiring the local implementation to be re-checked whenever that
  dependency is updated.

Do not copy an API or vendor behavior from memory. Read the pinned source,
including the implementation of behavior the local code depends on, before
changing the local code or its comments.

## Memory and language rules

The embedded runtime uses static or caller-owned storage.

- no heap allocation in controller or real-time paths;
- no exceptions;
- no RTTI;
- no template-heavy replacement for the runtime-polymorphic design;
- no hidden allocation;
- no unbounded work in interrupt handlers.

The nRF52840 project compiles C as GNU C17 and C++ as GNU C++23. Host compile
checks use the same language standards so target-only language diagnostics are
not hidden by an older host dialect. Treat new warnings as defects.

## IOsonata object model

At the HCI controller boundary, transports are `DeviceIntrf` implementations.
Do not make controller code depend on a concrete UART, TinyUSB, or MCU driver.

The current layering is deliberate:

```text
UART byte stream ----> H:4 adapter ----+
USB CDC byte stream -> H:4 adapter ----+--> packet DeviceIntrf --> HCI controller
native USB HCI ------------------------+
```

`DevAddr` is the HCI packet type and the data buffer is one complete HCI packet
without an H:4 indicator. UART and CDC framing belongs below that boundary.

Target-specific clock, interrupt, USB peripheral, MPSL and SDC bring-up stays in
the target port. Board-specific pin and host policy stays in `board.h`. Do not
move MCU details into the generic HCI parser, dispatcher, transport, or routing
layers.

## Interrupt and scheduler rules

Interrupt handlers record the minimum state and wake the HCI thread. HCI packet
parsing, command dispatch and SDC access stay in thread context.

The nRF52840 runtime has a strict-priority HCI thread and, on boards with a
mode button, a short critical-priority status/control thread. Do not add
blocking output or unbounded work to the high-priority mode-button path.

Internal flash writes are not performed while MPSL/SDC owns the radio. The mode
switch sequence is:

```text
button confirmed -> stop HCI runtime -> stop USB/SDC/MPSL -> NVM write/verify
                 -> system reset -> load persisted mode before USB/radio start
```

Do not replace this with a retained-register handoff through a bootloader.

The runtime stop handshake must also cover a thread that has been created but
has not yet executed its first instruction. `ThreadArmed` is part of the stop
state for that reason; do not reduce the stopped test to `ThreadLive` alone.

## HCI and USB changes

When adding or changing an HCI command, keep all of these in agreement:

- dispatch table;
- supported-command reporting where the Core specification assigns a bit;
- parameter and return lengths;
- Command Complete versus Command Status behavior;
- SDC resource/support calls needed by the feature;
- host command catalog or dedicated target-profile coverage;
- hardware/release test coverage for advertised capability.

For native USB changes, preserve the packet `DeviceIntrf` boundary. The USB
class driver may frame or reframe packets, but it must not bypass the controller
through transport-specific callbacks.

USB descriptor versioning comes from `HCI_CONTROLLER_VERSION_BCD`. Transport
PIDs are the mode-specific `HCI_USB_PID_*` definitions in `usb_descriptors.c`.
Do not reintroduce an independent project-level USB release macro or one shared
PID macro that the descriptor no longer reads.

## Eclipse project files

- Linked resources use `PARENT-n-PROJECT_LOC` or another portable workspace
  form. Never commit a machine-local absolute path.
- A change required by the HciController sources must be present in every build
  configuration that compiles those sources, Debug and Release variants alike.
- Remove stale defines, linked resources, exclusions, include paths, and library
  paths when the source that used them is removed or renamed.
- Parse `.project` and `.cproject` as XML after editing them.
- Check linked-resource paths for accidental `<location>/...` entries before
  committing.

## Review procedure

Before changing a subsystem:

1. Read its complete public header and implementation.
2. Read the caller and the target/vendor implementation it depends on.
3. Search every public symbol definition affected by the change.
4. Read the relevant host tests and hardware harness path.
5. Check recent work touching the same subsystem.
6. State which existing design pattern the change follows.

For IOsonata library changes, also follow the IOsonata repository `AGENTS.md`:
shared APIs require cross-target review, not an nRF52840-only inspection.

Hardware results supplied by the maintainer are authoritative. Do not claim a
build, hardware result, radio result, or release result that was not actually
run.

## Pre-commit checks

Run the IOsonata coding-standard checks on every touched source file. At a
minimum verify ASCII, prohibited added text, balanced braces, project XML where
applicable, and the absence of absolute linked-resource paths.

Useful local checks include:

```sh
grep -cP '[^\x00-\x7F]' path/to/file
python3 -c "s=open('path/to/file').read(); print(s.count('{')-s.count('}'))"
python3 -c "import xml.dom.minidom; xml.dom.minidom.parse('nRF52840/ioc/.project')"
python3 -c "import xml.dom.minidom; xml.dom.minidom.parse('nRF52840/ioc/.cproject')"
grep '<location>/' nRF52840/ioc/.project nRF52840/ioc/.cproject
```

Before pushing, inspect the complete diff and confirm the branch still points
to the exact parent that was reviewed.

## Change discipline

- Keep commits focused.
- Make surgical edits with unique anchors; do not rewrite unrelated code while
  fixing a defect.
- Preserve public APIs unless the change is intentional and reviewed.
- Do not hide an unsupported capability behind a successful-looking fallback;
  fail closed and report the actual capability.
- A source release is not Bluetooth SIG qualification, PTS certification, or RF
  certification. Documentation must not imply otherwise.
