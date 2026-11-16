# Sentinel Viewer

**Version 0.4.2** · Desktop Sentinel-2 downloader/exporter + TI-Nspire CX II viewer

Sentinel Viewer downloads Sentinel-2 Level-2A imagery for a selected location, packages it into a `.svd.tns` data file, and displays it on a TI-Nspire CX II running Ndless.

The desktop program does the online search, scene screening, image download, colour processing, and file export. The calculator program scans the folder containing the calculator program for supported `.svd` data files and lets you choose which image to open. The calculator does not need a live connection to the desktop or to the internet after a data file has been transferred.

## What the project does

### Desktop application

Run `SentinelFetch` without arguments to start the local browser control panel. The panel lets you:

1. **Choose a location.** Search for a place by name, enter latitude/longitude, click the map, or drag the marker. The orange rectangle shows the base 3.2 × 2.4 km area.
2. **Find Sentinel-2 scenes.** Search recent Sentinel-2 Level-2A catalogue entries and inspect cloud/shadow and no-data percentages over the selected area.
3. **Select a scene.** The panel recommends the newest scene that meets the selected cloud threshold and has no more than 2% no-data. When none meets the cloud threshold, it recommends the least-cloudy scene with acceptable data coverage. You can select another listed scene yourself.
4. **Adjust the preview.** Change exposure, midtones, shadow lift, highlight recovery, contrast, saturation, temperature, and black level/offset. The panel reports how many pixels are clipped to black or approach white.
5. **Export the calculator file.** Save an SVD2 image pyramid containing all five geographic levels.

The control panel is served by the desktop program on `127.0.0.1` (localhost). It is not intended to be exposed to a network. Leave the console window open while using the browser panel; use the panel's **Quit** button to stop the server.

### Five geographic levels in each export

Every SVD2 level is a 320 × 240 image. Moving to the next wider level doubles both the ground pixel size and the width/height of the geographic area. All levels remain centred on the selected location.

| Calculator level | Ground sampling | Geographic coverage |
|---:|---:|---:|
| 1 | 10 m/pixel | 3.2 × 2.4 km |
| 2 | 20 m/pixel | 6.4 × 4.8 km |
| 3 | 40 m/pixel | 12.8 × 9.6 km |
| 4 | 80 m/pixel | 25.6 × 19.2 km |
| 5 | 160 m/pixel | 51.2 × 38.4 km |

These are **separate geographic crops**, not enlarged versions of the first image. The downloader reads each progressively larger extent from the Sentinel-2 source imagery and resamples it to 320 × 240. The wider levels show more of the surrounding area at coarser spatial detail; they do not create extra source-image detail.

### Current web-preview limitation

The browser preview currently shows **only level 1 (10 m/pixel, 3.2 × 2.4 km)**. It uses the same grading/rendering path as the calculator for that base level, but the web interface does not yet have a selector for levels 2–5. The `--preview` command-line option also writes only the base 10 m/pixel image. The five levels are included in the exported SVD2 file and can be switched on the calculator.

### Calculator controls

| Key | Action |
|---|---|
| `×` | Zoom in to the next finer stored level |
| `÷` | Zoom out to the next wider-area level |
| `+` | Increase brightness |
| `−` | Decrease brightness |
| `C` | Toggle cloud and cloud-shadow overlay |
| `I` | Show scene date/ID and current resolution |
| `Esc` | Exit the viewer |

Zoom switching is available for SVD2 files. Legacy SVD1 files can still be opened, but they contain only one image and do not have the multi-level zoom pyramid.

## Satellite data and scene selection

The desktop source implementation queries the **Element 84 Earth Search STAC API** for the Sentinel-2 Level-2A collection and reads its `red`, `green`, `blue`, and `scl` assets. GDAL accesses the cloud-optimised GeoTIFF assets remotely using HTTP range requests where supported.

Cloud and no-data percentages are calculated from the Sentinel Scene Classification Layer (SCL) over the **base 3.2 × 2.4 km area**, rather than relying only on the scene-wide cloud percentage. The current classification counts SCL classes 3, 8, 9, and 10 as cloud/shadow/cirrus, and class 0 as no-data. The automatic recommendation excludes scenes with more than 2% no-data.

The browser panel screens up to 12 candidate scenes per search. The command-line fetcher screens up to 40. The command-line default is to require the requested maximum cloud percentage; use `--allow-cloudy` to permit the least-cloudy usable scene when no scene meets that threshold.

Imagery is retrieved from an external service. A working internet connection is needed for catalogue search and image downloads. The browser map and place search also use external services (Leaflet from a CDN, OpenStreetMap tiles, Esri World Imagery, and the Nominatim geocoder). If the map or place search is unavailable, manually entering coordinates still works.

## SVD data files

### SVD2 (current export format)

