# Preserve raw SG_IO ATA IDENTIFY responses

This repository currently publishes LKM releases only. The attached source patch
is a proposed fix for the build source, not a change to released binaries. It is
based on the recoverable AuxXxilium source commit
[`32c18946a362aaba4d6a7dfc4131c0d2623252ef`](https://github.com/AuxXxilium/arc-lkm/commit/32c18946a362aaba4d6a7dfc4131c0d2623252ef).
The maintainer must apply/adapt it in the current source repository and rebuild.
No deleted source tree is restored here.

## Problem

`handle_sg_io_ioctl()` calls `sanitize_sg_io_identify_response()` after the real
ioctl. `sanitize_ata_string()` treats the raw firmware field as an ordinary string,
but ATA IDENTIFY strings are word-swapped. Crucial MX500 firmware `M3CR023` is
encoded as:

```text
Decoded: M3CR023[space]
Raw:     33 4d 52 43 32 30 20 33
Before:  33 4d 52 43 32 30 20 20
```

The sanitizer blanks the final `3` after encountering the raw padding byte.
It also leaves the original IDENTIFY integrity checksum unchanged. DSM smartctl
then prints `M3CR02` and `Drive Identity Structure error: invalid SMART checksum`.
Recomputing the checksum would hide the corruption, not restore the firmware.
Even a word-aware display sanitizer should not mutate a raw diagnostic response.

## Proposed fix

Remove the SG_IO-specific sanitization path and let SG_IO use the existing
unmodified ioctl passthrough. Delete the helpers, opcode constants, unused
includes and comment referring to the removed path. Real driver buffers, status,
residual and errors are preserved. Native HDIO handling and synthetic-device
SMART emulation are deliberately outside this patch's scope. Firmware cleanup
for presentation should happen on a decoded display copy, not the raw IDENTIFY
payload.

## Verification

Run on Linux with Python 3, git, a C compiler and network access:

```sh
python3 tools/reproduce-identify-checksum.py
```

The script fetches the pinned historical source, applies the patch in a temporary
directory, extracts the actual original/repaired dispatch functions, and compiles
both with a userspace ioctl fixture. It accesses no disks and needs no root.
It reproduces the Crucial truncation/checksum failure before the fix, then checks
complete 512-byte preservation, integrity checksum and failed-ioctl buffer/error
preservation after the fix, for ATA PASS-THROUGH 12 and 16. It also covers even-
length and embedded-space firmware strings. Fixtures contain no real serials.
Observed output:

```text
BEFORE: reproduced M3CR023 truncation and invalid IDENTIFY checksum (ATA12/16)
AFTER: original 512-byte response/checksum and ioctl errors preserved (ATA12/16)
```

A separate local runtime workaround bypassing the same postprocessing was tested
on Arc 4.2.2 / LKM release 26.10.27, DS3622xs+, DSM kernel 4.4.302+. With the original
module, raw SG_IO via the SSD's block node differed from the host/generic-SCSI
response in exactly one byte (offset 53: `33` -> `20`); checksum sum modulo 256
changed from 0 to 237. After bypassing postprocessing and cleanly rebooting, all
512 bytes matched the host response, firmware was `M3CR023`, checksum was valid,
and SMART reported PASSED without the checksum warning. This was a local binary
workaround, **not** a full build or on-device test of the attached C source patch.
The userspace harness does not replace kernel/module integration testing.

An independent QEMU ATA sense-format fix was already present during both sides
of that comparison. This submission does not claim to solve QEMU sense handling
or validate SMART self-test execution.
