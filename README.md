# yogabook-camera

Camera userspace for the **Lenovo Yoga Book YB1-X90F** (Android-model Cherry Trail): `yb-camerad` presents the front OV2740 and rear OV8858, which share one AtomISP capture device, as two selectable PipeWire cameras.

Project: <https://github.com/mbicz/yogabook-camera>

Version **0.1** is the first public, early release. It exports the daemon previously deployed under local development package version 1.1.7; that numbering is not this project's public release history. The daemon source is unchanged. This repository contains its active Makefile, Debian packaging and desktop integration, not the separate experimental GStreamer plugin from the original development tree.

## Hardware and desktop prerequisites

The supported integration context is **Debian 13 (trixie), amd64 Linux, GNOME on Wayland/Mutter, PipeWire and WirePlumber 0.5** on the YB1-X90F. This is hardware-specific software, not a general webcam or libcamera driver. Other Yoga Book models, desktops and stock Debian 12's older WirePlumber configuration format are not validated targets.

**Installing this package alone does not bring up cameras on a stock kernel.** Read [KERNEL.md](KERNEL.md) for the exact public kernel branch/commit, configuration, firmware and ACPI requirements and the distinction between deployed LTS evidence and the unbooted exact submission branch. The included `tools/patch_dsdt_cameras.py` operates on a user-supplied stock OEM-revision-3 DSDT; it does not install an override. Do not apply it blindly to an already overridden or different model's table. No firmware binaries, AIQB calibration files, machine dumps or prebuilt ACPI tables are distributed here.

The daemon uses standard Linux V4L2/media interfaces and links PipeWire and libsystemd. Its build does **not** require a surrounding repository, a kernel source tree, private AtomISP ioctls or special AtomISP kernel headers. At runtime the session user must have access to the capture/media/lens devices, PipeWire and the session D-Bus. Screen rotation follows `org.gnome.Mutter.DisplayConfig`; rear posture handling uses the hinge IIO sensor.

## Camera behavior

| PipeWire node | Sensor | Capture mode before screen rotation |
| --- | --- | --- |
| `yb-camera-front` / Yoga Book front camera | OV2740 | 1920 × 1080 I420, approximately 30 fps |
| `yb-camera-rear` / Yoga Book rear camera | OV8858 | 1600 × 1212 I420, approximately 24 fps, 2× binned |