New exports use **SVD2**, a compact, multi-resolution display format:

- A 256-byte header stores image dimensions, scene metadata, level count, and the per-level metadata table.
- Each level contains 320 × 240 RGB565 display pixels and a packed 2-bit-per-pixel classification mask.
- Mask values are: `0` normal, `1` no-data, `2` cloud shadow, and `3` cloud/cirrus.
- Each level is stored as raw data, LZSS-compressed data, or LZSS data with a horizontal XOR predictor. The exporter chooses a compressed representation only when it is smaller than the raw payload.
- CRC32 checksums protect the header and each decoded level. The calculator verifies a level before displaying it.

Compression is lossless relative to the RGB565 pixels and classification mask being stored. Converting the original Sentinel-2 reflectance values to RGB565 is a display-oriented conversion, not a lossless archive of the original spectral bands. The five levels share one file, and each level is decoded when selected rather than keeping every decoded level in memory at once.

Desktop colour adjustments are baked into the exported RGB565 images. The calculator can still adjust overall brightness and toggle the saved cloud/shadow mask, but it cannot independently change the desktop grading sliders after export.

### SVD1 (legacy input)

The calculator retains SVD1 compatibility for older files. SVD1 stores the original 16-bit red, green, and blue reflectance planes, plus an optional SCL mask. It is not a multi-resolution format. New desktop exports are SVD2.

## Quick start: use the browser control panel

1. Build or obtain the desktop executable as described below.
2. Run `SentinelFetch.exe` (or `SentinelFetch` on other platforms) with no arguments.
3. Open the URL printed in the console if the browser does not open automatically. The default port is 8765; if it is already occupied, the program tries the next nine ports.
4. Pick a location and press **Find scenes**.
5. Select a scene, adjust the appearance, and press **Save file for the calculator**.
6. Transfer the generated `.svd.tns` file to the same calculator folder as `sentinelviewer.prg.tns`. You can keep multiple `.svd.tns` files there and choose between them when the viewer starts.

The saved file is written to the working directory by default. The UI reports the full path and file size after a successful save. The default export filename is `sentinelviewer.svd.tns`, but the calculator now discovers any supported `.svd` or `.svd.tns` file beside the calculator program. Custom `--out` filenames no longer need to be renamed.

## Build the desktop application

### Requirements

- CMake 3.21 or newer
- A C++17 compiler and C99 compiler
- libcurl
- GDAL
- nlohmann-json
- Threads
- On Windows, Visual Studio 2022 or a compatible toolchain is recommended

### Windows with Visual Studio 2022 and vcpkg

Install Visual Studio C++ build tools, CMake, and vcpkg. Set `VCPKG_ROOT` to the vcpkg checkout, then run these commands from the repository root in PowerShell:

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows

cmake --build build --config Release
```

The vcpkg manifest in `vcpkg.json` declares the desktop dependencies. The first configuration may take some time because GDAL and its dependencies need to be installed/built.

The executable is normally produced at:

```text
build/desktop/Release/SentinelFetch.exe
```

Run it with no arguments to launch the browser control panel.

### Windows with MSYS2 MINGW64

Install the toolchain and libraries from an MSYS2 MINGW64 shell:

```sh
pacman -S --needed \
  mingw-w64-x86_64-toolchain \
  mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-curl \
  mingw-w64-x86_64-gdal \
  mingw-w64-x86_64-nlohmann-json
```

From the repository root in the same shell:

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/desktop/SentinelFetch.exe
```

Run the executable from the MINGW64 shell, or add `C:\msys64\mingw64\bin` to `PATH`, so its runtime DLL dependencies can be found.

## Command-line mode

Supplying any of `--lat`, `--lon`, `--out`, `--preview`, `--max-cloud`, `--days`, `--allow-cloudy`, or `--raw` selects command-line download/export mode. The coordinates below are an example; replace them with the centre of your chosen area. Omitting `--lat` and `--lon` uses the application’s built-in starting position. For example:

```powershell
.\build\desktop\Release\SentinelFetch.exe `
  --lat 40.0 `
  --lon -75.0 `
  --days 120 `
  --max-cloud 10 `
  --out sentinelviewer.svd.tns `
  --preview base-preview.bmp
```

Available options:

