# HciController coding and review rules

HciController follows the IOsonata engineering and code-organization rules.
The purpose is to keep the controller small, deterministic, portable at the HCI
boundary, and readable without introducing a second framework beside IOsonata.

## Source style

- Use ASCII in source files and project documentation.
- Match the indentation already used by the file being changed. IOsonata library
  sources use tabs; do not mass-reformat an unrelated file as part of a
  functional change.
- Keep comments technical and specific. Describe the hardware, protocol rule,
  state transition, or failure being handled.
- Follow the naming already used by the subsystem. Public HciController symbols
  use the `Hci...` prefix, C state types use the `_t` suffix, and implementation
  state remains private where it does not belong in the public API.
- Do not add large macro frameworks or broad conditional-compilation designs
  when a normal function, table, target operation, or board policy is enough.
- Do not add `printf` to controller/library paths for diagnostics. Use the
  existing `HciTrace`/system-log path where logging is safe.

## Memory and language rules

The embedded runtime uses static or caller-owned storage.

- no heap allocation in controller or real-time paths;
- no exceptions;
- no RTTI;
- no template-heavy replacement for the runtime-polymorphic design;
- no hidden allocation;
- no unbounded work in interrupt handlers.

The Eclipse configurations build C++ with exceptions and RTTI disabled. Keep
new code compatible with that model.

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

## Change discipline

- Keep commits focused.
- Do not rewrite unrelated code while fixing a defect.
- Preserve public APIs unless the change is intentional and reviewed.
- Do not hide an unsupported capability behind a successful-looking fallback;
  fail closed and report the actual capability.
- A source release is not Bluetooth SIG qualification, PTS certification, or RF
  certification. Documentation must not imply otherwise.
