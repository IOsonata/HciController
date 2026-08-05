# HCI command coverage

The dispatch table in `src/hci_sdc_nrfxlib.cpp` carries 59 commands. Anything
not listed there is answered with Unknown HCI Command, so the table is the
controller's actual capability, and `HCI_Read_Local_Supported_Commands` reports
exactly the same set. A host test checks the two against each other in both
directions, so a command cannot be added to one and forgotten in the other.

## Supported

Controller and baseband

    0x0C01  Set Event Mask
    0x0C03  Reset
    0x0C7B  Read Authenticated Payload Timeout
    0x0C7C  Write Authenticated Payload Timeout

Informational parameters

    0x1001  Read Local Version Information
    0x1002  Read Local Supported Commands
    0x1003  Read Local Supported Features
    0x1009  Read BD_ADDR

Link control

    0x0406  Disconnect                              status
    0x041D  Read Remote Version Information         status

LE basics

    0x2001  LE Set Event Mask
    0x2002  LE Read Buffer Size
    0x2003  LE Read Local Supported Features
    0x2005  LE Set Random Address

Legacy advertising and scanning

    0x2006  LE Set Advertising Parameters
    0x2007  LE Read Adv Physical Channel Tx Power
    0x2008  LE Set Advertising Data
    0x2009  LE Set Scan Response Data
    0x200A  LE Set Advertising Enable
    0x200B  LE Set Scan Parameters
    0x200C  LE Set Scan Enable

Connection management

    0x200D  LE Create Connection                    status
    0x200E  LE Create Connection Cancel
    0x2013  LE Connection Update                    status
    0x2015  LE Read Channel Map
    0x2016  LE Read Remote Features                 status

Filter accept list

    0x200F  LE Read Filter Accept List Size
    0x2010  LE Clear Filter Accept List
    0x2011  LE Add Device To Filter Accept List
    0x2012  LE Remove Device From Filter Accept List

Security

    0x2017  LE Encrypt
    0x2018  LE Rand
    0x2019  LE Enable Encryption                    status
    0x201A  LE Long Term Key Request Reply
    0x201B  LE Long Term Key Request Negative Reply

Direct test mode

    0x201D  LE Receiver Test v1
    0x201E  LE Transmitter Test v1
    0x201F  LE Test End

Data length

    0x2022  LE Set Data Length
    0x2023  LE Read Suggested Default Data Length
    0x2024  LE Write Suggested Default Data Length
    0x202F  LE Read Maximum Data Length

PHY

    0x2030  LE Read PHY
    0x2031  LE Set Default PHY
    0x2032  LE Set PHY                              status

Extended advertising

    0x2035  LE Set Advertising Set Random Address
    0x2036  LE Set Extended Advertising Parameters
    0x2037  LE Set Extended Advertising Data        variable
    0x2038  LE Set Extended Scan Response Data      variable
    0x2039  LE Set Extended Advertising Enable      variable
    0x203A  LE Read Maximum Advertising Data Length
    0x203B  LE Read Number Of Supported Adv Sets
    0x203C  LE Remove Advertising Set
    0x203D  LE Clear Advertising Sets

Extended scanning and initiating

    0x2041  LE Set Extended Scan Parameters         variable
    0x2042  LE Set Extended Scan Enable
    0x2043  LE Extended Create Connection           variable, status

Vendor specific

    0xFC09  VS Read Static Addresses                variable return
    0xFFF0  VS Read Counters

The board carries no public address, so `HCI_Read_BD_ADDR` answers all zeros
and a host that asks for Own_Address_Type 0x00 is refused with 0x12. 0xFC09 is
what BlueZ and Zephyr ask instead: it reports the static random address SDC
derives from FICR, which is the value IOsonata `nrf_get_mac_address()` also
produces, so the board keeps one identity across runs and across firmware.
Its return is a count byte followed by 22 octets per address, so the length
depends on the answer; the table declares the count byte alone, which is the
minimum the command always carries and what an error is padded out to.

0xFFF0 is this firmware's own, and the routing layer answers it without going
near the radio. It reports the counters the dispatch table and the routing
layer keep of everything they refused: unknown opcodes, wrong lengths, ACL the
controller would not take, packets it asked to have offered again. Until it
existed those numbers only lived in RAM, so a board could be questioned only
with a debugger on it, which is no use on a sealed dongle. One version byte
then sixteen counters, four octets each little endian, laid out in `hci_sdc.h`;
counters are appended and the version raised, never renumbered.

