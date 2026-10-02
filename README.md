# NL_source

This repository contains the V6 C++/Qt6 source of Neural Signal Command Center,
version **6.0.4-20260929**. It is a standalone CMake project; previous V2-V5
project directories are not required.
V6 keeps the established acquisition and license protocol while unifying live acquisition,
recording, BIN replay, and page distribution through one session pipeline.

## Version 6.0.4 (2026-09-29)

- Shared, stamped frame coordinates keep TDM electrode identities stable over
  upstream gaps, page switches, queue drops, replay seeks, and EOF restarts.
- Queued SPI commands are bound to their original endpoint; changing the
  control address cancels outstanding old-device work and ignores stale replies.
- BIN selection now checks session membership; replay and export share part
  validation, and replay reports read errors instead of remaining in play state.
- Replay sample rates no longer overwrite the live acquisition configuration.
- Time-domain zoom uses source coordinates. Invalid FFT metrics are cleared
  while the current spectrum remains available when it can be computed.
- Recording manifests include source frame origin and timestamp integrity
  counters. TDM MATLAB exports include four shared binary time-index files.
- Spike retention is capped at 128 MiB and rejects stale timeline writes.

See [the repair report](docs/v6_6.0.4_fixes_20260929.md) for verification and the
MATLAB time-index API.

## Version 6.0.3 (2026-07-27)

- Spike amplitudes, RMS values, absolute thresholds, and retained snippets are
  now input-referred. The Spike page provides explicit 60x/180x conversion
  gain selection matching the REC register.
- The built-in Spike source now models the hardware's default 60x front-end
  gain instead of the former test-only 256x value.
- Each ADC/TDM electrode can keep an independent draggable threshold.
- Clicking an electrode opens a non-modal single-channel window with its own
  Y range, auto-range, overlay-count, and trace-fade settings.
- Existing output-referred absolute thresholds are migrated once to
  input-referred units.

## Implemented now

- Five pages:
  - SPI control page
  - Realtime waveform page
  - Unified analyzer page
  - Spike sweep page
  - Array-wide Spike retention page
- Unified session pipeline:
  - One live or BIN source shared by all pages
  - Background loss-aware recording with frame-aligned file splitting
  - `session.json` metadata manifest and continuous multi-part replay
  - Fixed real-time 1x replay, pause, seek, and queue backpressure
- Core read pipeline preserved:
  - TCP receive threads
  - Frame alignment by `256 * 4` bytes
  - Channel sorting and ring buffer
- Raw stream save to timestamped session folders
- Theme system with 5 styles, runtime switchable.
- Plain-text user config in `NL_CommandCenter_v6_config.ini`, including network, recording, realtime waveform, channel-map, acquisition, FFT, analyzer, UI, SPI direct/quick-command, and stimulator parameters.
- Version update check from the existing license/update server, with a GitHub release download link shown in Settings.

## Build prerequisites (Windows)

- Qt 6 (Core, Gui, Widgets, Network, OpenGL, OpenGLWidgets, Concurrent)
- CMake >= 3.20
- A C++17 compiler; the tested Windows toolchain is MinGW 11.2 with Qt 6.5.3.
- OpenSSL libcrypto with Ed25519 support. The current Windows CMake integration
  expects `include/openssl/evp.h`, `lib/libcrypto.dll.a`, and
  `bin/libcrypto-1_1-x64.dll` under `OPENSSL_ROOT_DIR`.

Use the MinGW kit and matching OpenSSL import library for the build below.
An MSVC build requires adapting the OpenSSL library discovery in CMake.

## Build

