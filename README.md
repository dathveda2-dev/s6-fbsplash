# `s6-fbsplash`

A zero-dependency C99 Linux framebuffer (`/dev/fb0`) bootsplash and PTY console wrapper for `s6` (and any other minimal init like `runit`, `dinit`, `sinit`, or `openrc`).

Instead of pulling in Plymouth, DRM/libdrm, `libpng`, or `systemd`, `s6-fbsplash` links only against `libc` and `libutil`, opens `/dev/fb0`, switches `/dev/tty1` to `KD_GRAPHICS`, runs your init script inside a PTY, composites a translucent dark console panel over a **Farbfeld (`.ff`)** or raw `.bgra` background with PSF2 bitmap fonts + ANSI SGR color + UTF-8 support, and restores `KD_TEXT` on exit.

If `/dev/fb0`, the background, or the font is missing, it immediately `execvp`s the wrapped command—so it can **never** brick your boot.

## Features

- **Zero External Dependencies:** Pure C99 (`libc` + `libutil` for `openpty`).
- **Any Resolution via Farbfeld (`.ff`) or `.bgra`:** Reads [Farbfeld](https://tools.suckless.org/farbfeld/) headers dynamically and scales to your framebuffer (`FBIOGET_VSCREENINFO`), or reads raw `.bgra` (`--bg-size WxH`).
- **Double-Buffered Rendering:** Pre-computes the darkened translucent panel (`bg_panel`) once and composites into `backbuf` before blitting to `/dev/fb0`—zero flicker.
- **ANSI & UTF-8 Terminal Grid:** Supports `PSF2` unicode tables (`.psfu`), UTF-8 decoding, 16-color ANSI SGR (`\x1b[...m`), `\x1b[2J`, `\x1b[H`, `\x1b[K`, and scrolling.
- **Interactive TTY Input Passthrough:** Forwards keyboard input from `--tty` to the child PTY so LUKS passphrases and rescue prompts work (disable with `--no-input`).
- **Headless Preview Mode (`--render-ppm`):** Render a real frame to a `.ppm` image without `/dev/fb0` to test themes on your desktop.

## Building & Installing

```bash
make
sudo make install   # installs to /usr/local/bin/s6-fbsplash
```

## Preparing Assets

Convert any wallpaper to Farbfeld (`.ff`) using ImageMagick (`magick` / `convert`) or `ffmpeg`:

```bash
# ImageMagick
magick wallpaper.jpg -resize 1920x1200^ -gravity center -extent 1920x1200 /etc/s6/background.ff

# Or copy an uncompressed PSF2 console font
zcat /usr/share/consolefonts/LatGrkCyr-12x22.psfu.gz > /etc/s6/font.psfu
```

Preview your theme without rebooting:

```bash
./s6-fbsplash \
  --bg /etc/s6/background.ff \
  --font /etc/s6/font.psfu \
  --badge "s6" \
  --title "Gentoo Linux // s6-svscan" \
  --accent e60000 \
  --render-ppm preview.ppm \
  -- /bin/sh -c 'printf "\033[32m[ok]\033[0m starting s6-rc...\n"'
```

## Using with `s6` (`rc.init`)

Move your real `/etc/s6/current/scripts/rc.init` to `rc.init.real` and replace `rc.init` with:

```sh
#!/bin/execlineb -S0
/usr/local/bin/s6-fbsplash
  --bg /etc/s6/background.ff
  --font /etc/s6/font.psfu
  --title "Gentoo Linux // s6-svscan"
  --accent e60000
  -- /etc/s6/current/scripts/rc.init.real $@
```

## Tests

```bash
make test
```
