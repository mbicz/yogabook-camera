# yogabook-camera

`yb-camerad` exposes the Lenovo Yoga Book YB1-X90F's two cameras as PipeWire
sources. The sensors share one AtomISP capture device, so only one can stream
at a time. Select the camera in the application.

| PipeWire node | Sensor | Capture mode |
| --- | --- | --- |
| `yb-camera-front` | OV2740 | 1920 × 1080 I420, about 30 fps |
| `yb-camera-rear` | OV8858 | 1600 × 1212 I420, about 24 fps |

The daemon opens the capture device only while an application is streaming.
It rotates frames to follow the display and adjusts the rear image for the
hinge position. Rear autofocus uses the WV517 lens driver. Exposure, white
balance and colour correction run in the kernel.

## Requirements

The integration targets Debian 13, amd64, GNOME on Wayland, PipeWire and
WirePlumber 0.5. Display rotation uses Mutter's D-Bus interface.

The kernel needs the Yoga Book camera fixes. See [KERNEL.md](KERNEL.md) for
pinned source revisions, configuration, ISP firmware and ACPI corrections.
Installing this package does not install those prerequisites. Firmware and
prebuilt ACPI tables are not included.

## Build and install

On amd64 Debian:

```sh
sudo apt install git build-essential dpkg-dev debhelper pkgconf \
    libpipewire-0.3-dev libspa-0.2-dev libsystemd-dev
git clone https://github.com/mbicz/yogabook-camera.git
cd yogabook-camera
dpkg-buildpackage -us -uc -b
sudo apt install ../yogabook-camera_0.1_amd64.deb
```

The build uses standard Linux headers; it does not need a kernel source tree.
`make` builds the daemon alone. Install through the Debian package, not
`sudo make install`: the latter bypasses the Snapshot diversion and can
overwrite the application's executable.

Close camera applications before installation. Log out and back in afterwards
to start the user service and load the WirePlumber policy.

## What the package changes

- Enables `yb-camerad.service` for PipeWire user sessions.
- Hides the raw AtomISP device from WirePlumber so it does not compete with the
  daemon.
- Sets `atomisp nr=1` for the module's next load. This requires the kernel's
  noise-reduction parameter patch.
- Installs a system-wide Snapshot wrapper. The original executable is preserved
  as `/usr/bin/snapshot.distrib` using `dpkg-divert`.

The wrapper sets `GDK_DISABLE=gl,offload` to avoid the GTK/Mesa crashes observed
on this hardware. It also chooses a posture-based camera default when Snapshot
starts. Camera selection remains manual while the application is running.
Snapshot is recommended, not required by the daemon.

To inspect the service:

```sh
systemctl --user status yb-camerad.service
journalctl --user -u yb-camerad.service
```

Do not open the raw capture device with another tool while the daemon is using
it. Firmware faults can wedge capture; see [KERNEL.md](KERNEL.md) for operational
limits.

## Limitations

This is an early release. Colour, exposure and focus still need testing across
more lighting conditions. Dark or low-detail scenes can prevent autofocus from
converging. There is no tap-to-focus or full-resolution still-capture mode;
GNOME Camera saves frames from the preview stream.

Older PipeWire GStreamer plugins can discard colour metadata. The included
[backport](pipewire-1.4.2-colourimetry.patch) is for PipeWire 1.4.2 and is not
installed by this package. Prefer a distribution version containing the upstream
fix; see [KERNEL.md](KERNEL.md#separate-userspace-colourimetry-requirement).

## Remove

Close camera applications, then run:

```sh
systemctl --user stop yb-camerad.service
sudo apt remove yogabook-camera
```

Removal restores the original Snapshot executable. Log out and back in to drop
the WirePlumber policy. The kernel, firmware, ACPI overrides and per-user camera
preferences are left unchanged.

## License

[GPL-2.0-or-later](COPYING). The PipeWire backport is
[MIT-licensed](LICENSES/MIT-PipeWire.txt); attribution is in
[debian/copyright](debian/copyright).
