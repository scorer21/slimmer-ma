#!/bin/bash
# setup-pi.sh - turns a fresh Raspberry Pi OS Lite into a slimmer player for Music Assistant
#
#   squeezelite on the USB sound card, LCDd for a 20x4 HD44780 on I2C,
#   rotary encoder and buttons through kernel overlays (no pikeyd),
#   slimmer as a systemd service, and as few SD card writes as possible.
#
# Usage on the Pi:
#   curl -fsSL https://raw.githubusercontent.com/scorer21/slimmer-ma/master/scripts/setup-pi.sh -o setup-pi.sh
#   sudo bash setup-pi.sh
#
# Everything can be preset through the environment, otherwise the script asks:
#   PLAYER_NAME, MA_HOST, MA_PORT, MA_TOKEN, LCD_ADDR, ENCODER_A, ENCODER_B,
#   KEY_ENTER_GPIO, KEY_SPACE_GPIO, KEY_BACK_GPIO, ALSA_PARAMS, START_VOLUME

set -euo pipefail

# apt-get waits up to 10 minutes if something else (e.g. apt-daily) holds the dpkg lock

[ "$(id -u)" = 0 ] || { echo "Bitte mit sudo starten: sudo bash $0"; exit 1; }

REPO=https://github.com/scorer21/slimmer-ma.git
LCDAPI_REPO=https://github.com/spdawson/lcdapi.git
SRC=/usr/local/src
CONFIG_TXT=/boot/firmware/config.txt
[ -f "$CONFIG_TXT" ] || CONFIG_TXT=/boot/config.txt

ask() { # ask VAR "question" default
	local var=$1 question=$2 default=${3:-}
	if [ -z "${!var:-}" ]; then
		read -r -p "$question${default:+ [$default]}: " value </dev/tty
		printf -v "$var" '%s' "${value:-$default}"
	fi
}

ask PLAYER_NAME "Name des Players" "Musikbox"
ask MA_HOST "IP oder Name vom Music-Assistant-Server" ""
ask MA_PORT "Port von Music Assistant" "8095"
if [ -z "${MA_TOKEN:-}" ]; then
	read -r -s -p "MA-Token (Profil -> Long-lived token, leer lassen = spaeter eintragen): " MA_TOKEN </dev/tty
	echo
fi
LCD_ADDR=${LCD_ADDR:-0x3F}
ENCODER_A=${ENCODER_A:-24}
ENCODER_B=${ENCODER_B:-25}
KEY_ENTER_GPIO=${KEY_ENTER_GPIO:-22}
KEY_SPACE_GPIO=${KEY_SPACE_GPIO:-27}
KEY_BACK_GPIO=${KEY_BACK_GPIO:-18}
ALSA_PARAMS=${ALSA_PARAMS:-80:4::1}
START_VOLUME=${START_VOLUME:-70}

# The player id in Music Assistant is the MAC address. Pin it to the wired one,
# so the player stays the same when it later runs over WLAN.
PLAYER_MAC=${PLAYER_MAC:-$(cat /sys/class/net/eth0/address 2>/dev/null || true)}

echo "==> Pakete (das dauert am Pi 1 eine Weile)"
apt-get -o DPkg::Lock::Timeout=600 update
DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=600 install -y --no-install-recommends \
	squeezelite lcdproc i2c-tools alsa-utils git ca-certificates curl \
	build-essential cmake autoconf automake libtool libcurl4-openssl-dev libicu-dev

echo "==> config.txt: I2C, Drehgeber, Tasten"
# Linux key codes: ENTER 28, SPACE 57, BACKSPACE 14 (slimmer expects exactly these)
sed -i '/# slimmer-ma begin/,/# slimmer-ma end/d' "$CONFIG_TXT"
cat >> "$CONFIG_TXT" <<EOF
# slimmer-ma begin
dtparam=i2c_arm=on
dtoverlay=rotary-encoder,pin_a=$ENCODER_A,pin_b=$ENCODER_B,relative_axis=1
dtoverlay=gpio-key,gpio=$KEY_ENTER_GPIO,keycode=28,label="ENTER"
dtoverlay=gpio-key,gpio=$KEY_SPACE_GPIO,keycode=57,label="SPACE"
dtoverlay=gpio-key,gpio=$KEY_BACK_GPIO,keycode=14,label="BACKSPACE"
# slimmer-ma end
EOF
grep -qx i2c-dev /etc/modules || echo i2c-dev >> /etc/modules

echo "==> squeezelite"
CARD=""
for dir in /proc/asound/card*; do
	[ -e "$dir/usbid" ] && CARD=$(cat "$dir/id") && break
done
if [ -z "$CARD" ]; then
	echo "    Keine USB-Soundkarte gefunden, squeezelite nimmt vorerst die Standardausgabe."
	OUTPUT=default
else
	echo "    USB-Soundkarte: $CARD"
	OUTPUT="sysdefault:CARD=$CARD"
fi
cat > /etc/default/squeezelite <<EOF
# written by setup-pi.sh
SL_NAME="$PLAYER_NAME"
SL_SOUNDCARD="$OUTPUT"
SB_EXTRA_ARGS="-a $ALSA_PARAMS${PLAYER_MAC:+ -m $PLAYER_MAC}${MA_HOST:+ -s $MA_HOST}"
EOF
systemctl enable squeezelite
systemctl restart squeezelite || true

