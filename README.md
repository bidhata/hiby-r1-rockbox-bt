# rockbox-hiby-r1

Patch-maintenance repo for **HiBy R1** Rockbox builds with **Bluetooth audio** support.

This repository does **not** vendor the full Rockbox source tree. Instead, it builds a HiBy R1 Rockbox release by:

1. fetching upstream Rockbox at a pinned commit  
2. copying in extra source files from `overlay/`  
3. applying the patch series from `patches/`  
4. building release artifacts for the HiBy R1

## What it adds

### Bluetooth Audio (aptX + SBC)

- **Bluetooth menu** in Rockbox — scan, pair, connect, disconnect, and status
- **Automatic codec negotiation** — prefers **aptX** when available, falls back to **SBC**
- Active codec displayed in the **Status** screen
- Playback routing to **BlueALSA A2DP** with automatic fallback to **local audio** on disconnect/failure
- **Absolute volume** sync from Rockbox to the Bluetooth stack
- Bluetooth preserved across the **bootloader → Rockbox** handoff
- BT-aware **PCM buffering** optimized for wireless latency

### USB / ADB

- ADB handled through the existing USB mode setting path
- Clean mode-switch with proper gadget reattach cycle

## Repository layout

```
├── .github/workflows/    CI build + release automation
├── baseline/             Provenance for the captured on-device baseline
├── overlay/              New source files not present upstream
│   ├── apps/             Bluetooth controller + menu integration
│   └── firmware/         PCM backend hooks for BlueALSA routing
├── patches/              Patch series applied on top of upstream Rockbox
└── scripts/              Local reproducible build script
```

## Build outputs

A successful build produces:

| Artifact | Description |
|----------|-------------|
| `rockbox.r1` | Rockbox firmware binary |
| `bootloader.r1` | Bootloader binary |
| `r1_rb.upt` | Flashable update package (when a base `r1.upt` is provided) |
| `rockbox-info.txt` | Rockbox build info |
| `build-metadata.txt` | Build provenance (commits, mirrors, config) |
| `SHA256SUMS` | Checksums for all artifacts |

## Build locally

```bash
./scripts/build.sh
```

Artifacts are written to `out/`.

To also build a flashable `.upt` package:

```bash
R1_UPDATE_PATH=/path/to/r1.upt ./scripts/build.sh
```

Or:

```bash
R1_UPDATE_URL='https://example.invalid/r1.upt' \
R1_UPDATE_SHA256='<sha256>' \
./scripts/build.sh
```

## Install

### Update an existing Rockbox install

If Rockbox is already installed, replace:

```
/usr/data/mnt/sd_0/.rockbox/rockbox.r1
```

with the built `rockbox.r1`, then reboot into Rockbox.

### Install a packaged release

If a release includes `r1_rb.upt`, use that through the normal HiBy R1 local update flow.

For most users, `r1_rb.upt` is the simplest way to install a full packaged build.

## Releases

GitHub Actions builds release artifacts automatically for version tags:

```bash
git tag v0.1.0
git push origin v0.1.0
```

## Bluetooth Codec Support

| Codec | Status | Notes |
|-------|--------|-------|
| **aptX** | ✅ Preferred | Auto-selected when the receiver and on-device BlueALSA support it |
| **SBC** | ✅ Fallback | Always available as the baseline A2DP codec |
| **LDAC** | 🔜 Planned | Architecturally supported; depends on on-device BlueALSA + libldac |
| **aptX HD** | 🔜 Planned | Same auto-negotiation framework; needs on-device library |

To check which codecs your device supports, connect via ADB and run:

```bash
bluealsa-cli codec /org/bluealsa/hci0/dev_XX_XX_XX_XX_XX_XX/a2dpsrc/sink
```

## Known Issues

- Bluetooth connection setup currently prefers **aptX**, falling back to **SBC** if unavailable.
- Bluetooth support is still **experimental** — expect occasional issues with pairing, connecting, or routing audio.
- **Music playback does work**, but the overall Bluetooth experience is not yet fully reliable.
- **LDAC** support requires on-device `libldac` libraries and a BlueALSA build with `--enable-ldac`.

## License

This project is licensed under the [MIT License](LICENSE).