The eight marked status answer with a Command Status rather than a Command
Complete, as Vol 4 Part E 7.7.15 requires. Getting that wrong leaves a host
stack waiting for an event that never arrives.

## Built out, and how to build them back in

A SoftDevice Controller header declares the whole HCI API, but a library
variant only contains the commands it was built with, so a declaration is not
a guarantee that the symbol links. Seven commands therefore sit behind a macro
in
`src/hci_sdc_nrfxlib.cpp`, each covering the handler, the table row and the
supported commands bit together, so the three stay consistent whichever way the
macro goes.

    HCI_SDC_HAS_READ_SUPPORTED_STATES   0     0x201C  LE Read Supported States
    HCI_SDC_HAS_READ_TRANSMIT_POWER     0     0x204B  LE Read Transmit Power
    HCI_SDC_HAS_READ_REMOTE_VERSION     1     0x041D  Read Remote Version Info
    HCI_SDC_HAS_AUTH_PAYLOAD_TIMEOUT    1     0x0C7B and 0x0C7C
    HCI_SDC_HAS_VS_READ_STATIC_ADDRESSES  1   0xFC09  VS Read Static Addresses
    HCI_SDC_HAS_VS_READ_COUNTERS          1   0xFFF0  VS Read Counters

The first two default to off because a link against the multirole library
proved the symbols absent. Read Supported States reports legacy advertising
states and is left out of a build that only enables the extended advertiser.
Read Transmit Power belongs to LE Power Control. The next two default to on
because the specification makes them mandatory, and the vendor specific one
because a board with no public address is unusable to a host without it. The
counter readout needs no SDC symbol at all and is on for the same reason.

List what a library really defines with

    arm-none-eabi-nm --defined-only libsoftdevice_controller_multirole.a \
        | grep " T sdc_hci_cmd_"

and set the macro to match. That way a missing symbol is an undefined
reference at link time and a one line change here, rather than a controller
that advertises a command it cannot run.

## How the handlers are written

The handlers are all one of seven shapes, so they are generated by macro
rather than copied fifty times: parameters only, parameters with a return, no
parameters, no parameters with a return, and three variable length forms that
differ only in how the trailing array is measured, by byte count, by element
count, or by the number of PHYs named in a bitmap.

A variable length command carries a trailing array whose size comes from a
field inside the fixed part, and SDC trusts that field. Each variable length
macro therefore requires the parameter length the host sent to match what the
count field declares, exactly rather than at least, since Vol 4 Part E 5.4.1
fixes Parameter_Total_Length. A mismatch is refused with 0x12. Without that
check a short packet with a large count makes SDC read past the end of the
receive buffer, which holds the previous packet.

Parameter lengths come from `sizeof()` on the SDC type rather than hand counted
numbers, and static assertions pin each of those types to the length Vol 4 Part
E gives. Without them nothing compares an SDC type against the wire: the host
test sends the same `sizeof()` the table declares, so the two agree by
construction whatever either is worth, and a type that did not match would show
up only as a board refusing a correctly formed command with 0x12.

Every table row also declares the response kind and the return parameter
length that a successful call produces, so a rejection is shaped the same way
as a success. A Command Complete that is short of the length the host holds
for that opcode reads to the host as no answer at all.

## Tests

    make -C tests run NRFXLIB_DIR=/path/to/sdk-nrfxlib

94 checks across nine binaries. `hci_sdc_dispatch_test` compiles the real
dispatch table against the real nrfxlib headers with only the SDC entry points
stubbed, then checks that every opcode reaches the intended SDC function, that
Command Status and Command Complete are used where the specification says,
that return payload lengths match the SDC return types, that a variable length
command whose count field disagrees with the packet is rejected with 0x12,
that a wrong fixed length is rejected before the handler runs, that an
unassigned opcode is answered with 0x01, and that a controller error is passed
through rather than masked.

Two of those checks walk the whole table rather than a hand picked list. One
drives every fixed length entry twice, once accepted and once rejected, and
compares what comes out against what the row declares. The other reads the
supported commands bitmap back off the wire and matches it against the table
in both directions.

The stubs under `tests/stubs/sdclink` follow the nrfxlib signatures, so a
change in the real headers shows up as a compile error rather than as wrong
behaviour.

If NRFXLIB_DIR does not resolve, that one test is skipped and the other eight
still run against the fakes under `tests/stubs`.
