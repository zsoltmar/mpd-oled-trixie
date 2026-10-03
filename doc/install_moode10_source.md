# Install on moOde 10 (Raspberry Pi OS Trixie)

moOde 10 is Debian 13 (Trixie). Upstream binary packages stop at moOde 8,
so build from this tree.

Log in over SSH as `pi` (enable **System / Local Services / SSH term server**
in the moOde UI). From a checkout on another machine:

```
./scripts/deploy-to-pi.sh
```

That syncs this tree to `pi@moode.local`, builds `mpd_oled_cava` if it is
missing, builds `mpd_oled`, and installs both under `/usr/local`.
Override the target with `MPD_OLED_HOST=user@host`.

## Build on the Pi

```
sudo apt update
sudo apt install autoconf automake libtool pkg-config make g++ git \
  libfftw3-dev libiniparser-dev libmpdclient-dev libi2c-dev i2c-tools \
  lm-sensors libasound2-dev

git clone --branch 1.0.0 --depth 1 https://github.com/karlstav/cava
cd cava
./autogen.sh
./configure --disable-input-portaudio --disable-input-sndio \
  --disable-output-ncurses --disable-input-pulse \
  --disable-input-pipewire --disable-input-jack --disable-output-sdl \
  --program-prefix=mpd_oled_
make
sudo make install-strip

cd ..
git clone https://github.com/antiprism/mpd_oled
# or copy this fork into place
cd mpd_oled
./bootstrap
CPPFLAGS="-W -Wall -Wno-psabi" ./configure --prefix=/usr/local
make
sudo make install-strip
sudo mpd_oled_service_install
```

## I2C

Add `i2c-dev` to `/etc/modules`.

On Trixie the firmware config is `/boot/firmware/config.txt`
(often linked from `/boot/config.txt`):

```
dtparam=i2c_arm=on
dtparam=i2c_arm_baudrate=800000
```

Reboot, then `sudo i2cdetect -y 1`. Pass the address with `-a`
(for example `-a 3c`).

`i2c_arm_baudrate=800000` is required for a high frame rate. The default
100 kHz bus cannot refresh a 128x64 panel at 60 fps. See
[Frame rate](#frame-rate).

OLED type (`-o`):

    1  Adafruit SPI 128x64
    3  Adafruit I2C 128x64
    4  Seeed I2C 128x64
    6  SH1106 I2C 128x64
    7  SH1106 SPI 128x64

SPI on a Pi 5 is not supported by the bundled bcm2835 library.

## Audio copy and metadata

In the moOde UI:

* set the timezone
* turn on **Audio / MPD options / Metadata file**
  (`/var/local/www/currentsong.txt`)
* turn on **Audio / ALSA / Loopback**

Start the display (SH1106 I2C example):

```
sudo mpd_oled_service_edit -o 6 -a 3c -b 21 -g 1 -f 60 -c alsa,plughw:Loopback,1
```

Playing screen is a full-width spectrum, then title, artist, elapsed time,
bitrate, volume, and a progress bar. Stopped screen is weekday, clock, date,
and IP. Change either in `draw_clock` / `draw_spect_display` in `src/main.cpp`.

## Frame rate

`-f` sets both the spectrum rate and the display rate. On a Pi 4 with an
I2C 128x64 panel (SH1106 or SSD1306):

| I2C baud | Useful `-f` | Full-frame time | Bus ceiling |
| --- | --- | --- | --- |
| 100 kHz (kernel default) | 15–25 | about 100 ms | about 10 fps |
| 800 kHz | 60 | 12.5 ms | about 80 fps |

Set 800 kHz, reboot, then use `-f 60`. That measured ceiling is for this
tree: each 128-byte page is one I2C write, with no sleep between writes.
Stay under the ceiling. A higher `-f` than the bus can finish queues
frames and looks uneven. The Pi 4 CPU is not the limit.

Wiring and some panels get unstable above 800 kHz. If the image breaks
up, drop the baud to 400000 and use `-f 30`.