```powershell
git clone https://github.com/QianLei31/NL_source.git
cd NL_source

# Replace these paths with your local Qt, MinGW, and OpenSSL installations.
$qtRoot = 'E:/Qt/6.5.3/mingw_64'
$mingwRoot = 'E:/Qt/Tools/mingw1120_64'
$opensslRoot = "$mingwRoot/opt"
$env:PATH = "$mingwRoot/bin;$qtRoot/bin;" + $env:PATH

cmake -S . -B build -G 'MinGW Makefiles' `
  "-DCMAKE_CXX_COMPILER=$mingwRoot/bin/g++.exe" `
  "-DCMAKE_MAKE_PROGRAM=$mingwRoot/bin/mingw32-make.exe" `
  "-DCMAKE_PREFIX_PATH=$qtRoot" `
  "-DOPENSSL_ROOT_DIR=$opensslRoot" `
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j 6
ctest --test-dir build --output-on-failure -j 1
```

Run the commands from the repository root. The previous multi-version workspace
presets and helper scripts are not needed. CMake deploys Qt runtime files next
to the Windows executable when `windeployqt` is available.

## Run

```powershell
.\build\NL_CommandCenter_v6_qt.exe
```

The standalone repository was built from scratch on 2026-10-01 with Qt 6.5.3
and MinGW 11.2; all **40/40 tests passed**. Tests use temporary files and
loopback servers, not real FPGA hardware.

## Source layout

- `src/`: application, acquisition, processing, UI, and Qt theme resources.
- `tests/`: protocol, signal-processing, session, and review regression tests.
- `docs/`: design notes, compatibility notes, and repair reports.
- `tools/`: optional external Python dummy servers.
- `NL_CommandCenter_v6_config.ini`: shipped configuration template.

Build output, DLLs, logs, and machine activation caches are excluded. Large
recorded BIN samples are published as Release assets; see the sample-data
section below. Historic prototype files still present under `src/` are built
only when listed in `CMakeLists.txt`.

## Recorded sample data

The original `ADC_DATA.bin` supplied for application validation is available
from the [recorded-data release](https://github.com/QianLei31/NL_source/releases/tag/sample-adc-data-20261001).

- [Download ADC_DATA.bin](https://github.com/QianLei31/NL_source/releases/download/sample-adc-data-20261001/ADC_DATA.bin)
- [Data description and playback notes](datasets/README.md)
- [Metadata](datasets/ADC_DATA.metadata.json) and [SHA-256 checksum](datasets/SHA256SUMS.txt)

The uploaded file is unchanged. Its sampling rate and TDM mode are not
confirmed; set them to the original acquisition parameters for quantitative
analysis. The 620-byte incomplete tail is ignored by the application's
frame-aligned reader.

## Basic use

For hardware, configure the host and the control/data ports in Settings
(the default pair is 7 and 5001). Use the global receive and recording controls
to acquire data, then switch between waveform, analyzer, and spike views.
BIN replay and MATLAB binary export are available through the session controls
and the storage panel. TDM displays the selected 0/2 or 1/3 phase pair; TDM
export writes all four phases and includes sample-position files.

For a local test, choose the managed sine or spike source in Settings, or start
an external Python dummy from `tools/` and select its loopback ports.

## Compatibility notes

- The executable-side `NL_CommandCenter_v6_config.ini` is the default template. Runtime edits are saved to the user config location, typically `%LOCALAPPDATA%\NeuralLab\NL_CommandCenter_v6_qt\NL_CommandCenter_v6_config.ini`, so settings survive switching to a new release folder. Older `config.ini` files are migrated automatically if the user config does not exist.
- SPI binary command encoding keeps `"spi" + hex(4 bytes)` pattern.
- Stream frame parsing keeps 256-channel, 4-byte-per-point protocol.
- Unified panel FFT follows the ADC analyzer reference flow: windowed FFT, configurable DC-bin removal, configurable signal-bin integration, and power-bin statistics.
- License product id remains `nl_command_center_v4`, so existing compatible activations and update-server behavior are preserved.

## Update Check

The Settings popover checks the existing server endpoint:

```text
GET http://47.101.131.245/v1/app-version?product=nl_command_center_v4&current_version=6.0.4-20260929&channel=stable
```

Expected JSON fields:

```json
{
  "latest_version": "6.0.4",
  "download_url": "https://github.com/QianLei31/NL_CommandCenter_v5_release/releases/latest",
  "message": "optional short release note"
}
```

`version`, `tag`, or `tag_name` are accepted aliases for `latest_version`; `release_url`, `github_url`, or `url` are accepted aliases for `download_url`.

If this endpoint is not deployed yet and returns 404, the Settings panel keeps the GitHub download button enabled and reports that the update server has no version metadata yet.