| Option | Meaning |
|---|---|
| `--lat DEG` | Centre latitude; accepted range is −80 to 80 degrees. Default: the built-in starting position. |
| `--lon DEG` | Centre longitude; accepted range is −180 to 180 degrees. Default: the built-in starting position. |
| `--out FILE` | Output SVD2 file. Default: `sentinelviewer.svd.tns`. |
| `--max-cloud PCT` | Maximum cloud + shadow percentage over the base area. Default: `10`. |
| `--days N` | Search the previous N days. Default: `120`. |
| `--allow-cloudy` | If no usable scene meets `--max-cloud`, allow the least-cloudy usable scene. |
| `--preview FILE.bmp` | Also write a 24-bit BMP of the base 10 m/pixel level only. |
| `--raw` | Skip desktop colour grading and use the default reflectance-to-display curve. The output is still an SVD2 RGB565 display file, not raw spectral-band storage. |
| `--ui` | Force the browser control panel, even when other UI options are supplied. |
| `--port N` | Starting local control-panel port. Default: `8765`; the program tries up to ten consecutive ports. |
| `--no-browser` | Do not automatically open the browser when starting the control panel. |
| `--help` | Print command-line usage. |

`--port` and `--no-browser` are control-panel options and do not, by themselves, switch the program into command-line export mode.

## Build the calculator program

The calculator client is built with the **Ndless SDK**. The calculator source uses the `enable_relative_paths()` and `file_each()` functions from `libndls`, plus the native numeric-input dialog. Use an Ndless SDK whose `libndls.h` declares those APIs. The SDK tools must be available on `PATH`:

- `nspire-gcc`
- `nspire-ld`
- `genzehn`
- `make-prg`

From the repository root:

```sh
cd calculator
make
```

This produces:

```text
sentinelviewer.prg.tns
```

The calculator viewer uses the program path supplied at launch to make its file operations relative to the program folder, then enumerates that folder for supported SVD files. Transfer the program to the TI-Nspire CX II and launch it from the calculator's Ndless environment. The `.prg.tns` extension is the packaged Ndless program; it is separate from the `.svd.tns` satellite data file.

## Transfer and open the exported imagery

1. Transfer `sentinelviewer.prg.tns` and one or more `.svd.tns` data files into the **same calculator folder** using a supported TI-Nspire file-transfer method (for example, TI-Nspire Computer Link or N-Link).
2. Launch `sentinelviewer.prg.tns` from the calculator.
3. At startup, the viewer scans the folder containing the launched program for `.svd` and `.svd.tns` files with a supported SVD1 or SVD2 header. The choices are sorted by filename and shown in a numeric selection dialog. If more than eight files are found, choose a page first, then choose a file from that page.
4. Use `×` and `÷` to change geographic level. Use `I` to confirm the current resolution and scene metadata.

The exported data filename is no longer fixed: you can keep multiple exports in the program's folder and choose which one to load each time the calculator app starts. The desktop default remains `sentinelviewer.svd.tns`, but custom `--out` filenames work without renaming.

The desktop application does not implement a live USB-pull protocol in which the calculator requests data directly from the PC. File transfer is a separate step; see [`docs/live-usb.md`](docs/live-usb.md) for the design limitation.

## Tests

### Shared SVD format and renderer tests

The portable C format/renderer test suite can be run without the desktop dependencies using GCC or a compatible C compiler:

```sh
sh tests/run.sh
```

This compiles and runs `tests/test_svd.c` with AddressSanitizer and UndefinedBehaviorSanitizer enabled. It covers SVD1 parsing and payload checks, SVD2 headers and levels, LZSS decode/round trips, checksum failures, and RGB565 rendering. The command writes its test executable and sample PPM images inside `tests/`.

### CMake test targets

Configure with `-DSVD_BUILD_TESTS=ON` to enable the CMake tests. With the desktop dependencies available, this also builds the grading/export tests:

```sh
cmake -S . -B build -DSVD_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The grading/export tests use a fake scene source and exercise grading, SVD2 creation, file read-back, checksums, and matching between the base-level preview and exported pixels. They do not contact Earth Search or validate a live satellite download.

## Known limitations and status

- The web preview has no level picker yet. It previews the base 10 m/pixel crop only; levels 2–5 are available after export on the calculator.
- The wider levels are lower-resolution views of larger areas. They are intended for regional context, not close-up detail.
- The browser map, tile layers, and place-name search rely on external services and may not load without internet access. Manual coordinates remain available.
- A physical calculator/OS test is still needed for the Ndless client, including file transfer, screen rendering, memory use, and key handling on the intended device.
- A complete live build and download test of the curl/GDAL/STAC source should be performed in the target desktop environment with the declared dependencies installed.
- Live USB pull/push is not implemented; use a file-transfer utility.
- Sentinel-2 10 m/pixel imagery is not building-level or survey-grade imagery. Cloud, haze, shadows, acquisition date, and resampling affect what can be seen.

## Source layout

```text
common/       Portable C99 SVD1/SVD2 format, compression, checksums, renderer
desktop/      SentinelFetch application, STAC/GDAL source, grading, local HTTP UI
 calculator/  Ndless TI-Nspire viewer
 docs/        Design notes and known USB-transfer limitation
 tests/       Host-side format, renderer, grading, and export tests
```

## License

See [`LICENSE.txt`](LICENSE.txt).
