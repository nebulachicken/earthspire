# Cape Canaveral Sentinel Viewer — prototype 0.2

The desktop app **downloads the data**; the calculator **turns it into the image**.
Nothing is screen-mirrored any more.

```
 PC: SentinelFetch.exe                       TI-Nspire CX II: capeviewer.prg.tns
 ─────────────────────                       ───────────────────────────────────
 STAC search (Earth Search)                  reads capecanaveral.svd.tns
 pick newest scene clear over the crop       verifies CRC32 checksums
 download raw B04/B03/B02 + SCL window  ───► reflectance → tone curve → RGB565
 write capecanaveral.svd.tns                 draws it (lcd_blit), + / - / C / I keys
          (file transfer: N-Link, TI software, ...)
```

## What changed from 0.1

| | 0.1 | 0.2 |
|---|---|---|
| PC output | finished RGB565 picture, shown full-screen | raw 16-bit reflectance bands + cloud mask in one file |
| Colour conversion | PC (GDAL pre-rendered `visual` asset) | **calculator** (integer tone mapping) |
| Transport | ScreenMirror (video of the desktop) | file transfer of the data file |
| Cloud check | whole-scene percentage | cloud/shadow share **over the 3.2 × 2.4 km crop**, from the SCL layer |
| Date window | hard-coded end date | rolling `--days` look-back |
| Dependencies | curl, GDAL, SDL2, nlohmann-json | curl, GDAL, nlohmann-json |
| GPL code involved | ScreenMirror (GPL-3.0) | none; Ndless is MPL-2.0 and not bundled |

The calculator also gets things the picture never had: brightness control, a cloud/shadow
overlay, and the scene date/ID, because it receives the underlying data.

## Data file (SVD1)

128-byte header (magic, geometry, reflectance offset/scale, cloud %, scene ID, date, CRC32s)
followed by three little-endian `uint16` planes (R, G, B surface reflectance DN, 320 × 240)
and one `uint8` plane (Scene Classification Layer). About 538 KB.
Reflectance = (DN + offset) / 10000. Spec: `common/svd_format.h`.

## Build the desktop app (Windows 10/11, VS 2022, CMake, vcpkg)

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release
```

Configure from the **repository root** (not `desktop/`), so vcpkg finds `vcpkg.json`.
The first configure builds GDAL, which takes a while.

```powershell
.\build\desktop\Release\SentinelFetch.exe --preview preview.bmp
```

Options: `--out FILE`, `--max-cloud PCT` (default 10, share of the *crop*), `--days N`
(default 120), `--allow-cloudy`, `--preview FILE.bmp`. It re-reads and checksum-verifies
the file it wrote. The preview BMP uses the same conversion code as the calculator.

## Build the calculator client

Needs the Ndless SDK on PATH (`nspire-gcc`, `nspire-ld`, `genzehn`, `make-prg`).

```sh
cd calculator
make          # -> capeviewer.prg.tns
```

Copy `capeviewer.prg.tns` to the calculator like any Ndless program. Ndless lists CX II OS
6.2.0.333 and 6.4.0.74 as supported; check Settings → Status → About for the full build number.

## Send the data to the calculator

Transfer `capecanaveral.svd.tns` into the calculator's **My Documents** root using any tool
that can upload files to a CX II, for example N-Link or TI-Nspire Computer Link software.
(The `.tns` suffix is required by the OS; the client opens the full name.) Then launch
`capeviewer` from the calculator. Refresh by re-running `SentinelFetch` and re-sending.

## Calculator keys

`+` / `-` brightness · `C` cloud/shadow overlay · `I` scene info · `Esc` quit

## Honest status

- The shared C code (container, checksums, tone mapping) has host tests: `tests/run.sh`.
- `desktop/main.cpp` has been syntax-checked only; it has not been linked against real
  GDAL/curl or run against the live STAC API. Asset names (`red`, `green`, `blue`, `scl`,
  `raster:bands`) are written from the Earth Search v1 schema as I understand it.
- `calculator/main.c` has not been compiled with the Ndless SDK or run on a calculator.
  Ndless API usage (`lcd_init`, `lcd_blit`, `show_msgbox`, key names, `genzehn` flags)
  follows the SDK conventions from memory and may need small fixes.
- Not implemented: a live USB pull (calculator requesting data from the PC). That needs a
  USB service on both ends; see docs/live-usb.md.
- A Sentinel-2 10 m pixel is not building-level imagery.
