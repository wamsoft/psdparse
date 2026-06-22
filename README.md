# psdparse

Pure C++17 PSD (Photoshop) reader/writer library, with pybind11-based Python bindings.

- Lazy I/O: PSD pixel data is **not** copied into memory at parse time. Only the structural metadata (a few hundred KB even for large files) is read upfront; layer pixels are paged in on demand via mmap or stream callbacks.
- Round-trip save: `load(p) -> save(q)` produces a byte-identical PSD file.
- Python wrapper: `import psdparse` → `PSDFile.load(path) / layer_image(i) / merged_image() / save(path)`.

The library was extracted from the [psdfile](https://github.com/wamsoft/psdfile) kirikiri plugin in 2026. psdfile now consumes this library as a submodule.

## Architecture (quick read)

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for full details.

```
IteratorBase (psdbase.h, pure virtual — parser only sees this)
├── MemoryReader   (psdparse.h)  ... mmap / contiguous buffer
└── StreamReader   (psdparse.h)  ... arbitrary seekable stream
    └── Source (pure virtual; subclass per backend)
        ├── IStreamSource              ... std::istream
        └── (your custom Source)       ... e.g. iTJSBinaryStream wrapper

WriterBase (psdwrite.h, pure virtual — symmetric to IteratorBase)
└── FileWriter (FILE*)
```

`psd::PSDFile::load(const char *path)` mmaps a local file. `loadFromStream(std::istream&)` / `loadFromReader(IteratorBase&)` accept arbitrary I/O. `save(const char *path)` writes the loaded data back as PSD.

All public path arguments are **UTF-8** (`char *`). On Win32, conversion to UTF-16 happens internally via `psd::utf8ToWide` (inline in psdbase.h).

## Install (Python)

```bash
pip install psdparse          # once published to PyPI
```

Build from source — needs only a C++17 compiler + CMake 3.16+, **no vcpkg**:

```bash
pip install .                 # or:  pip wheel . -w dist
```

`zlib` is taken from the system if present, otherwise fetched from source by
CMake (`FetchContent`), so no package manager is required. Packaging uses
[scikit-build-core](https://scikit-build-core.readthedocs.io/); cross-platform
wheels are built in CI (`.github/workflows/wheels.yml`, cibuildwheel).

## Build (C++ library / CLI)

Requires CMake 3.16+ and a C++17 compiler. **vcpkg is no longer needed.**

```powershell
# C++ library + CLI (static CRT)
cmake --preset x64-windows
cmake --build --preset x64-windows --config Release

# C++ library + Python module (dynamic CRT, matches CPython)
cmake --preset x64-windows-python
cmake --build --preset x64-windows-python --config Release
```

`Makefile` is a thin wrapper:

```
make PRESET=x64-windows prebuild build
make PRESET=x64-windows-python prebuild build
```

Build artifacts:
- `build/x64-windows/psdparse/Release/psdparse_cli.exe`
- `build/x64-windows-python/python/Release/psdparse.cp312-win_amd64.pyd`

Only dependency: `zlib` (system, or auto-fetched from source).

## Python API

```python
import psdparse

p = psdparse.PSDFile()
p.load(r"path/to/file.psd")   # mmap-backed

print(p.header.width, p.header.height, len(p.layers))
for layer in p.layers:
    print(layer.name_unicode, layer.blend_mode.name, layer.opacity)

bgra = p.merged_image()                # bytes, BGRA, 4*W*H
layer_bgra = p.layer_image(0, "masked") # bytes, BGRA, 4*w*h

p.save(r"out.psd")              # byte-identical round-trip
```

Full API reference: [docs/PYTHON_API.md](docs/PYTHON_API.md).

## Tests

Tests live under `tests/` and use [pytest](https://docs.pytest.org/). They need sample PSDs placed at the repo root or `tests/data/` (not committed — listed in `.gitignore`); without them the tests skip rather than fail.

```powershell
# after building the Python module (pip install . / preset x64-windows-python):
python -m pytest -v
```

## tools/psd_export.py

```
python tools/psd_export.py input.psd [--out-dir DIR] [--mode masked|image|mask]
```

Outputs `layers.json` (full layer metadata), `merged.png` (composite), and per-layer PNGs.

## License

MIT — see [LICENSE](LICENSE).
