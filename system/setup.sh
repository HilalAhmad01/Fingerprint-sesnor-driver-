#!/usr/bin/env bash
# ==============================================================================
# ELAN 04f3:0903 fingerprint setup for Omarchy (Arch + Hyprland + SDDM)
#
#   sudo ./system/setup.sh [--fingerprint-sudo]
#
# 1. Builds this libfprint fork as a pacman package (libfprint-elanpress) and
#    installs it in place of libfprint / libfprint-git. Undoes the old
#    approach (ninja install + NoExtract/IgnorePkg in pacman.conf).
# 2. Installs the PAM policy (system/fprint-pam-apply) and keeps it applied
#    across Omarchy updates (fprint-pam-guard.path):
#      lock screen: fingerprint, 5 rejected presses -> password required
#      sudo/polkit: password only (fingerprint + lockout with
#                   --fingerprint-sudo)
#      SDDM:        password only
# 3. Boot: SDDM autologin stays, but the session locks itself straight away
#    and is terminated if the lock screen does not come up (fail closed).
#
# Re-run it after changing the driver to rebuild and reinstall.
# ==============================================================================
set -euo pipefail

REPO_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
LIB=/usr/local/lib/fprint-elanpress
FINGERPRINT_SUDO=0

for arg in "$@"; do
  case $arg in
    --fingerprint-sudo) FINGERPRINT_SUDO=1 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

step() { echo -e "\e[34m===> $*\e[0m"; }
warn() { echo -e "\e[33m[!] $*\e[0m"; }

if [[ $EUID -ne 0 ]]; then
  echo -e "\e[31m[!] Run with sudo: sudo $0 $*\e[0m"
  exit 1
fi
CURRENT_USER=${SUDO_USER:-}
if [[ -z $CURRENT_USER || $CURRENT_USER == root ]]; then
  echo "Run this through sudo from your user account (makepkg refuses to run as root)." >&2
  exit 1
fi
USER_HOME=$(getent passwd "$CURRENT_USER" | cut -d: -f6)

step "Step 1: build dependencies"
pacman -S --needed --noconfirm base-devel meson ninja gobject-introspection

step "Step 2: building the libfprint-elanpress package"
# the old script ran the build as root at times; make sure the user can write
chown -R "$CURRENT_USER:" "$REPO_DIR/packaging/arch"
sudo -u "$CURRENT_USER" bash -c "cd '$REPO_DIR/packaging/arch' && makepkg -f --cleanbuild"
PKG=$(ls -t "$REPO_DIR"/packaging/arch/libfprint-elanpress-*.pkg.tar.zst | head -1)
echo "built $PKG"

step "Step 3: undoing the pacman.conf hold on libfprint"
# NoExtract would make pacman skip the very library this package installs
if grep -qxE 'NoExtract = usr/lib/libfprint\*|IgnorePkg = libfprint libfprint-git' /etc/pacman.conf; then
  cp -a /etc/pacman.conf "/etc/pacman.conf.fprint-elanpress.bak"
  sed -i -e '/^NoExtract = usr\/lib\/libfprint\*$/d' \
         -e '/^IgnorePkg = libfprint libfprint-git$/d' /etc/pacman.conf
  echo "removed NoExtract/IgnorePkg lines (backup: /etc/pacman.conf.fprint-elanpress.bak)"
fi

step "Step 4: installing the package"
# --ask 4 accepts replacing the conflicting libfprint/libfprint-git in one
# transaction (as omarchy-setup-security-fingerprint does)
pacman -U --noconfirm --ask 4 "$PKG"
# the old `ninja install` left the test suite in /usr; remove it if no
# package owns it
for d in /usr/libexec/installed-tests/libfprint-2 /usr/share/installed-tests/libfprint-2; do
  if [[ -d $d ]] && ! pacman -Qoq "$d" >/dev/null 2>&1; then
    rm -rf -- "$d"
    echo "removed leftover $d"
  fi
done
rmdir --ignore-fail-on-non-empty /usr/libexec/installed-tests /usr/share/installed-tests 2>/dev/null || true