echo "==> LCDd"
DRIVER_PATH=$(dirname "$(find /usr/lib -name hd44780.so 2>/dev/null | head -1)")/
cat > /etc/LCDd.conf <<EOF
# written by setup-pi.sh - HD44780 20x4 behind a PCF8574 I2C backpack
[server]
DriverPath=$DRIVER_PATH
Driver=hd44780
Bind=127.0.0.1
Port=13666
ReportLevel=2
ReportToSyslog=no
User=nobody
Foreground=no
Hello="                    "
Hello="  Music Assistant   "
Hello="$(printf '%-20.20s' "  $PLAYER_NAME")"
Hello="                    "
GoodBye="                    "
GoodBye="      Goodbye!      "
GoodBye="                    "
GoodBye="                    "
WaitTime=5
ServerScreen=blank

[menu]

[hd44780]
ConnectionType=i2c
Device=/dev/i2c-1
Port=$LCD_ADDR
i2c_line_RS=0x01
i2c_line_RW=0x02
i2c_line_EN=0x04
i2c_line_BL=0x08
i2c_line_D4=0x10
i2c_line_D5=0x20
i2c_line_D6=0x40
i2c_line_D7=0x80
Backlight=yes
BacklightInvert=yes
Size=20x4
CharMap=hd44780_default
DelayBus=true
Keypad=no
EOF
# Debian ships LCDd as lcdproc.service
systemctl enable lcdproc

echo "==> lcdapi"
mkdir -p "$SRC"
rm -rf "$SRC/lcdapi"
git clone --depth 1 "$LCDAPI_REPO" "$SRC/lcdapi"
(cd "$SRC/lcdapi" && autoreconf -fi && ./configure --prefix=/usr/local && make && make install)
ldconfig

echo "==> slimmer (am Pi 1 locker eine halbe Stunde)"
rm -rf "$SRC/slimmer-ma" "$SRC/slimmer-build"
git clone --depth 1 "$REPO" "$SRC/slimmer-ma"
cmake -S "$SRC/slimmer-ma" -B "$SRC/slimmer-build" -DCMAKE_BUILD_TYPE=MinSizeRel
make -C "$SRC/slimmer-build"
install -m 755 "$SRC/slimmer-build/slimmer" /usr/local/bin/slimmer
install -m 644 "$SRC/slimmer-ma/scripts/slimmer.service" /etc/systemd/system/slimmer.service

umask 077
cat > /etc/default/slimmer <<EOF
# written by setup-pi.sh
MA_TOKEN="$MA_TOKEN"
OPTIONS="--mahost $MA_HOST --maport $MA_PORT --input all --volume $START_VOLUME --encoding ISO-8859-2${PLAYER_MAC:+ --mac $PLAYER_MAC}"
EOF
umask 022
systemctl daemon-reload
systemctl enable slimmer

echo "==> Compiler wieder runter"
# keep the libraries slimmer links against
ldd /usr/local/bin/slimmer | awk '/=> \//{print $3}' | xargs -r readlink -f | xargs -r dpkg -S 2>/dev/null \
	| cut -d: -f1 | sort -u | xargs -r apt-mark manual >/dev/null || true
DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=600 purge -y build-essential cmake autoconf automake libtool \
	libcurl4-openssl-dev libicu-dev
DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=600 autoremove --purge -y
apt-get -o DPkg::Lock::Timeout=600 clean
rm -rf "$SRC/slimmer-build" "$SRC/lcdapi"

echo "==> SD-Karte schonen"
# journal only in RAM
mkdir -p /etc/systemd/journald.conf.d
cat > /etc/systemd/journald.conf.d/volatile.conf <<EOF
[Journal]
Storage=volatile
RuntimeMaxUse=16M
EOF
# /var/log and /tmp in RAM, no access times on the root file system
grep -q '^tmpfs /var/log ' /etc/fstab || echo 'tmpfs /var/log tmpfs defaults,noatime,nosuid,nodev,mode=0755,size=16m 0 0' >> /etc/fstab
grep -q '^tmpfs /tmp ' /etc/fstab || echo 'tmpfs /tmp tmpfs defaults,noatime,nosuid,nodev,size=32m 0 0' >> /etc/fstab
sed -i -E '/[[:space:]]\/[[:space:]]+ext4[[:space:]]/{/noatime/!s/(ext4[[:space:]]+)([^[:space:]]+)/\1\2,noatime/}' /etc/fstab
# no swap file on the card (zram stays, it lives in RAM)
if systemctl cat dphys-swapfile.service >/dev/null 2>&1; then
	dphys-swapfile swapoff || true
	systemctl disable dphys-swapfile
	dphys-swapfile uninstall || true
fi
# no daily package list downloads and man page indexing
systemctl disable --now apt-daily.timer apt-daily-upgrade.timer man-db.timer 2>/dev/null || true

echo "==> WLAN-Stick"
if lsusb 2>/dev/null | grep -i '0bda:8172' >/dev/null; then
	if modinfo r8712u >/dev/null 2>&1; then
		echo "    RTL8191S erkannt, Treiber r8712u ist vorhanden."
	else
		echo "    RTL8191S erkannt, aber dieser Kernel hat keinen Treiber dafuer mehr (r8712u)."
	fi
fi

echo
echo "Fertig. Jetzt einmal neu starten: sudo reboot"
[ -n "$MA_TOKEN" ] || echo "Der MA-Token fehlt noch, eintragen in /etc/default/slimmer (MA_TOKEN=\"...\")."
