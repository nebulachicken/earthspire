# Cape Canaveral Sentinel Viewer — prototype 0.4

The desktop app **downloads and exports the image pyramid**; the calculator **displays it**.
Nothing is screen-mirrored any more.

```
 PC: SentinelFetch.exe                       TI-Nspire CX II: capeviewer.prg.tns
 ─────────────────────                       ───────────────────────────────────
 STAC search (Earth Search)                  reads capecanaveral.svd.tns
 pick newest scene clear over the crop       verifies CRC32 checksums
 download 5 extents of B04/B03/B02 + SCL ─► decompresses selected RGB565 level
 grade, pack and compress 10..160 m/px      draws it; × / ÷ zoom; + / - brightness
          (file transfer: N-Link, TI software, ...)
```

## New in 0.4: compressed multi-resolution exports

Run `SentinelFetch` with no arguments and it opens a page in your browser (served from your own
machine, 127.0.0.1 only) where you can:

1. **Pick the location** on a map (OpenStreetMap or satellite view), search for a place by name,
   or type coordinates. The orange box is the 3.2 x 2.4 km area the calculator will show.
2. **Choose the scene**: it lists recent scenes with the cloud share over *your* area and
   auto-selects the newest clear one; click any other date to load it instead.
3. **Adjust the colours** with live preview: exposure, midtones, shadow lift, highlight recovery,
   contrast, saturation, temperature, and the reflectance offset (black level).
4. **Save** the file for the calculator.

The preview uses the same grading and RGB565 conversion as the calculator. The saved file now
contains five pre-rendered levels, with the same 320 × 240 display size at every level. Each step
zooms out by 2× in each dimension: the geographic footprint doubles in width and height while the
metres-per-pixel value doubles. The calculator can switch levels immediately without resampling.
Colour settings are baked into each level. Tick "Save without color adjustments" (or use `--raw`)
to bypass the desktop grading controls and use the calculator's default reflectance-to-display
curve. Exported files store display-ready RGB565 pixels rather than retaining the source spectral
bands; this is lossless relative to the calculator's 16-bit RGB565 display, but not a raw-band archive.

If large parts of the picture are solid black, look at the "clipped to black" figure under the
preview. A high value means the offset subtraction is pushing dark ground to zero; move
"Black level / offset" toward 0 and watch the preview.

Command-line mode still works: any of `--lat --lon --out --max-cloud --days --allow-cloudy
--preview --raw` runs without the browser. `--ui`, `--port N`, `--no-browser` control the panel.
Note the command-line default applies the default colour grading; add `--raw` to bypass the desktop grading controls.

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

The calculator also gets brightness control, a cloud/shadow overlay, scene date/ID and instant
switching between the five pre-rendered geographic scales.

## Data file (SVD2)

New exports use SVD2: a 256-byte header followed by five compressed image levels at 10, 20, 40,
80 and 160 m/pixel. Each level remains 320 × 240, covering respectively 3.2 × 2.4 km,
6.4 × 4.8 km, 12.8 × 9.6 km, 25.6 × 19.2 km and 51.2 × 38.4 km. The payload stores RGB565
pixels plus a 2-bit-per-pixel class mask (normal, no-data, cloud shadow or cloud/cirrus). A small
LZSS codec with a lossless horizontal XOR predictor is used when it makes a level smaller; otherwise
the level is stored uncompressed. Per-level CRC32 checksums are verified before display. This
reduces storage substantially compared with the old three-`uint16`-band + SCL representation, and
the five wider views share one exported file. The calculator still accepts legacy SVD1 files.
Specification and codec: `common/svd_format.h` and `common/svd_format.c`.

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
(default 120), `--allow-cloudy`, `--preview FILE.bmp`, `--lat/--lon`, `--raw`. It re-reads and
checksum-verifies the file it wrote. The preview BMP uses the same conversion code as the calculator.

**MSYS2 MINGW64 instead of vcpkg:** `pacman -S --needed mingw-w64-x86_64-toolchain
mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-curl mingw-w64-x86_64-gdal
mingw-w64-x86_64-nlohmann-json`, then in the MINGW64 shell from the repository root:
`cmake -S . -B build -G Ninja && cmake --build build && ./build/desktop/SentinelFetch.exe`.
Run it from that shell (or put `C:\msys64\mingw64\bin` on your PATH) so the DLLs are found.

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

- `×` zoom in (move to the next finer level)
- `÷` zoom out (move to the next wider-area level)
- `+` / `−` brightness
- `C` cloud/shadow overlay
- `I` scene and current zoom information
- `Esc` quit

## Honest status

- The shared C code (legacy SVD1, SVD2 headers, LZSS compression/decompression, checksums and
  RGB565 rendering) has host tests: `tests/run.sh`. The desktop grading/export tests also create,
  reopen and verify a five-level SVD2 file.
- The colour grading, local web server and control-panel logic (`desktop/grade.cpp`, `httpd.cpp`,
  `app.cpp`) were built and tested on Linux with a fake scene source: unit tests
  (`tests/grade_test.cpp`), an end-to-end run of the real server checking every endpoint, and the
  page's own script run under Node against that server. A saved file was verified to render
  identically to the preview.
- `desktop/source_stac.cpp` (the curl/GDAL/STAC part, adapted from 0.2) has still never been
  compiled against the real libraries or run against the live STAC API. Asset names (`red`,
  `green`, `blue`, `scl`, `raster:bands`) are written from the Earth Search v1 schema as I
  understand it.
- The map, satellite layer and place search use Leaflet, OpenStreetMap/Esri tiles and Nominatim
  loaded from the internet; they have not been exercised in a real browser. If the map cannot
  load, typing coordinates still works.
- `calculator/main.c` has been syntax-checked against lightweight Ndless API stubs, but has not
  been built with the actual Ndless SDK or run on a physical calculator. File I/O, display switching
  and memory use therefore still need an on-device test.
- Not implemented: a live USB pull (calculator requesting data from the PC). That needs a
  USB service on both ends; see docs/live-usb.md.
- A Sentinel-2 10 m pixel is not building-level imagery.
