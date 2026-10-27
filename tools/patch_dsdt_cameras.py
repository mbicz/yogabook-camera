#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Michal Bicz
r"""Fix the Lenovo Yoga Book YB1-X90F DSDT camera descriptions (same-length AML patch).

Input:  the firmware DSDT (OEM revision 3), e.g. /sys/firmware/acpi/tables/DSDT
        captured on a kernel *without* the override.
Output: DSDT with OEM revision 6, loaded early via CONFIG_ACPI_TABLE_UPGRADE.

Firmware bugs fixed (verified on hardware; setup and limitations in KERNEL.md):
  CAM4 (INT33FB, front OV2740): I2C 0x36 on I2C4 -> 0x10 on I2C3 (Linux i2c-2)
  CAM4 _DSM CsiLanes "1" -> "2" (sensor runs 2 MIPI lanes)
  CAM4 GPIO _DSM: RCAM==1 branch reports powerdown (pin 54) sensor-on value 0,
        but the sensor only answers with pin 54 high -> force the Else branch
  CAM3 (INT33BE, rear OV8858): I2C 0x10 -> 0x36 on I2C3
  CAM3 has no _PR0 (the X91F tables have {P28W, P18W, CLK0}), so ACPI never
        powers it: turn the unused CDEP package into _PR0 = {P28W, P18W, P33W}.
        CLK0 (\_SB.PCI0.I2C4.CLK0) doesn't fit; the atomisp CSI2 bridge hands
        pmc_plt_clk_0 to the sensor driver instead. P33W only pads the
        package to its original length (it is in the front camera's _PR0 too).
"""
import struct
import sys

I2C_HDR = bytes.fromhex("8e1e00010001020000010600801a0600")  # I2cSerialBusV2, 400 kHz, 7-bit


def pkglen(aml, off):
    lead = aml[off]
    extra = lead >> 6
    if not extra:
        return lead & 0x3F
    val = lead & 0x0F
    for k in range(extra):
        val |= aml[off + 1 + k] << (4 + 8 * k)
    return val


def device_range(aml, name):
    hits = [i for i in range(len(aml) - 4) if aml[i:i + 4] == name]
    if len(hits) != 1:
        sys.exit(f"{name!r}: expected one occurrence, found {len(hits)}")
    pl_off = hits[0] - 2
    while aml[pl_off - 2:pl_off] != b"\x5b\x82":  # DeviceOp
        pl_off -= 1
    return pl_off, pl_off + pkglen(aml, pl_off)


def find_one(aml, pat, start, end):
    hits = [i for i in range(start, end) if aml[i:i + len(pat)] == pat]
    if len(hits) != 1:
        sys.exit(f"pattern {pat.hex()}: expected one hit in [{start:#x},{end:#x}), found {len(hits)}")
    return hits[0]


def set_byte(aml, off, old, new):
    if aml[off] != old:
        sys.exit(f"offset {off:#x}: expected {old:#04x}, found {aml[off]:#04x} (not the stock DSDT?)")
    aml[off] = new


def patch_i2c(aml, start, end, old_addr, new_addr, old_bus=None, new_bus=None):
    count = 0
    i = start
    while (i := aml.find(I2C_HDR, i, end)) >= 0:
        addr_off = i + len(I2C_HDR)
        if struct.unpack_from("<H", aml, addr_off)[0] == old_addr:
            set_byte(aml, addr_off, old_addr, new_addr)
            if old_bus is not None:
                src_end = aml.find(b"\x00", addr_off + 2)
                set_byte(aml, src_end - 1, old_bus, new_bus)
            count += 1
        i += 1
    if not count:
        sys.exit(f"no I2C resource with address {old_addr:#x} in [{start:#x},{end:#x})")


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <stock-dsdt.aml> <out.aml>")
    aml = bytearray(open(sys.argv[1], "rb").read())
    if aml[:4] != b"DSDT" or struct.unpack_from("<I", aml, 24)[0] != 3:
        sys.exit("input is not the stock YB1-X90F DSDT (signature DSDT, OEM revision 3)")

    cam3 = device_range(aml, b"CAM3")
    cam4 = device_range(aml, b"CAM4")

    patch_i2c(aml, *cam3, 0x10, 0x36)
    patch_i2c(aml, *cam4, 0x36, 0x10, ord("4"), ord("3"))

    i = find_one(aml, b"CsiLanes\x00\x0d1\x00", *cam4)
    set_byte(aml, i + 10, ord("1"), ord("2"))

    i = find_one(aml, b"RCAM\x01\xa4\x0b\x01\x36", *cam4)  # If (RCAM == One) Return (0x3601)
    set_byte(aml, i + 4, 0x01, 0x00)

    # Name (CDEP, Package (0x02) { I2C7, ^^I2C7.PMIC })  ->
    # Name (_PR0, Package (0x03) { P28W, P18W, ^^^P33W })   (15 bytes either way)
    old = b"CDEP\x12\x11\x02I2C7^^\x2eI2C7PMIC"
    new = b"_PR0\x12\x11\x03P28WP18W^^^P33W"
    assert len(old) == len(new)
    i = find_one(aml, old, *cam3)
    aml[i:i + len(old)] = new

    struct.pack_into("<I", aml, 24, 6)
    aml[9] = 0
    aml[9] = (-sum(aml)) & 0xFF
    open(sys.argv[2], "wb").write(aml)


if __name__ == "__main__":
    main()
