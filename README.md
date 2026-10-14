# ELAN 04f3:0903 Linux Fingerprint Sensor Driver (libfprint)

[![Hardware](https://img.shields.io/badge/Hardware-ASUS%20VivoBook%2016X%20(M1603QA)-blue.svg)](#hardware)
[![Sensor](https://img.shields.io/badge/Sensor-ELAN%2004f3%3A0903-orange.svg)](#hardware)
[![Driver](https://img.shields.io/badge/Driver-elanpress%20(SIFT%20Matching)-success.svg)](#how-the-driver-works)
[![License: LGPL 2.1](https://img.shields.io/badge/License-LGPL%202.1-green.svg)](COPYING)

A fork of **libfprint** with an `elanpress` driver for the **ELAN `04f3:0903`**
press-type fingerprint sensor in the touchpad of ASUS laptops (VivoBook 16X /
M1603QA), plus an installer for Omarchy (Arch + Hyprland + SDDM) that sets up
tap-to-unlock with a failure lockout.

## Contents
1. [Hardware](#hardware)
2. [Why upstream libfprint fails](#why-upstream-libfprint-fails)
3. [How the driver works](#how-the-driver-works)
4. [Install on Omarchy / Arch](#install-on-omarchy--arch)
5. [Enrolling](#enrolling)
6. [Authentication policy](#authentication-policy)
7. [Booting straight to a fingerprint prompt](#booting-straight-to-a-fingerprint-prompt)
8. [Measuring accuracy](#measuring-accuracy)
9. [Building on other distributions](#building-on-other-distributions)
10. [Troubleshooting](#troubleshooting)
11. [License](#license)

---

## Hardware

| Attribute | Specification |
| :--- | :--- |
| **Tested laptop** | ASUS VivoBook 16X (`M1603QA`) |
| **Sensor** | Elan Microelectronics Corp. ELAN:Fingerprint, `04f3:0903` (bcdDevice 1.70) |
| **Form factor** | Small capacitive press sensor in the touchpad corner |
| **USB endpoints** | EP1 OUT `0x01` (commands), EP3 IN `0x83` (replies), EP2 IN `0x82` (image stream); bulk, full speed |
| **Tested environment** | Arch Linux / Omarchy, Hyprland (Wayland), SDDM, fprintd 1.94 |

## Why upstream libfprint fails

Upstream lists `04f3:0903` under the `elan` *swipe* driver, which fails with
`Calibration failed!`, `No minutiae found` or protocol errors:

1. **Swipe vs. press.** The swipe driver stitches frames of a finger moving
   across the sensor. On a press sensor the finger does not move, so there is
   nothing to stitch.
2. **Too small for minutiae.** NBIS/Bozorth3 matching needs ridge endings and
   bifurcations from a large area; this sensor sees a few millimetres of
   finger per press.

## How the driver works

`04f3:0903` is claimed by the `elanpress` driver (`libfprint/drivers/elanpress*.c`)
instead of `elan`.

**Capture.** Presence is detected from the image: each polled frame is
compared with a background frame of the empty sensor, and a frame counts as
touched once 12% of its pixels sit clearly above it. The touch is imaged as
soon as its contact area stops growing (typically 40-60 ms after landing, at
most 150 ms). A tap that lifts sooner is not dropped; its fullest frame is
used. The background is kept between authentications and corrected
automatically if it was ever taken with a finger on the sensor.

**Matching.** SIFT keypoints are extracted from the background-subtracted
image (`elanpress-sift.c`). A press matches an enrolled touch when at least
12 keypoint pairs, each a clear nearest neighbour by descriptor (Lowe's ratio
test), agree on one rigid placement of one image on the other, and the
agreeing keypoints are spread out rather than clustered on a local artefact
(`elanpress-match.c`). Enrollment stores 12 touches. Each press is decided on
its own: no best-of-N resampling, which inflates a stranger's score as much
as the owner's.

**Templates.** Only the keypoints of each enrolled touch are stored (print
format 2), not images. Prints from older builds (format 1, which stored the
images) still verify, but should be re-enrolled.

**Robustness.** USB transfers always run to completion and cancellation takes
effect between them. Aborting a frame read halfway used to wedge the sensor
so that it dropped off the bus on the next open, which happened every 30 s
on the lock screen. At open, the sensor is stopped and stale data is drained;
USB errors during capture clear the endpoint halts and restart the capture.

## Install on Omarchy / Arch

```bash
git clone https://github.com/HilalAhmad01/Fingerprint-sesnor-driver-.git
cd Fingerprint-sesnor-driver-
sudo ./system/setup.sh                    # sudo/polkit stay password-only
sudo ./system/setup.sh --fingerprint-sudo # fingerprint for sudo/polkit too
```

The installer:

1. builds this tree as a pacman package, `libfprint-elanpress`
   (`packaging/arch/PKGBUILD`, unit tests run during the build), and installs
   it in place of `libfprint`/`libfprint-git`. Pacman owns every file, so
   `sudo pacman -S libfprint` restores the stock library. It also removes the
   `NoExtract`/`IgnorePkg` lines an earlier version of this setup added to
   `/etc/pacman.conf`.
2. applies the [authentication policy](#authentication-policy) and installs
   `fprint-pam-guard.path`, which re-applies it whenever Omarchy rewrites one
   of the PAM files (it does on updates).
3. sets up the [boot flow](#booting-straight-to-a-fingerprint-prompt).

Backups of every file it changes are left next to it with a
`.fprint-elanpress.bak` suffix. Re-run it after changing the driver.

## Enrolling

```bash
fprintd-enroll -f right-index-finger
fprintd-verify
```

Press and lift 12 times. Move the finger a little between presses so that
different parts of it are stored; a press only matches a stored touch it
overlaps. If you enrolled with an older build, re-enroll
(`fprintd-delete $USER` first) so the stored images are replaced by keypoints.

## Authentication policy

Applied by `system/fprint-pam-apply`, configured in `/etc/fprint-elanpress.conf`.

| Where | How |
| :--- | :--- |
| **Lock screen** | Tap to unlock. Each press is its own attempt. After **5 rejected presses in a row**, fingerprint is disabled until the password unlocks the screen. Sensor errors and timeouts do not count. |
| **sudo, polkit** | Password only (default). With `--fingerprint-sudo`: fingerprint first with its own 5-strike lockout, then password. |
| **SDDM greeter** | Password only. |
| **Lid closed** | Fingerprint is skipped (`omarchy-hw-laptop-closed`). |

The lockout uses `pam_faillock` with its own tally (`/var/lib/fprint-faillock`).
The lock screen authenticates as your user, so its tally directory belongs to
you; `sudo faillock --dir /var/lib/fprint-faillock/lock --user $USER` shows it.

## Booting straight to a fingerprint prompt

SDDM logs you in automatically and the session locks itself at once
(`fprint-boot-lock`, started from Hyprland's autostart), so boot ends at the
lock screen waiting for a tap. Only autologin sessions are locked, and the
lock **fails closed**: if the lock screen is not up within the time limit,
the session is terminated and SDDM asks for the password.

Know what this does and does not protect:

* There is a short window (about a second) between the session starting and
  the lock screen appearing.
* **A login screen does not protect data on an unencrypted disk.** Anyone
  holding the laptop can boot a USB stick and read or change everything,
  fingerprint templates included. Windows Hello is backed by BitLocker,
  Secure Boot and the TPM; the Linux equivalent is LUKS full-disk encryption
  unlocked by the TPM (`systemd-cryptenroll --tpm2-device=auto`, with Secure
  Boot enabled and the `sd-encrypt` initramfs hook), which also boots without
  typing a passphrase.
* Do **not** put a LUKS keyfile into the initramfs to skip the passphrase.
  The initramfs sits on the unencrypted EFI partition, so the key is readable
  by anyone with the disk, and the encryption protects nothing. (An earlier
  version of this README described that procedure.)
* Setting a firmware (BIOS) supervisor password and disabling USB boot raises
  the bar for the USB-stick attack on an unencrypted disk, cheaply.

## Measuring accuracy

The threshold of 12 pairs comes from 40 genuine and 30 impostor presses,
far too few to bound the false accept rate a lock screen needs (1 in 10,000
or better). To measure it on your own sensor:

```bash
sudo systemctl stop fprintd
sudo mkdir -m 700 /root/presses
sudo ELANPRESS_DUMP_DIR=/root/presses /usr/lib/fprintd -t   # fprintd's path may differ
# in another terminal: fprintd-verify repeatedly, one finger at a time
```

Sort the dumped `*.pgm` files into one directory per finger (fingers of other
people are just more directories), then:

```bash
meson setup build -Ddrivers=elan,elanpress && ninja -C build
./build/tools/elanpress-eval /root/presses/sorted
```

It prints false reject and false accept rates for a range of thresholds.
The dumps are pictures of fingerprints: delete them afterwards.

## Building on other distributions

```bash
meson setup build --prefix=/usr -Ddrivers=elan,elanpress -Ddoc=false -Dgtk-examples=false
ninja -C build
meson test -C build --suite unit-tests
```

Dependencies: meson, ninja, a C compiler, glib2, libgusb, libgudev,
gobject-introspection (Debian/Ubuntu: `libglib2.0-dev libgusb-dev
libgudev-1.0-dev libgirepository1.0-dev`; Fedora: `glib2-devel libgusb-devel
libgudev-devel gobject-introspection-devel`). Install it through your
distribution's packaging rather than `ninja install` over the system
libfprint; the PAM policy scripts in `system/` are written for Omarchy.

## Troubleshooting

**Debug log.**
```bash
sudo systemctl stop fprintd
sudo G_MESSAGES_DEBUG=all /usr/lib/fprintd -t
```
Look for `elanpress` lines: `poll: touch (coverage ...)`, `touch imaged after
N ms`, and `MATCH` / `no match` with the pair count.

**Fingerprint stopped working on the lock screen.** You may have hit the
5-strike lockout; unlock once with the password.

**Sensor keeps disconnecting** (`journalctl -k | grep "usb 1-3"`). Builds
before this one wedged the sensor when a verify was cancelled mid-frame.
Make sure the installed package is current: `pacman -Q libfprint-elanpress`.

**Light taps not recognised.** Run the debug log and tap: if no
`poll: touch` lines appear, the touch did not reach the 12% coverage /
`ELANPRESS_TOUCH_PIXEL_DELTA` thresholds in `elanpress-match.h`.

## License

* LGPL-2.1-or-later ([COPYING](COPYING)).
* Based on [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint), including NBIS software from NIST.
* The `elanpress` driver builds on the press-sensor work in [filip-rs/libfprint](https://github.com/filip-rs/libfprint/tree/elanpress).
