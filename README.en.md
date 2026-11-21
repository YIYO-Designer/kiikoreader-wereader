# kiikoreader WeRead Firmware

Community firmware source for **kiikoreader Metalio S31**, based on firmware `0.3.3-rc113`. It integrates WeRead shelves, account management, book caching, progress synchronization, and reading-duration reporting while reusing the native shelf, reader, and system font settings.

> This is not an official release by Tencent/WeRead or Metalio/kiikoread. WeRead endpoints are unofficial service interfaces and may change without notice.

中文说明：[README.md](README.md)

## Features

- QR-based account login and account management.
- Sync the WeRead shelf and covers in server order, with cached-state indicators.
- Cache book text and chapter images as local EPUB files.
- Read cached books with the firmware's native reader and its existing features.
- Upload local reading progress or download cloud progress.
- Track and report reading time for eligible WeRead books over Wi-Fi; offline users can connect for reporting or continue local reading.
- Reuse the existing shelf UI, pagination, and system font-size settings.

## Build

See [source layout](docs/SOURCE_LAYOUT.md), [WeRead architecture](docs/WEREAD_ARCHITECTURE.md), and [Metalio S31 build guide](docs/BUILD_METALIO_S31.md). ESP-IDF **v6.1** is required. From an exported ESP-IDF environment at the repository root:

```bash
python tools/build_firmware.py --board metalio47 --idf-metalio47 "$IDF_PATH" --python "$(command -v python)" --output build/boards --s31-pie --s31-text-ripple --s31-reader-panel37
```

GitHub Actions runs board builds and host-side checks. Verify the board, partition layout, and image type before flashing a device.

## License

Licensing varies by file and component. See [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md); do not treat the whole repository or historical firmware as covered by one license.

## Wuming 4G variant (3.0.0)

The supplied EC718 / ESP32-S31 4G variant is preserved as a pinned v3.0.0 base plus a source overlay in [`variants/wuming-4g/`](variants/wuming-4g/). See [source, build and flashing instructions](docs/WUMING_4G.md) and the [file inventory](docs/WUMING_4G_SOURCE.md). Restore it with `python tools/materialize_wuming_4g.py --output build/wuming-4g/source` before building; the root project still builds the original S31/Pico firmware.

The existing [v3.0.0 release](https://github.com/YIYO-Designer/kiikoreader-wereader/releases/tag/v3.0.0) additionally provides only `Kiikoread_Metalio_S31_4G_full_16MB.bin` (16 MiB, write at `0x0`, erases internal flash). Some bundled IoT SIMs may not reach WeRead; use a compatible external SIM or Wi-Fi. This import does not claim fresh hardware testing.
