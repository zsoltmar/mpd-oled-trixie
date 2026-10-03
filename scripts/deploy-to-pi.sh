#!/bin/bash
# Sync this tree to the moOde Pi and build/install mpd_oled.
#   MPD_OLED_HOST   default pi@moode.local
#   MPD_OLED_KEY    default ~/.ssh/id_pi

set -euo pipefail

HOST="${MPD_OLED_HOST:-pi@moode.local}"
KEY="${MPD_OLED_KEY:-$HOME/.ssh/id_pi}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [[ ! -f "$KEY" ]]; then
  echo "SSH key not found: $KEY" >&2
  exit 1
fi

SSH=(ssh -i "$KEY" -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new)
RSYNC_RSH="ssh -i ${KEY} -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new"

echo "Deploy ${ROOT} -> ${HOST}"
if ! "${SSH[@]}" "$HOST" 'echo ok' >/dev/null; then
  echo "SSH to ${HOST} failed." >&2
  echo "Authorize this key (password prompt):" >&2
  echo "  ssh-copy-id -i ${KEY}.pub ${HOST}" >&2
  exit 1
fi

REMOTE_HOME="$("${SSH[@]}" "$HOST" 'printf %s "$HOME"')"
REMOTE_DIR="${REMOTE_HOME}/mpd-oled-trixie"

"${SSH[@]}" "$HOST" "mkdir -p ${REMOTE_DIR}"
rsync -az --delete \
  --exclude .git \
  --exclude autom4te.cache \
  --exclude '*.o' --exclude '*.lo' --exclude '*.la' \
  -e "$RSYNC_RSH" \
  "${ROOT}/" "${HOST}:${REMOTE_DIR}/"

"${SSH[@]}" "$HOST" "REMOTE_DIR=$(printf '%q' "$REMOTE_DIR") bash -s" <<'EOF'
set -euo pipefail
sudo apt-get update
sudo apt-get install -y autoconf automake libtool pkg-config make g++ git \
  libfftw3-dev libiniparser-dev libmpdclient-dev libi2c-dev i2c-tools \
  lm-sensors libasound2-dev

if ! command -v mpd_oled_cava >/dev/null 2>&1; then
  cava_src="${HOME}/src/cava-1.0.0"
  mkdir -p "${HOME}/src"
  if [[ ! -f ${cava_src}/configure.ac ]]; then
    rm -rf "${cava_src}"
    git clone --branch 1.0.0 --depth 1 https://github.com/karlstav/cava "${cava_src}"
  fi
  cd "${cava_src}"
  # Cava's automake -Werror breaks on Trixie's autoconf.
  sed -i 's/ -Werror / /' configure.ac
  ./autogen.sh
  ./configure --disable-input-portaudio --disable-input-sndio \
    --disable-output-ncurses --disable-input-pulse \
    --disable-input-pipewire --disable-input-jack --disable-output-sdl \
    --program-prefix=mpd_oled_
  make -j"$(nproc)"
  sudo make install-strip
fi

cd "${REMOTE_DIR}"
./bootstrap
CPPFLAGS="-W -Wall -Wno-psabi" ./configure --prefix=/usr/local
make -j"$(nproc)"
sudo make install-strip
sudo mpd_oled_service_install
if systemctl is-active --quiet mpd_oled; then
  sudo systemctl restart mpd_oled
fi
echo "Installed $(command -v mpd_oled)"
EOF
