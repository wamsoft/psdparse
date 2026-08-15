# psdparse

Pure C++17 PSD (Photoshop) reader/writer library, with pybind11-based Python bindings.

- Lazy I/O: PSD pixel data is **not** copied into memory at parse time. Only the structural metadata (a few hundred KB even for large files) is read upfront; layer pixels are paged in on demand via mmap or stream callbacks.
- Round-trip save: `load(p) -> save(q)` produces a byte-identical PSD file.
- **Edit & save**: add / delete / reorder / duplicate layers (folder-aware moves), copy layers between files, replace layer & mask pixels, edit parameters / names / masks / fill opacity, edit layer-effect (`lfx2`) values, and edit text layers (body text, per-run style, rich-text run/paragraph rebuild, placement & text box), or build a PSD from scratch — all with byte-exact re-serialization of the parts you touch (unedited layers stay byte-identical).
- Python wrapper: `import psdparse` → `PSDFile.load(path) / layer_image(i) / merged_image() / save(path)` plus the editing API.

The library was extracted from the [psdfile](https://github.com/wamsoft/psdfile) kirikiri plugin in 2026. psdfile now consumes this library as a submodule.

## Architecture (quick read)

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for full details.

```
IteratorBase (psdbase.h, pure virtual — parser only sees this)
├── MemoryReader   (psdparse.h)  ... mmap / contiguous buffer
└── StreamReader   (psdparse.h)  ... arbitrary seekable stream
    └── Source (pure virtual; subclass per backend)
        ├── IStreamSource              ... std::istream
        └── (your custom Source)       ... e.g. a host app's stream class

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

Editing (8-bit RGB), re-serialized on save — unedited layers stay byte-identical:

```python
p.layers[0].opacity = 128                       # parameters
p.layers[0].name_unicode = "背景"               # rename
p.delete_layer(3); p.move_layer(1, 4)           # structure
p.set_layer_pixels(0, bgra_bytes, w, h)         # replace pixels
p.set_layer_mask_pixels(0, gray, 0, 0, w, h)    # mask pixels + geometry
p.set_effects(0, {"masterFXSwitch": False})     # layer-effect values
p.set_text(2, "新しいテキスト")                 # text content
p.save(r"edited.psd")

q = psdparse.PSDFile()                           # or build one from scratch
q.create_blank(1024, 768)
q.add_layer("bg", 0, 0, bgra_bytes, 1024, 768)
q.save(r"new.psd")
```

Full API reference: [docs/PYTHON_API.md](docs/PYTHON_API.md).

Rich text, folder-aware moves and text placement are bound too:

```python
p.set_rich_text(2, "赤\r青", [{"length": 2, "color": (1, 0, 0)},
                             {"length": 2, "color": (0, 0, 1), "font": "Arial"}])
p.set_justification(2, 2)              # center every paragraph
p.set_run_style(2, 0, font="Times New Roman")
p.move_text_layer(2, 40, -15)          # transform + layer/mask rect
p.set_text_bounds(2, 0, 0, 300, 200)   # flow box (paragraph text)

p.move_layer_sibling(i, up=True)       # whole folders move as one block
start, count = p.group_span(i)
```

## PSD feature coverage

What psdparse can and cannot read **and edit**, at a glance:
[docs/SUPPORT.md](docs/SUPPORT.md) (対応状況マトリクス).

## Tests

Tests live under `tests/` and use [pytest](https://docs.pytest.org/). The small
synthesized fixtures are committed under `tests/data/` (`masktest.psd`,
`maskparams.psd`, `labsample.psd`, `graysample.psd`, `duosample.psd`,
`textboxsample.psd`, `textmask.psd` — force-added past the `*.psd` ignore rule).
The larger real-world samples (`fontsample.psd`, `config.psd`, artwork PSDs,
Adobe's `Layer Comps.psd`) are **not** committed; drop them into `tests/data/`
or the repo root and the tests that need them stop skipping.

```powershell
# after building the Python module (pip install . / preset x64-windows-python):
python -m pytest -v
```

## Examples (composite / variations / sprite extraction)

psdparse gives you the pieces (per-layer pixels, position, opacity, masks) but
does **not** composite or re-render — the Python imaging ecosystem does that
better. [`examples/`](examples/) shows the recipes:

- **`composite.py`** — composite layers with Pillow (position + opacity + mask,
  normal blend), and optionally write the result back as the PSD's stored
  preview via `set_merged_image`.
- **`variations.py`** — enumerate tachie/expression combinations by treating
  layer folders as mutually-exclusive option slots.
- **`extract_layers.py`** — export per-layer PNGs + a manifest, with alpha edge
  extension (bleed) so sprites stay clean under rotation/scaling.

See [examples/README.md](examples/README.md). On the sample tachie PSDs
(all normal blend) `composite.py` matches Photoshop's stored composite to ~0.1
mean level difference.

## tools/psd_export.py

```
python tools/psd_export.py input.psd [--out-dir DIR] [--mode masked|image|mask]
```

Outputs `layers.json` (full layer metadata), `merged.png` (composite), and per-layer PNGs.

## License

MIT — see [LICENSE](LICENSE).
