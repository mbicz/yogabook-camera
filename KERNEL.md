# Kernel, ACPI and firmware compatibility

`yogabook-camera` is userspace for the **Lenovo Yoga Book YB1-X90F** front
OV2740 and rear OV8858, sharing one AtomISP2 capture device. Installing this
package does not make an arbitrary stock Linux 6.17 or 6.18 kernel camera-ready.
It does not install a kernel, ACPI tables or ISP firmware.

The first public release, 0.1, retains the locally deployed 1.1.7 daemon and its
**two YU12 cameras**: front 1920×1080 and rear 1600×1212 (2× binned). There is no
third full-resolution camera node.

## Known source baselines and evidence

Use a complete pinned kernel tree with its board support and a suitable full
configuration, rather than treating the topic map below as a tested cherry-pick
recipe:

| Kernel source | What is established |
|---|---|
| [6.18.58 deployed LTS source, `fe92d01241f2adc9acec9e78dc232c0a21f8608e`](https://github.com/mbicz/yogabook-linux-kernel/tree/fe92d01241f2adc9acec9e78dc232c0a21f8608e) (`yb-lts-6.18.58-yb7.8`) | Device ran `6.18.58-yogabook7-lts-cam-thermal1`, UTS build `#yb7.8`, with camera package 1.1.7. Both real PipeWire paths captured 150 frames each and completed additional 600-buffer pipelines; WV517 was discovered and rear autofocus reported ready. |
| [6.17 submission candidate, `10aa1f4d0fc4ab42faca26fce07ab92d3d728783`](https://github.com/mbicz/yogabook-linux-kernel/tree/10aa1f4d0fc4ab42faca26fce07ab92d3d728783) (`jekhor-submission`) | Complete `vmlinux modules` build passed with the complete camera configuration. **This exact candidate has not been installed or booted on the device.** |
| [6.17 candidate base, `bc6f91d9a5c3cd966f2ce7676b61495525394835`](https://github.com/jekhor/yogabook-linux-kernel/tree/bc6f91d9a5c3cd966f2ce7676b61495525394835) (`v6.17.4-yogabook6`) | Jekhor's board-support baseline, not by itself the working modern camera stack. The candidate adds the 21 topics mapped below. |

The published LTS commit has the same source tree as the actual build commit
`72372d64bc5cb314cf03dade763597a1a5fbf1af`; publication changed identity metadata,
not the code tree. LTS carries corresponding camera fixes through a different
port/history, not these exact 21 commit objects. An equivalent port may satisfy
this contract without sharing their ancestry, but needs its own target-line
build and hardware validation. Version numbers alone are not compatibility
proof.

The LTS observations are not a claim of daylight clipping-boundary acceptance,
colour-chart accuracy or rear autofocus image-quality acceptance. GNOME Camera's
UI was not exercised on the `#yb7.8` boot (the desktop was at GDM); real PipeWire
capture was exercised. Earlier LTS boots supplied lens-movement, autofocus
convergence and colour-bar evidence, not fresh tests of the 6.17 candidate.

## Userspace/kernel interface

The daemon uses the standard Linux userspace V4L2 and media-controller headers,
not kernel build headers or AtomISP private ioctl definitions:

- Discover named `ov2740` and `ov8858` capture inputs with `VIDIOC_ENUMINPUT`,
  select with `VIDIOC_S_INPUT`, and discover discrete YUV420 sizes with
  `VIDIOC_ENUM_FRAMESIZES`.
- Request single-planar `V4L2_PIX_FMT_YUV420` (**YU12**, advertised to PipeWire as
  I420) through `VIDIOC_S_FMT`. Capture uses MMAP buffers and the standard
  `REQBUFS`, `QUERYBUF`, `QBUF`, `DQBUF`, `STREAMON` and `STREAMOFF` operations.
  The negotiated size/format must match the selected camera; the daemon reads
  the returned row stride.
- Discover the rear lens with `MEDIA_IOC_G_TOPOLOGY`: the capture device's graph
  must contain the OV8858 sensor and a WV517 lens connected by an ancillary
  link, with an accessible V4L2 subdevice interface. It does not hardcode a
  `/dev/v4l-subdevN` index.
- Query and write standard `V4L2_CID_FOCUS_ABSOLUTE` on that lens. A writable,
  active integer control must cover the daemon's calibrated search range
  225–414 with step 1. If the lens or usable control is unavailable, video
  continues but rear autofocus does not.

AE/AWB and ISP tuning are supplied by the kernel; the daemon does not load AIQB
files or implement an AtomISP private-ioctl 3A stack. Standard APIs do not imply
that an arbitrary future staging-driver media graph or format change is
compatible: preserve the observable contract above. No kernel patches are
copied into this project, and no DKMS or matching kernel headers are needed to
build the daemon.

## Finite kernel topic map

Numbers are the order in the published 21-commit 6.17 series. These are feature
ownership/dependency explanations, **not independently validated minimal patch
sets**. Use the full pinned candidate or corresponding LTS tree.

### Enumeration, capture and package integration

| Topic | Exact public commit | Requirement |
|---|---|---|
| 09 — permit camera I2C clients | [`4a0608e6a175`](https://github.com/mbicz/yogabook-linux-kernel/commit/4a0608e6a1755af7144ebbd9e6be6a510f12fcc8) | The board's skip-I2C-clients quirk otherwise hides the sensor HIDs. This permits enumeration; it does **not** correct the firmware's wrong bus/address descriptions. |
| 10 — front-camera wiring | [`c63208a102c8`](https://github.com/mbicz/yogabook-linux-kernel/commit/c63208a102c8880a7ed5d2d1f40289ec2b11c6ac) | Modern OV2740 match for `INT33FB`, bridge link configuration, two lanes and Whiskey Cove V2P8SX regulator lookup. |
| 07 — rear sensor | [`ec8d7857d632`](https://github.com/mbicz/yogabook-linux-kernel/commit/ec8d7857d6322984659c944d8d7dd0b09ad3998e) | OV8858 ACPI binding, two-lane board modes, 19.2 MHz clock handling, unity digital gain and valid exposure/gain ranges. |
| 11 — rear bridge/clock | [`5697322d4f00`](https://github.com/mbicz/yogabook-linux-kernel/commit/5697322d4f00de0eff0ed2c15471d5a03162280a) | Rear link configuration and PMC clock lookup when the rear `_PR0` lacks a clock reference. |
| 02 — planar geometry | [`d80e50bf50ef`](https://github.com/mbicz/yogabook-linux-kernel/commit/d80e50bf50ef8dfaa17705a4b859defef2714779) | Unpadded advertised planar widths and 12-pixel input padding; part of the current YU12 capture geometry. |
| 05 — `atomisp nr` parameter | [`5dc162d34f0c`](https://github.com/mbicz/yogabook-linux-kernel/commit/5dc162d34f0c21f6d2c85eb86e1a76aa825fe682) | **Required by the supplied package integration.** `atomisp-yogabook-camera.conf` installs `options atomisp nr=1`. A driver without the parameter is not compatible with that module configuration. |

Topic 05 defines `module_param(nr, uint, 0644)` in
`drivers/staging/media/atomisp/pci/sh_css_params.c`, linked into `atomisp`.
`nr=1` selects stock filter strengths for newly created ISP streams; the public
kernel default remains 2. This setting affects ISP noise reduction, not PipeWire
pixels, and is not a private ioctl ABI. For a built-in AtomISP driver, modprobe
configuration does not apply; use the corresponding kernel parameter
`atomisp.nr=1` if the package's denoising policy is desired.

### Automatic exposure, colour and calibration

These are needed to reproduce the deployed automatic, corrected-colour camera
experience, rather than merely enumerate a device and obtain bytes:

| Topic | Exact public commit | Role |
|---|---|---|
| 01 — in-kernel 3A | [`7e7313f75e17`](https://github.com/mbicz/yogabook-linux-kernel/commit/7e7313f75e1706996358c1f1e7be8419ae7e3270) | Consume/recycle frame-completion S3A statistics; automatic exposure and white balance, highlight/tone handling and state reset/recovery. No separate proprietary userspace 3A process is required. |
| 03 — disable destructive AA defaults | [`87b9f4d7f36f`](https://github.com/mbicz/yogabook-linux-kernel/commit/87b9f4d7f36f4fba87b80d481a8bbf3e7fce6873) | Removes the Bayer/YUV anti-aliasing defaults that suppressed chroma. |
| 04 — optical black and initial CCM | [`f8a77335ca87`](https://github.com/mbicz/yogabook-linux-kernel/commit/f8a77335ca871920bc41aeb102249592fae36826) | Black-pedestal subtraction and initial OV2740 colour correction. |
| 06 — front lens shading | [`bb4935c4f5ad`](https://github.com/mbicz/yogabook-linux-kernel/commit/bb4935c4f5ad8311622a080e749590389279a68d) | Apply the front module's checked factory OTP calibration. |
| 08 — rear lens shading | [`66f9130686b3`](https://github.com/mbicz/yogabook-linux-kernel/commit/66f9130686b394b3aa33babbae0b78d9a44cbdd1) | Read/cache checked rear OTP at probe, apply at stream start; avoids loading OTP while the ISP receiver is armed. |
| 17 — per-sensor factory tuning | [`a012863d5622`](https://github.com/mbicz/yogabook-linux-kernel/commit/a012863d56228dfe43308e9d7c413cca22564173) | Each sensor's white-response curve and interpolated CCMs, including rear 16/1024 versus front 64/1024 pedestal. Supersedes the initial single front matrix. |
| 18 — YUV metadata | [`f5b552023dd5`](https://github.com/mbicz/yogabook-linux-kernel/commit/f5b552023dd5c2af9dc84bcfae09f4002199959f) | Describe actual full-range BT.601 output, not inferred limited-range/HD colour encoding. |

The tuning coefficients are in the kernel source. Proprietary Lenovo AIQB
binaries are not distributed or required at runtime by this daemon. OTP lens
shading is read from each physical sensor module by the kernel driver.

### Rear focus

[Topic 16, `e9093f63c306`](https://github.com/mbicz/yogabook-linux-kernel/commit/e9093f63c306facb36c2860f75dc99edc3c1f923)
adds the WV517 lens driver, runtime power management, standard absolute focus,
and the Yoga Book IPU-bridge association/GPIO wiring. This is required for rear
autofocus, not for basic video capture. The kernel exposes lens movement;
contrast autofocus is performed by this daemon. The firmware calls the actuator
AD5823, but the actual module uses WV517; do not substitute an AD5823 driver
based only on the firmware label.

### Not requirements of the two-camera capture package

| Topic | Exact public commit | Boundary |
|---|---|---|
| 19 — larger native NV12 preview | [`809fd4a42fe9`](https://github.com/mbicz/yogabook-linux-kernel/commit/809fd4a42fe969b8aedd69683253110a0a453c6a) | Optional capability for other clients. The current daemon requests YU12, not native 3200×2436 NV12, and does not set `disable_digital_zoom`. |
| 12 — LP855X config | [`7b2b50341031`](https://github.com/mbicz/yogabook-linux-kernel/commit/7b2b50341031cb58f1f7162fde22160ae0c7ce61) | Panel backlight, not camera capture. |
| 13 — edge LED | [`6309e98f79df`](https://github.com/mbicz/yogabook-linux-kernel/commit/6309e98f79dffd5aed183fd56514b5f59ca04320) | System awake indication, not camera capture. |
| 14 — firmware shutdown | [`5e1de0c89805`](https://github.com/mbicz/yogabook-linux-kernel/commit/5e1de0c8980581b7a977755fa4f19dca935a353a) | Power-off, not camera capture. |
| 15 — tablet switch | [`cc860287de21`](https://github.com/mbicz/yogabook-linux-kernel/commit/cc860287de21efa635c34332a9ff9a735ca6dafb) | Desktop tablet-mode reporting; independent of camera enumeration/capture. |
| 20 — duplicate GPS/virtual GPIO | [`c9c92dac92c4`](https://github.com/mbicz/yogabook-linux-kernel/commit/c9c92dac92c47f2d6724c49b91df40eed6459dbb) | Separate ACPI status quirks, not the camera DSDT correction. |
| 21 — i915 VBT backlight | [`10aa1f4d0fc4`](https://github.com/mbicz/yogabook-linux-kernel/commit/10aa1f4d0fc4ab42faca26fce07ab92d3d728783) | Panel backlight description, not camera capture. |

## Kernel configuration

The old `yogabook_defconfig` in the 6.17 candidate is **not a complete modern
camera configuration**. Start from a working full board configuration and enable
the following, retaining storage/root-filesystem and other machine requirements.
Use the target tree's Kconfig to resolve dependency selections; this is not a
complete replacement `.config`.

```text
CONFIG_X86_64=y
CONFIG_EFI=y
CONFIG_PCI=y
CONFIG_ACPI=y
CONFIG_ACPI_TABLE_UPGRADE=y
CONFIG_FW_LOADER=y
CONFIG_PM=y
CONFIG_COMMON_CLK=y
CONFIG_I2C=y
CONFIG_GPIOLIB=y
CONFIG_REGULATOR=y
CONFIG_LEDS_CLASS=y
CONFIG_STAGING=y
CONFIG_STAGING_MEDIA=y
CONFIG_MEDIA_SUPPORT=m
CONFIG_MEDIA_CAMERA_SUPPORT=y
CONFIG_MEDIA_PCI_SUPPORT=y
CONFIG_MEDIA_CONTROLLER=y
CONFIG_VIDEO_DEV=m
CONFIG_VIDEO_V4L2_I2C=y
CONFIG_VIDEO_V4L2_SUBDEV_API=y
CONFIG_VIDEO_CAMERA_SENSOR=y
CONFIG_VIDEO_CAMERA_LENS=y
CONFIG_INTEL_ATOMISP=y
CONFIG_VIDEO_ATOMISP=m
CONFIG_IPU_BRIDGE=m
CONFIG_INTEL_SKL_INT3472=m
CONFIG_VIDEO_OV2740=m
CONFIG_VIDEO_OV8858=m
CONFIG_VIDEO_WV517=m
CONFIG_PMIC_OPREGION=y
CONFIG_CHT_WC_PMIC_OPREGION=y
CONFIG_INTEL_SOC_PMIC_CHTWC=y
CONFIG_REGULATOR_WHISKEY_COVE=y
CONFIG_X86_INTEL_LPSS=y
CONFIG_I2C_DESIGNWARE_CORE=y
CONFIG_I2C_DESIGNWARE_PLATFORM=y
CONFIG_I2C_DESIGNWARE_BAYTRAIL=y
CONFIG_PINCTRL_CHERRYVIEW=y
```

Keep platform-clock support (`CONFIG_X86_INTEL_LPSS` and its selected PMC clock
driver) and the inherited Whiskey Cove PMIC/opregion support. `REGULATOR_WHISKEY_COVE`
is part of this board-support fork, not a promise about stock upstream Kconfig.
Disable conflicting sensor drivers: `CONFIG_VIDEO_ATOMISP_OV2722` and
`CONFIG_VIDEO_OV5693`; also disable the retired `CONFIG_VIDEO_ATOMISP_OV2740` if
that symbol exists in the chosen tree. The front `INT33FB` HID is shared with
OV2722, and rear `INT33BE` with OV5693. Do not load the legacy sensor in place of
the modern OV2740/OV8858 drivers.

Source of the driver dependencies:
[AtomISP Kconfig](https://github.com/mbicz/yogabook-linux-kernel/blob/10aa1f4d0fc4ab42faca26fce07ab92d3d728783/drivers/staging/media/atomisp/Kconfig),
[IPU bridge Kconfig](https://github.com/mbicz/yogabook-linux-kernel/blob/10aa1f4d0fc4ab42faca26fce07ab92d3d728783/drivers/media/pci/intel/Kconfig),
and [INT3472 Kconfig](https://github.com/mbicz/yogabook-linux-kernel/blob/10aa1f4d0fc4ab42faca26fce07ab92d3d728783/drivers/platform/x86/intel/int3472/Kconfig).

## Required camera ACPI correction

The kernel's camera-client allowlist is not enough: stock X90F firmware describes
the actual sensor wiring incorrectly. The corrective source is supplied here as
[`tools/patch_dsdt_cameras.py`](tools/patch_dsdt_cameras.py), **not** as a binary
DSDT dump or a kernel patch. A public stock-table dump is a reference, not an
already-corrected camera table.

The patcher accepts the known stock **OEM revision 3** DSDT and produces the
camera-only **OEM revision 6** DSDT, preserving table/package lengths and fixing
the ACPI checksum. It rejects already-overridden revisions and unexpected
patterns. It is specific to this X90F firmware, not a generic X91/Windows-model
DSDT conversion.

Corrections:

- CAM4/front `INT33FB`: move I2C4 address `0x36` to I2C3 address `0x10`;
  correct `CsiLanes` from 1 to 2; correct the powerdown GPIO sensor-on polarity
  (pin 54 must be high).
- CAM3/rear `INT33BE`: change I2C3 address `0x10` to `0x36`; replace unused
  `CDEP` with `_PR0 = { P28W, P18W, ^^^P33W }`. The bridge supplies the rear
  PMC clock separately. Leave the rear device present, not hidden by an old
  front-only `_STA=0` workaround.

### Generate from your own stock table

On the Yoga Book, first boot a known recovery/stock-table entry **without an
existing DSDT override**. Save your own unmodified firmware table, then run the
patcher as an ordinary user. Do not use the live table from an overridden boot
as the input:

```sh
mkdir -p acpi-work
sudo cat /sys/firmware/acpi/tables/DSDT > acpi-work/dsdt-stock.aml
python3 tools/patch_dsdt_cameras.py \
    acpi-work/dsdt-stock.aml acpi-work/dsdt-camera.aml
sha256sum acpi-work/dsdt-camera.aml
```

For the known firmware used here the camera-only output SHA-256 is
`0d1a87d41d0064fd1dba88c81c126d5f8aa22621b55102709e183bf384fec065`.
If the patcher rejects your table or the result differs, do not force offsets,
change revision checks or blindly install someone else's AML. Preserve the
original and investigate the firmware difference.

### Load as an early initrd, with a recoverable boot entry

Linux's authoritative procedure is
[Upgrading ACPI tables via initrd](https://docs.kernel.org/admin-guide/acpi/initrd_table_override.html).
It requires `CONFIG_ACPI_TABLE_UPGRADE=y`, matching table identity and a higher
OEM revision, plus an **uncompressed `newc` cpio at the beginning of the initrd**.
Copying AML into the ordinary compressed initramfs is not equivalent. Secure
Boot/kernel-lockdown policy can prevent table override; check the kernel log
rather than assuming it loaded.

After checking the generated table, prepare the early archive (this does not
change a bootloader or running kernel):

```sh
mkdir -p acpi-work/early/kernel/firmware/acpi
cp acpi-work/dsdt-camera.aml acpi-work/early/kernel/firmware/acpi/dsdt.aml
(cd acpi-work/early && find kernel -print | cpio -H newc --create) \
    > acpi-work/acpi-camera.cpio
```

For GRUB, install that archive as a separate file in `/boot`, then use a
**separate candidate boot entry**, preserving the working kernel and original
initrd entry unchanged. Its `initrd` line must load this archive before the
normal initrd (and keep any existing microcode archive in its required order).
For example, on a layout where `/boot` is GRUB's filesystem root:

```text
initrd /acpi-camera.cpio /initrd.img-<candidate-release>
```

Paths differ when `/boot` is inside the root filesystem. Copy the actual working
entry's disk/search/root settings; the example is only the archive order, not a
complete boot entry. For the first attempt, select/edit the candidate manually
for one boot rather than replacing the saved default. Keep console/physical
recovery available; a remote reboot should have a separately planned rollback.
After boot, check kernel ACPI-override messages, the loaded OEM revision and
camera enumeration before promoting an entry. The camera Debian package does
none of these boot changes automatically.

The original deployment instead used an initramfs-tools hook calling
`prepend_earlyinitramfs`; that local hook selected tables by `-cam` and `-thermal`
release tokens. Those tokens are **hook policy**, not a Linux camera ABI and not
needed by the explicit early-archive arrangement above.

The deployed `#yb7.8` kernel used a camera-preserving **thermal DSDT revision 7
and thermal SSDT revision 4**, not camera-only revision 6. The `7.8` kernel build
label is not an AML revision. Thermal corrections and native PMIC ADC support
are separate from camera bring-up; this project does not export that thermal
patcher or its AML. Reproducing that exact thermal deployment requires those
additional external artifacts. For camera-only setup, use the supplied stock→6
patcher, not thermal tables with an unrelated kernel.

## Required ISP firmware

The Cherry Trail ISP2401 requires `shisp_2401a0_v21.bin`, version
`irci_stable_candrpv_0415_20150521_0458`, SHA-256:

```text
e89359f4e4934c410c83d525e283f34c5fcce9cb5caa75ad8a32d66d3842d95c
```

[Upstream Linux's firmware instructions](https://github.com/torvalds/linux/blob/v6.10/drivers/staging/media/atomisp/TODO)
identify this exact version/hash and the
[Intel Aero firmware source](https://github.com/intel-aero/meta-intel-aero-base/blob/master/recipes-kernel/linux/linux-yocto/shisp_2401a0_v21.bin).
Obtain it separately, observe its licensing, check the hash and place it at
`/lib/firmware/intel/ipu/shisp_2401a0_v21.bin` before probing the driver. If AtomISP
loads from the initramfs, include the firmware there too. This project does not
redistribute it.

The [candidate firmware loader](https://github.com/mbicz/yogabook-linux-kernel/blob/10aa1f4d0fc4ab42faca26fce07ab92d3d728783/drivers/staging/media/atomisp/pci/atomisp_v4l2.c)
requests the `intel/ipu/` path, then falls back to the old top-level filename.
Do not substitute the ISP2400/Bay Trail or ISP2401-legacy firmware variants.

## Separate userspace colourimetry requirement

The kernel's full-range BT.601 metadata fix (topic 18) is distinct from the
GStreamer/PipeWire colourimetry fix. The deployed stack used
`gstreamer1.0-pipewire 1.4.2-1+0yb1` with upstream PipeWire commit
[`7795e06563dbb18112a4f63524f1f27f5a88c86e`](https://github.com/PipeWire/pipewire/commit/7795e06563dbb18112a4f63524f1f27f5a88c86e),
backported in [`pipewire-1.4.2-colourimetry.patch`](pipewire-1.4.2-colourimetry.patch).
Use a version containing the equivalent fix or apply the appropriate backport.
Installing this camera package alone does not rebuild the PipeWire GStreamer
plugin. This is not a kernel patch or a reason to copy private ISP headers.

## Operational expectations

- The shared ISP exposes both sensors as inputs of one V4L2 output, usually
  `/dev/video0`; only one camera streams at a time. Select the correct device
  with the daemon's `--device` option if enumeration differs.
- The service user needs access to the video device and, for rear autofocus,
  the related media and lens subdevice nodes (typically the `video` group).
  Probe order can change subdevice numbering.
- Disable competing capture of the raw AtomISP device. The supplied WirePlumber
  policy prevents its ordinary raw-device node from competing with this daemon.
- A rare boot-time OV2740 probe NACK was observed. Missing cameras after a boot
  are a driver/probe issue, not evidence that the PipeWire nodes should be
  recreated with different hardcoded input numbers.
- Do not unload/reload AtomISP as a generic recovery recipe: bridge reprobe and
  firmware failure paths have wedged the device during development. A controlled
  reboot into a known-good entry is safer than repeated streaming/probe attempts.