Both cameras are published for **manual selection in the application** (for example GNOME Camera's switch-camera button). There is no third full-resolution rear node. Both share one physical capture device and cannot capture independently at the same time. The daemon opens `/dev/video0` only while an application streams; it is not an always-on recording service.

The service's rotation tables keep the image upright as the screen turns. A mid-stream orientation change keeps the negotiated size and may letterbox the image rather than renegotiating it. The rear camera is on the keyboard half: the service applies an additional turn and selfie-style mirroring when unfolded. This does not automatically replace an application's manually selected camera. The optional Snapshot wrapper chooses a posture-based default **on application launch**, then leaves manual switching available.

Rear contrast autofocus discovers the associated WV517 lens through the media graph and searches the module's calibrated range. It waits for scene motion to settle, favors stable detail and holds focus until the scene changes. Dark, flat or moving scenes can prevent convergence. Video remains available without a usable lens driver, but that is not evidence of working autofocus. There is no tap-to-focus, focus-area UI or general camera-control panel supplied by this project.

### Image-quality limits

This bridge does not promise stock Android image quality. Color, highlight handling, exposure and remaining softness/noise have not received final visual acceptance. Much of that processing belongs to the kernel/ISP, not PipeWire pixels. The installed `nr=1` option selects the stock ISP noise-reduction preset instead of the earlier extra-smoothing preset; it does not eliminate grain or cure every detail problem. No compensating software sharpening, saturation or gain filter is applied here.

GNOME Camera takes photos and video from its preview stream: rear stills therefore remain 1600 × 1212, not the sensor's nominal full resolution. The experimental full-sensor rear stream was removed because its approximately 12 fps preview was not acceptable. Frame rate also depends on exposure, system load and the kernel.

## Build a Debian package (supported installation)

Build on an **amd64 Debian** system; these commands are not macOS build instructions. Install the normal build tools and package dependencies:

```sh
sudo apt update
sudo apt install build-essential dpkg-dev debhelper pkgconf \
    libpipewire-0.3-dev libspa-0.2-dev libsystemd-dev
git clone https://github.com/mbicz/yogabook-camera.git
cd yogabook-camera
dpkg-buildpackage -us -uc -b
```

The native package version is 0.1; the resulting binary package is placed in the parent directory. Install it with APT so runtime dependencies are resolved:

```sh
sudo apt install ../yogabook-camera_0.1_amd64.deb
```

`pipewire` and `wireplumber` are package dependencies. `snapshot` (GNOME Camera) is recommended, not required by the daemon. Use distribution packages and normal security updates; do not hold or downgrade old PipeWire, GTK, Mesa or camera packages to reproduce the development machine.

If you already have a local development package numbered 1.1.x, Debian considers 0.1 a downgrade even though this is the first public release. Keep the installed development package unless deliberately replacing it; do not automate that downgrade or infer that public 0.1 is a newer camera algorithm.

The Makefile can also compile only the daemon with `make`, and `make clean` removes that binary. **Do not use `sudo make install` on a live system:** it overwrites `/usr/bin/snapshot` without executing the Debian `dpkg-divert` hooks. `make install DESTDIR=...` is a packaging/staging operation, not the supported installation method. Debian rules explicitly select this Makefile.

## Installed integration and system-wide effects

The package installs:

- `/usr/bin/yb-camerad` and `/usr/lib/systemd/user/yb-camerad.service`. The user service is enabled for PipeWire sessions through Debian's systemd integration; system accounts such as the GDM greeter are excluded. It runs as the session user, not root.
- `/usr/share/wireplumber/wireplumber.conf.d/60-yb-camera.conf`. This WirePlumber 0.5 rule hides the raw AtomISP V4L2 device at PCI `0000:00:03.0`, avoiding a duplicate, unrotated, ambiguously selected camera node. It is system-wide policy for users of this machine.
- `/usr/lib/modprobe.d/atomisp-yogabook-camera.conf` with `options atomisp nr=1`. This changes the module's next-load option, not an already loaded module parameter. It does not replace other administrators' AtomISP option files. Arrange any reload or reboot through your normal safe kernel workflow, with camera apps closed.
- `/usr/bin/snapshot`, a wrapper, while Debian diverts the application's real executable to `/usr/bin/snapshot.distrib`. This affects **all launches and all users**, including desktop launchers; it is not a private shell alias.

Close camera applications before changing session services. On an already running user session, log out and back in so both the service and WirePlumber policy take effect. Inspect the daemon without running a camera test:

```sh
systemctl --user status yb-camerad.service
journalctl --user -u yb-camerad.service
```

Avoid other tools opening the raw AtomISP device concurrently with the daemon. Firmware faults can wedge capture and may require a reboot; repeatedly opening a wedged camera is not a recovery procedure.

### Snapshot workaround and optional application use

The daemon works independently of Snapshot. The shipped wrapper addresses GL-thread crashes observed with Mesa 25.0.7 and GTK 4.18 offload failures on this hardware by launching Snapshot with `GDK_DISABLE=gl,offload`. It also writes the current user's `org.gnome.Snapshot last-camera-id` setting to the posture-preferred camera. It does not lock camera selection, rotate the screen or modify desktop orientation settings.

The wrapper is included whenever this package is installed; there is no separate opt-in package switch. Users who do not want Snapshot can omit APT recommendations when installing, and use another PipeWire camera application. Once Snapshot is installed, `/usr/bin/snapshot.distrib` remains the original application and can be invoked directly to **bypass** the workaround (and the posture-default selection), accepting any underlying graphics bugs. Do not manually delete the wrapper or diversion while the package owns them.

## Remove

Close camera apps and stop the current session's daemon first:

```sh
systemctl --user stop yb-camerad.service
sudo apt remove yogabook-camera
```

Removal deletes the packaged integration and removes the diversion, restoring the distribution Snapshot executable if installed. It does not uninstall Snapshot, PipeWire or the kernel, remove an independently installed ACPI override, reset Snapshot's per-user camera preference, or change the value of an already loaded AtomISP module. Log out and back in to discard the old WirePlumber policy. Other logged-in users must also end or stop their running daemon sessions; removing files does not stop every user's process automatically.

## Optional PipeWire GStreamer colourimetry backport

`pipewire-1.4.2-colourimetry.patch` is a companion **source patch**, not compiled or installed by this package. It backports Michael Olbrich's upstream [colourimetry support commit](https://github.com/PipeWire/pipewire/commit/7795e06563dbb18112a4f63524f1f27f5a88c86e) to PipeWire **1.4.2**, mapping all colourimetry values available in that version's headers. Its header records the exact base file and hashes; absent newer enum mapping tails were omitted in the backport.

The daemon advertises full-range BT.601 output with BT.709 transfer/primaries. A PipeWire GStreamer consumer without colourimetry propagation can drop this metadata and interpret the video incorrectly. This is separate from sensor/ISP colour quality. Prefer an up-to-date distribution PipeWire containing the upstream change. Only if maintaining a 1.4.2-based system that still lacks it, apply the patch in the matching PipeWire source package and rebuild/install the affected GStreamer plugin (`gstreamer1.0-pipewire`) through that distribution's package workflow. Do not apply it blindly to newer or mismatched sources, pin obsolete binary packages, or treat it as a prerequisite for compiling `yb-camerad`.

## Source scope and licensing

This standalone tree deliberately excludes the old `CMakeLists.txt`, `gstyogabook*` and `atomisp-control.*` plugin and the original `test/` hardware-lab scripts. Those scripts contain hardcoded development user/UID assumptions, process kills, service stops and display rotation; they are not portable automated tests and are not shipped or invoked here. There is no automatic hardware test target. The ACPI patcher in `tools/` is a separate, explicitly documented setup utility, not a test or install hook.

Project source and integration are Copyright © 2026 Michal Bicz, licensed under **GPL-2.0-or-later**; see [COPYING](COPYING). The companion PipeWire patch is **MIT**, with upstream implementation by Michael Olbrich and base-file Copyright © 2018 Wim Taymans; see [LICENSES/MIT-PipeWire.txt](LICENSES/MIT-PipeWire.txt) and [debian/copyright](debian/copyright). Its upstream author/provenance header is retained.