step "Step 5: PAM policy"
install -d -m 0755 "$LIB"
for f in fprint-pam-apply fprint-locked-out fprint-faillock-reset fprint-boot-lock; do
  install -m 0755 -o root -g root "$REPO_DIR/system/$f" "$LIB/$f"
done
install -m 0644 "$REPO_DIR/system/fprint-pam-guard.path" /etc/systemd/system/
install -m 0644 "$REPO_DIR/system/fprint-pam-guard.service" /etc/systemd/system/
cat > /etc/fprint-elanpress.conf <<EOF
# Read by $LIB/fprint-pam-apply. Re-run it after editing:
#   sudo $LIB/fprint-pam-apply
# 1: fingerprint (with lockout) for sudo and polkit; 0: password only
FINGERPRINT_SUDO=$FINGERPRINT_SUDO
# user whose lock screen tally directory is set up
LOCK_USER=$CURRENT_USER
EOF
chmod 0644 /etc/fprint-elanpress.conf
for f in sudo polkit-1 sddm omarchy-lock-fingerprint omarchy-lock-password; do
  [[ -f /etc/pam.d/$f && ! -f /etc/pam.d/$f.fprint-elanpress.bak ]] &&
    cp -a "/etc/pam.d/$f" "/etc/pam.d/$f.fprint-elanpress.bak"
done
"$LIB/fprint-pam-apply"
systemctl daemon-reload
systemctl enable --now fprint-pam-guard.path
systemctl enable fprint-pam-guard.service

step "Step 6: boot to fingerprint lock (fail closed)"
AUTOSTART="$USER_HOME/.config/hypr/autostart.lua"
LOCK_CMD="o.exec_on_start(\"$LIB/fprint-boot-lock\")"
if [[ -f $AUTOSTART ]]; then
  if ! grep -qF "$LIB/fprint-boot-lock" "$AUTOSTART"; then
    cp -a "$AUTOSTART" "$AUTOSTART.fprint-elanpress.bak"
    # replace the old inline "wait for shell, then lock" line if present
    sed -i -e '/omarchy-shell shell ping.*omarchy system lock/d' \
           -e '/^-- Lock screen on boot so fingerprint prompt is immediately active$/d' "$AUTOSTART"
    printf '\n-- Lock autologin sessions at once; end the session if the lock fails\n%s\n' \
      "$LOCK_CMD" >> "$AUTOSTART"
    chown "$CURRENT_USER:" "$AUTOSTART"
    echo "updated $AUTOSTART (backup: $AUTOSTART.fprint-elanpress.bak)"
  fi
else
  warn "$AUTOSTART not found; add this line to your Hyprland autostart:"
  echo "    $LOCK_CMD"
fi
# the previous setup also locked from a post-boot hook; one locker is enough
OLD_HOOK="$USER_HOME/.config/omarchy/hooks/post-boot.d/10-lock-on-boot.hook"
if [[ -f $OLD_HOOK ]] && grep -q 'omarchy system lock' "$OLD_HOOK"; then
  mv "$OLD_HOOK" "$OLD_HOOK.disabled"
  echo "disabled $OLD_HOOK"
fi

step "Step 7: restarting fprintd"
systemctl stop fprintd 2>/dev/null || true   # D-Bus starts it again on demand

echo -e "\e[32m=================================================================\e[0m"
echo -e "\e[32mDone.\e[0m"
if sudo -u "$CURRENT_USER" fprintd-list "$CURRENT_USER" 2>/dev/null | grep -qi finger; then
  cat <<EOF

Existing enrollments still work, but they were saved by the old driver, which
stored the fingerprint *images* in /var/lib/fprint. Re-enroll so they are
replaced with keypoints only:

  fprintd-delete $CURRENT_USER
  fprintd-enroll -f right-index-finger

EOF
else
  echo -e "\nEnroll a finger: fprintd-enroll -f right-index-finger"
fi
echo "Press and lift 12 times, moving the finger a little between presses so"
echo "different parts of it are covered."
