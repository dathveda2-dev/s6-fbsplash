#!/usr/bin/env bash
set -euo pipefail

BIN="${1:-./s6-fbsplash}"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

# 1. Help and version
"${BIN}" --help 2>&1 | grep -q "s6/init"
"${BIN}" --version | grep -q "s6-fbsplash"
echo "✓ CLI --help and --version"

# 2. Execvp fallback preserves child exit code when assets/fb are missing
set +e
"${BIN}" --fb /nonexistent/fb0 --bg /nonexistent/bg.ff --font /nonexistent/font.psfu -- /bin/sh -c 'exit 42'
rc=$?
set -e
if (( rc != 42 )); then
    echo "FAIL: expected fallback exit 42, got ${rc}" >&2
    exit 1
fi
echo "✓ Missing-asset fallback preserves exit code (42)"

# 3. Built-in Farbfeld converter (--convert with cover-crop & --blur)
python3 - "${TMP}/in.ppm" <<'PY'
import sys
w, h = 160, 160
with open(sys.argv[1], "wb") as f:
    f.write(f"P6\n{w} {h}\n255\n".encode())
    f.write(b"\x80\x20\x30" * (w * h))
PY

"${BIN}" --convert "${TMP}/in.ppm" "${TMP}/converted.ff" --bg-size 128x72 --blur 4
python3 - "${TMP}/converted.ff" <<'PY'
import struct, sys
with open(sys.argv[1], "rb") as f:
    data = f.read()
assert data[:8] == b"farbfeld", f"bad magic: {data[:8]!r}"
w, h = struct.unpack(">II", data[8:16])
assert (w, h) == (128, 72), f"unexpected size: {(w, h)}"
assert len(data) == 16 + 128 * 72 * 8, f"unexpected length: {len(data)}"
PY
echo "✓ Built-in --convert (PPM -> 128x72 blurred Farbfeld) passed"

# 4. Headless Farbfeld + PSF2 + ANSI SGR render test (--render-ppm)
FONT_PATH=""
for candidate in /usr/share/consolefonts/*.psfu.gz /usr/share/kbd/consolefonts/*.psfu.gz; do
    if [[ -f "${candidate}" ]]; then
        zcat "${candidate}" > "${TMP}/test.psfu"
        FONT_PATH="${TMP}/test.psfu"
        break
    fi
done
if [[ -z "${FONT_PATH}" ]]; then
    for candidate in /usr/share/consolefonts/*.psfu /usr/share/kbd/consolefonts/*.psfu; do
        if [[ -f "${candidate}" ]]; then
            FONT_PATH="${candidate}"
            break
        fi
    done
fi
if [[ -z "${FONT_PATH}" ]]; then
    echo "FAIL: no .psfu(.gz) font found for render test" >&2
    exit 1
fi

# Generate a synthetic 64x64 Farbfeld image via python3 (fallback to raw bgra if no python3)
if command -v python3 >/dev/null 2>&1; then
    python3 - "${TMP}/bg.ff" <<'PY'
import struct, sys
w, h = 64, 64
with open(sys.argv[1], "wb") as f:
    f.write(b"farbfeld" + struct.pack(">II", w, h))
    px = struct.pack(">HHHH", 0x2000, 0x3000, 0x5000, 0xFFFF)
    f.write(px * (w * h))
PY
    BG_ARG="${TMP}/bg.ff"
    BG_EXTRA=""
else
    dd if=/dev/zero of="${TMP}/bg.bgra" bs=16384 count=1 status=none
    BG_ARG="${TMP}/bg.bgra"
    BG_EXTRA="--bg-size 64x64"
fi

# shellcheck disable=SC2086
"${BIN}" --bg "${BG_ARG}" ${BG_EXTRA} --font "${FONT_PATH}" \
    --badge "s6" --title "Test Boot" --accent e60000 \
    --render-ppm "${TMP}/out.ppm" --render-size 640x360 \
    -- /bin/sh -c 'printf "\033[32m[ok]\033[0m s6-rc started\n"'

head -n 2 "${TMP}/out.ppm" | tr '\n' ' ' | grep -q "P6 640 360"
sz="$(wc -c < "${TMP}/out.ppm")"
if (( sz < 640 * 360 * 3 )); then
    echo "FAIL: PPM output too small (${sz} bytes)" >&2
    exit 1
fi
echo "✓ Headless Farbfeld + PSF2 + ANSI SGR render passed (${sz} bytes)"
