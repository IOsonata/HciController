# Patches this firmware needs applied to TinyUSB

TinyUSB is not part of this repository. It is linked from
`${iosonata_loc}/external/tinyusb` by the Eclipse project, and it is the
upstream master branch. These patches are against that branch. Apply with:

    git -C <tinyusb> apply /path/to/HciController/external/tinyusb/0001-*.patch

Each one is listed here with what it is for and how to tell whether it is
still needed.

## 0001, one barrier for the event scan

`dcd_int_handler` opens by walking twenty five USBD event registers, and for
every event it finds it writes the register to zero and then issues `__ISB()`
and `__DSB()`. A `DSB` stalls the core until the write has actually reached
the peripheral, so an entry with several events pending pays for several of
those stalls.

The clears have to land before the handler returns, or the interrupt
re-triggers on an event that was already handled. One barrier after the whole
scan gives that, because a single `DSB` drains every write behind it and the
peripheral region is Device memory, whose accesses are not reordered against
each other. The reads later in the loop still see the writes made earlier in
it.

Measured before the patch on this firmware, from the `usbdt:` line: an
ordinary entry around fifteen hundred core cycles, twenty four microseconds
at 64 MHz, and a worst case at least four times that. The `port=` and
`portworst=` fields on that line are the handler alone, so the effect of this
patch is visible directly in them.

Drop this patch when upstream takes the same change.
