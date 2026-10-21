# Reconstruction Source Status

`gateway_parser.py`, `packet_parser.py`, `data_processor.py`, and
`websocket_server.py` are maintained Python source files. The protocol engine
modules under `vendor_pyc` are the original Python 3.14 bytecode modules from
the supplied PyInstaller archive. They are included as application inputs to
preserve the exact protocol behavior while the top-level gateway is rebuilt.

The build path is therefore reproducible and runnable today, but it is not a
claim that every third-party and protocol bytecode module has been converted
to source text. `build.ps1` supplies the dependency path and explicitly adds
the one module whose package import is not discoverable by static analysis.

The resulting one-file executable contains the six terrain GLBs, the
`NvaAssets.pak` archive, and both WinDivert files. The smoke checks exercise
CLI startup, JSONL completion, `/api/status`, `/resources/map_catalog.json`,
`/m3d/manifest.json`, and a terrain GLB response.

The complete live entry is `capture_launcher.py`. The supplied outer EXE is a
.NET single-file bundle containing readable managed IL; it embeds `NvaParser.exe` and owns
hotspot client discovery, WinDivert capture, TGCP key handling, and UDP
decryption. `NvaLocalRadarFull.exe` bundles that verified host as
`original_nva.exe`, plus the driver and asset files, so the rebuilt EXE starts
the same live capture path instead of waiting for synthetic stdin JSONL.

The managed host key and decryption path has now been traced and checked with
synthetic inputs; see [KEY_DECRYPTION_TRACE.md](KEY_DECRYPTION_TRACE.md).
