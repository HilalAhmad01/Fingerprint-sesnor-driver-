# ELAN 04f3:0903 Linux Fingerprint Sensor Driver (libfprint)

[![Hardware](https://img.shields.io/badge/Hardware-ASUS%20VivoBook%2016X%20(M1603QA)-blue.svg)](#hardware--system-specifications)
[![Sensor](https://img.shields.io/badge/Sensor-ELAN%2004f3%3A0903-orange.svg)](#hardware--system-specifications)
[![Driver](https://img.shields.io/badge/Driver-elanpress%20(SIFT%20Matching)-success.svg)](#how-the-driver-works)
[![License: LGPL 2.1](https://img.shields.io/badge/License-LGPL%202.1-green.svg)](COPYING)

A specialized fork of **libfprint** providing stable, production-ready press-mode fingerprint support for the **ELAN Microelectronics `04f3:0903`** biometric sensor embedded in ASUS laptops (such as the ASUS VivoBook 16X / M1603QA).

This project integrates a dedicated `elanpress` driver utilizing **SIFT (Scale-Invariant Feature Transform)** keypoint matching, tuned capacitive calibration routines, and complete integration guides for **SDDM, Hyprland/Wayland lock screens, sudo, Polkit, and LUKS boot-to-desktop unlock**.

---

## Table of Contents
1. [Hardware & System Specifications](#hardware--system-specifications)
2. [Root Cause: Why Upstream libfprint Fails](#root-cause-why-upstream-libfprint-fails)
3. [How This Driver Works](#how-this-driver-works)
4. [Prerequisites & Dependencies](#prerequisites--dependencies)
5. [Building and Installing](#building-and-installing)
6. [Preventing Package Manager Overwrites (Arch Linux)](#preventing-package-manager-overwrites-arch-linux)
7. [Enrolling and Verifying Fingerprints](#enrolling-and-verifying-fingerprints)
8. [PAM Setup (Sudo, Lock Screen, SDDM, Polkit)](#pam-setup-sudo-lock-screen-sddm-polkit)
9. [Seamless Boot-to-Desktop Unlock (LUKS + Hyprland)](#seamless-boot-to-desktop-unlock-luks--hyprland)
10. [Troubleshooting & Debug Logging](#troubleshooting--debug-logging)
11. [License & Acknowledgements](#license--acknowledgements)

---

## Hardware & System Specifications

| Attribute | Specification |
| :--- | :--- |
| **Tested Laptop** | ASUS VivoBook 16X (`M1603QA`) |
| **Fingerprint Sensor** | Elan Microelectronics Corp. ELAN:Fingerprint |
| **USB Vendor & Product ID** | `04f3:0903` (bcdDevice: 1.70) |
| **Sensor Form Factor** | Capacitive touch square embedded in touchpad corner (~96×96 or 144×64) |
| **USB Endpoints** | • EP 1 OUT (`0x01`), EP 1 IN (`0x81`) [Bulk, 64B]<br>• EP 2 IN (`0x82`) [Bulk, 64B] (Image stream)<br>• EP 3 OUT (`0x03`), EP 3 IN (`0x83`) [Bulk, 64B] (Commands) |
| **Tested Environment** | Arch Linux (Kernel 6.x / 7.x), Hyprland (Wayland), SDDM, fprintd |

---

## Root Cause: Why Upstream libfprint Fails

Although USB ID `04f3:0903` is listed in upstream `libfprint/drivers/elan.c`, stock versions fail with errors such as:
* `Device reported an error during enroll: Calibration failed!`
* `Failed to detect minutiae: No minutiae found`
* `Device reported an error during identify for enroll: The driver encountered a protocol error with the device.`

### The Underlying Issues:
1. **Swipe vs. Press Mismatch**: The physical sensor is a static press/touch sensor. Upstream `elan.c` treats it as a swipe sensor (`FP_SCAN_TYPE_SWIPE`) running multi-frame motion assembly (`fpi_assemble_frames`). Because the user simply places their finger on the sensor without swiping, frame assembly produces zero displacement and minutiae extraction fails.
2. **Minutiae Scarcity on Small Sensors**: Traditional NBIS / Bozorth3 fingerprint matching relies on ridge endings and bifurcations across large scan areas. A small touchpad corner sensor produces too few minutiae points to meet Bozorth3 matching thresholds.
3. **Calibration Timeouts & Drift**: `elan_calibrate` uses a strict delta threshold (`ELAN_CALIBRATION_MAX_DELTA = 500`) and aborts after only 10 attempts if baseline readings drift or status transitions take longer than 50ms.
4. **USB Protocol Desync**: When an operation times out, lack of clear-halt / endpoint reset causes subsequent USB transfers to fail with protocol errors.

---

## How This Driver Works

This repository resolves all aforementioned issues through targeted driver enhancements and a dedicated matching engine:

### 1. Dedicated `elanpress` Driver Pipeline
In `libfprint/drivers/elan.h` and `elanpress.h`, device `04f3:0903` is routed to the dedicated `elanpress` driver rather than the legacy swipe driver:
* **Press Capture Mode**: Treats captures as static touch snapshots rather than swipe sequences.
* **Touch Settling Delay**: Introduces a 150ms settling window for hardware status transitions (`0x00 -> 0x01 -> 0x03`), eliminating capture truncation.
* **Calibration Enhancements**: Increases calibration attempts from 10 to 30, relaxes the baseline delta threshold to 1000, and skips redundant re-calibration during verification if valid background data already exists.

### 2. SIFT Keypoint Feature Extraction (`elanpress-sift.c`)
Instead of depending solely on NBIS minutiae, this driver implements **Scale-Invariant Feature Transform (SIFT)** keypoint detection:
* Extracts local scale-space extreme points and gradient orientation descriptors directly from raw capacitive press frames.
* Highly robust to partial finger placements, minor rotations, and skin moisture variations.

### 3. Multi-Stage Gallery Matching (`elanpress-match.c`)
* Builds a **12-stage enrollment gallery**, assembling a comprehensive keypoint database for each enrolled finger.
* Verification matches the probe frame against the enrolled gallery using nearest-neighbor Euclidean distance with Lowe's ratio test, providing fast and reliable matching (< 200ms).

---

## Prerequisites & Dependencies

### Arch Linux / Omarchy:
```bash
sudo pacman -S --needed \
    base-devel \
    git \
    meson \
    ninja \
    gcc \
    pkgconf \
    glib2 \
    libusb \
    pixman \
    nss \
    gobject-introspection \
    gtk-doc \
    fprintd \
    polkit \
    pam
```

### Ubuntu / Debian:
```bash
sudo apt-get update && sudo apt-get install -y \
    build-essential \
    meson \
    ninja-build \
    libglib2.0-dev \
    libusb-1.0-0-dev \
    libpixman-1-dev \
    libnss3-dev \
    libgirepository1.0-dev \
    fprintd \
    libpam-fprintd
```

### Fedora:
```bash
sudo dnf install -y \
    meson \
    ninja-build \
    gcc \
    glib2-devel \
    libusb1-devel \
    pixman-devel \
    nss-devel \
    gobject-introspection-devel \
    fprintd \
    fprintd-pam
```

---

## Building and Installing

### 1. Clone the Repository
```bash
git clone https://github.com/HilalAhmad01/Fingerprint-sesnor-driver-.git libfprint
cd libfprint
```

### 2. Configure with Meson
Ensure the `elan` and `elanpress` drivers are enabled:
```bash
meson setup builddir \
    --prefix=/usr \
    --sysconfdir=/etc \
    -Ddrivers=elan,elanpress \
    -Dgtk-examples=false \
    -Ddoc=false
```

### 3. Compile and Install
```bash
ninja -C builddir
sudo ninja -C builddir install
```

### 4. Restart the Fingerprint Daemon
```bash
sudo systemctl restart fprintd
```

### 5. Verify the Driver Mode
Check that `fprintd` now recognizes the sensor as a press sensor:
```bash
fprintd-list "$USER"
```
**Expected output:**
```text
found 1 devices
Device at /net/reactivated/Fprint/Device/0
Using device /net/reactivated/Fprint/Device/0
Fingerprint reader: ElanTech press-type fingerprint sensor (press)
```

---

## Preventing Package Manager Overwrites (Arch Linux)

Package managers (such as Arch Linux's `pacman`) may overwrite `/usr/lib/libfprint-2.so` when system updates pull `libfprint` or `libfprint-git`.

To prevent updates from overwriting your custom driver:

```bash
sudo sed -i '/^\[options\]/a NoExtract = usr/lib/libfprint*\nIgnorePkg = libfprint libfprint-git' /etc/pacman.conf
```

If your driver is ever replaced following a manual package reinstall, simply re-run:
```bash
cd /path/to/libfprint
sudo ninja -C builddir install
sudo systemctl restart fprintd
```

---

## Enrolling and Verifying Fingerprints

### 1. Enroll Your Finger
Run enrollment for your desired finger (e.g. `right-index-finger`):
```bash
fprintd-enroll -f "right-index-finger"
```

> **Enrollment Tip**: When prompted to place your finger, lift and press firmly on the sensor. Vary the angle slightly with each of the 12 touches to build a rich keypoint gallery.

### 2. Verify Enrolled Finger
```bash
fprintd-verify -f "right-index-finger"
```
Place your finger on the sensor; you should see:
```text
Verify result: verify-match (done)
```

---

## PAM Setup (Sudo, Lock Screen, SDDM, Polkit)

To enable fingerprint authentication across all desktop access points (terminal sudo, lock screen, display manager login, and GUI permission prompts), configure PAM.

### One-Shot Installation Script
Run the following script with `sudo` to configure all PAM services:

```bash
sudo bash -c '
# 1. SDDM Login Screen
cat << "EOF" > /etc/pam.d/sddm
#%PAM-1.0
auth        [success=1 default=ignore] pam_exec.so quiet /usr/bin/omarchy-hw-laptop-closed
auth        sufficient  pam_fprintd.so
auth        include     system-login
-auth       optional    pam_kwallet5.so

account     include     system-login
password    include     system-login

session     optional    pam_keyinit.so          force revoke
session     include     system-login
-session    optional    pam_gnome_keyring.so    auto_start
-session    optional    pam_kwallet5.so         auto_start
EOF

# 2. Hyprland Lock Screen
cat << "EOF" > /etc/pam.d/omarchy-lock-fingerprint
#%PAM-1.0
auth       required                    pam_fprintd.so
account    include                     system-local-login
EOF

# 3. Terminal Sudo
cat << "EOF" > /etc/pam.d/sudo
#%PAM-1.0
auth        [success=1 default=ignore] pam_exec.so quiet /usr/bin/omarchy-hw-laptop-closed
auth        sufficient  pam_fprintd.so
auth        include     system-auth
account     include     system-auth
session     include     system-auth
session     optional    pam_systemd.so class=none
EOF

# 4. Polkit GUI Prompts
cat << "EOF" > /etc/pam.d/polkit-1
#%PAM-1.0
auth        [success=1 default=ignore] pam_exec.so quiet /usr/bin/omarchy-hw-laptop-closed
auth        sufficient  pam_fprintd.so
auth        required    pam_unix.so
account     required    pam_unix.so
password    required    pam_unix.so
session     required    pam_unix.so
EOF

echo "==> All PAM configs applied successfully!"
'
```

### How Each Authentication Target Works:
1. **Terminal Sudo**: Running `sudo <cmd>` prompts for fingerprint. Touching the sensor grants root privileges instantly; pressing Enter or typing a password works as a fallback.
2. **SDDM Login**: When booting or logging in, touching the sensor logs in immediately without typing a password.
3. **Lock Screen**: Waking from sleep or locking (`Super + Ctrl + L`) immediately unlocks upon touch.
4. **Clamshell Gating**: If the laptop lid is closed while connected to external displays, `omarchy-hw-laptop-closed` bypasses fingerprint waiting and directly prompts for keyboard password.

---

## Seamless Boot-to-Desktop Unlock (LUKS + Hyprland)

### Why Fingerprints Cannot Decrypt LUKS at Early Boot
Full Disk Encryption (LUKS) requires cryptographic passphrases to decrypt storage blocks before the Linux kernel mounts the filesystem. At that stage, userspace daemons (`fprintd`, D-Bus, USB subsystems) are not running, and biometric scanners cannot perform mathematical block decryption.

### The Solution: Keyfile Auto-Decrypt + Startup Session Lock
To obtain a Windows Hello / Linux Mint style experience (power on laptop -> prompt for fingerprint on desktop):

#### Part 1: LUKS Initramfs Auto-Unlock
1. **Generate a root-only random cryptographic keyfile**:
   ```bash
   sudo dd bs=512 count=1 if=/dev/urandom of=/crypto_keyfile.bin
   sudo chmod 000 /crypto_keyfile.bin
   ```

2. **Add the keyfile to your LUKS encrypted partition**:
   ```bash
   sudo cryptsetup luksAddKey /dev/nvme0n1p2 /crypto_keyfile.bin
   ```

3. **Embed the keyfile into the initramfs boot image**:
   ```bash
   echo 'FILES+=(/crypto_keyfile.bin)' | sudo tee /etc/mkinitcpio.conf.d/keyfile.conf
   ```

4. **Rebuild initramfs and update your bootloader (Limine / GRUB)**:
   ```bash
   sudo limine-update
   # OR for standard mkinitcpio:
   # sudo mkinitcpio -P
   ```

#### Part 2: Auto-Lock Session on Boot
Ensure the fingerprint lock screen engages immediately upon desktop startup:

1. In `~/.config/hypr/autostart.lua`:
   ```lua
   o.exec_on_start("bash -c 'while ! omarchy-shell shell ping >/dev/null 2>&1; do sleep 0.1; done; omarchy system lock'")
   ```

2. In `~/.config/omarchy/hooks/post-boot.d/10-lock-on-boot.hook`:
   ```bash
   #!/bin/bash
   while ! omarchy-shell shell ping >/dev/null 2>&1; do
     sleep 0.2
   done
   if [[ $(omarchy-shell lock isLocked 2>/dev/null) != "true" ]]; then
     omarchy system lock
   fi
   ```
   ```bash
   chmod +x ~/.config/omarchy/hooks/post-boot.d/10-lock-on-boot.hook
   ```

#### Rollback Instructions
To restore manual pre-boot LUKS password entry at any time:
```bash
sudo rm -f /crypto_keyfile.bin /etc/mkinitcpio.conf.d/keyfile.conf
sudo cryptsetup luksKillSlot /dev/nvme0n1p2 1
sudo limine-update
```

---

## Troubleshooting & Debug Logging

### 1. View Verbose Debug Output
Stop the system daemon and run `fprintd` manually with debug logging:
```bash
sudo systemctl stop fprintd
sudo G_MESSAGES_DEBUG=all /usr/libexec/fprintd -t
```
In another terminal, trigger verification:
```bash
fprintd-verify
```
Look for `elanpress` logs showing SIFT keypoint counts and match distance scores.

### 2. Device Busy / USB Protocol Desync
If the device enters a stalled state after unexpected sleep/resume:
```bash
# Unbind and rebind the USB device driver
echo "04f3:0903" | sudo tee /sys/bus/usb/drivers/usb/unbind
sleep 1
echo "04f3:0903" | sudo tee /sys/bus/usb/drivers/usb/bind
sudo systemctl restart fprintd
```

### 3. "No minutiae found" or Enrollment Aborted
* Ensure the sensor surface is clean and dry.
* Press flat against the center of the sensor rather than swiping.
* Re-run `fprintd-enroll` with firm, steady presses.

---

## License & Acknowledgements

* **License**: GNU Lesser General Public License v2.1 or later ([LGPL-2.1-or-later](COPYING)).
* **Upstream**: Based on the [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) project and includes NBIS software from NIST.
* **Inspiration**: Driver architecture references press-mode sensor adaptations from [filip-rs/libfprint](https://github.com/filip-rs/libfprint/tree/elanpress).
