# Recorded ADC Sample

The repository publishes the original recorded BIN used in earlier application
validation as a Release asset, keeping the source checkout small.

## Download

[ADC_DATA.bin](https://github.com/QianLei31/NL_source/releases/download/sample-adc-data-20261001/ADC_DATA.bin)

[Release page](https://github.com/QianLei31/NL_source/releases/tag/sample-adc-data-20261001)

| Property | Value |
|---|---|
| Original file name | ADC_DATA.bin |
| File size | 339,730,028 bytes (323.99 MiB) |
| Complete frames at 1,024 bytes/frame | 331,767 |
| Incomplete trailing bytes | 620 |
| Sampling rate | Not confirmed |
| TDM acquisition mode | Not confirmed |
| File modification time in UTC | 2026-06-20T17:13:37.9010315Z |
| Publication date | 2026-10-01 |

The modification time is filesystem metadata, not a verified acquisition date.
The original BIN was not trimmed or re-encoded. There is no original
`session.json` accompanying this sample, so acquisition settings must be
provided manually.

SHA-256:

```text
5cf03681e067c27cb5193c590b363bbd4b3f3d4c38a0186157744bd70e47bba0
```

## Playback

1. Download the BIN into a dedicated folder.
2. Set the sampling rate to the value used in acquisition, then open this file
   through the application's BIN playback controls.
3. Enable TDM only if this recording was acquired in TDM mode. The application
   reads 256 interleaved 32-bit little-endian words per frame and uses the low
   12 bits of each word as the ADC sample.
4. The final 620 bytes do not form a complete frame. V6.0.4 ignores this tail
   and reports a warning.

The first four inspected frames have zero timestamp high bits on all 256
channels. This limited inspection does not establish timestamp behavior
throughout the entire recording; do not treat this sample as a verified
timestamp-continuity fixture.

The sample can also be selected in the storage panel's offline MATLAB binary
export. Supply the known acquisition sampling rate and mode there. Exported
ADC voltage is `double(code) * 1.8 / 4096`; input-referred voltage additionally
depends on the original front-end gain.

## Check the Download

```powershell
Get-FileHash -LiteralPath .\ADC_DATA.bin -Algorithm SHA256
```

The result must equal the checksum above. [ADC_DATA.metadata.json](ADC_DATA.metadata.json)
contains the same file information in machine-readable form.
