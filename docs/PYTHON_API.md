# psdparse Python API Reference

The Python bindings expose a small surface area focused on **reading** PSDs, **extracting raw pixel data**, and **saving** a loaded file back as PSD. Pixel data is returned as raw BGRA `bytes` objects suitable for handing to PIL, NumPy, etc.

All public path arguments are Python `str`. pybind11 transparently encodes them as UTF-8 before reaching the C++ layer.

```python
import psdparse
```

## `class psdparse.PSDFile`

### Constructor

```python
p = psdparse.PSDFile()
```

### Loading

```python
p.load(path: str) -> bool
```

Memory-map the file at `path` and parse. On Windows the path is converted UTF-8 → UTF-16 internally before `CreateFileMappingW`. Layer pixels are paged in lazily — the parse step only reads structural metadata (a few hundred KB even for very large PSDs).

```python
p.load_bytes(data: bytes) -> bool
```

Parse a PSD already loaded into a Python `bytes`. The bytes are copied into an internal `std::vector`. Useful when the file came from somewhere other than disk.

```python
p.load_streamed(path: str) -> bool
```

Open `path` as a `std::ifstream` and parse via `StreamReader`. Functionally equivalent to `load()` but exercises the stream code path — handy for testing and for environments where mmap isn't appropriate (network paths, etc.).

### Saving

```python
p.save(path: str) -> bool
```

Save the currently loaded data back to disk as PSD. **The current implementation is round-trip-only**: `p.load(a); p.save(b)` produces a byte-identical copy. Modifying `layers` after load and then saving is not yet supported (see [ROADMAP.md](ROADMAP.md) for the per-channel / RLE-encoder work needed for that).

### Header

```python
p.header.width       # int
p.header.height      # int
p.header.channels    # int
p.header.depth       # int (8 / 16 / 32)
p.header.mode        # int (use psdparse.COLOR_MODE_* constants to compare)
p.header.version     # int (1 or 2)
```

### Layers

```python
p.layers             # list[LayerInfo] -- read-only
p.merged_alpha       # bool
p.is_loaded          # bool
```

### Image extraction

```python
p.merged_image() -> bytes
```

Returns the composite image as raw BGRA bytes. Length = `width * height * 4`. Raises `RuntimeError` if the PSD didn't store a composite (rare).

```python
p.layer_image(index: int, mode: str = "masked") -> bytes
```

Returns the pixels of one layer as raw BGRA bytes.

- `mode="masked"` (default) — the image with the layer mask applied to alpha
- `mode="image"` — the image only, ignoring mask
- `mode="mask"` — the mask only, rendered as grayscale-in-BGRA

Length = `layer.width * layer.height * 4`. Returns `b""` for empty layers (`width == 0` or `height == 0`). Raises `IndexError` on bad index, `ValueError` on bad mode.

## `class psdparse.LayerInfo`

Read-only view of one layer.

| Attribute | Type | Notes |
|---|---|---|
| `top, left, bottom, right` | `int` | layer bounding box on canvas |
| `width, height` | `int` | derived from bbox |
| `opacity` | `int` | 0..255 |
| `fill_opacity` | `int` | 0..255 |
| `clipping` | `int` | 0=base, 1=non-base |
| `blend_mode_key` | `int` | raw 4cc value (e.g. `'norm'` as int) |
| `blend_mode` | `BlendMode` enum | parsed blend mode |
| `layer_type` | `LayerType` enum | NORMAL / HIDDEN / FOLDER / ADJUST / FILL / TEXT |
| `layer_id` | `int` | -1 if unset |
| `channels` | `list[ChannelInfo]` | per-channel id+length |
| `name` | `str` | raw Pascal-string name (CP932 etc on Japanese PSDs — pybind11 may raise UnicodeDecodeError when read) |
| `name_unicode` | `str` | UTF-16 Unicode name from `luni` record (preferred) |
| `visible` | `bool` | flag bit 1 inverted |
| `transparency_protected` | `bool` | flag bit 0 |
| `obsolete` | `bool` | flag bit 2 |
| `pixel_data_irrelevant` | `bool` | flag bit 4 |

**Tip:** For Japanese PSDs prefer `name_unicode` and fall back to `name` only inside a `try / except UnicodeDecodeError`.

## Enums

```python
psdparse.LayerType.NORMAL
psdparse.LayerType.HIDDEN
psdparse.LayerType.FOLDER
psdparse.LayerType.ADJUST
psdparse.LayerType.FILL
psdparse.LayerType.TEXT
```

```python
psdparse.BlendMode.NORMAL
psdparse.BlendMode.MULTIPLY
psdparse.BlendMode.SCREEN
psdparse.BlendMode.OVERLAY
psdparse.BlendMode.PASS_THROUGH
# ... 28 values total. Use BlendMode.<name>.value to get the int.
```

Module-level integer constants for direct comparison:

```python
psdparse.COLOR_MODE_BITMAP, COLOR_MODE_GRAYSCALE, COLOR_MODE_INDEXED,
COLOR_MODE_RGB, COLOR_MODE_CMYK, COLOR_MODE_MULTICHANNEL,
COLOR_MODE_DUOTONE, COLOR_MODE_LAB

psdparse.LAYER_TYPE_NORMAL, LAYER_TYPE_HIDDEN, LAYER_TYPE_FOLDER,
LAYER_TYPE_ADJUST, LAYER_TYPE_FILL
```

## Pixel format

All `*_image()` methods return interleaved BGRA in little-endian byte order:

```
byte 0: B  (blue)
byte 1: G  (green)
byte 2: R  (red)
byte 3: A  (alpha)
```

This matches PIL's `"BGRA"` raw decoder:

```python
from PIL import Image
img = Image.frombytes("RGBA", (w, h), bgra_bytes, "raw", "BGRA")
img.save("out.png")
```

For NumPy:

```python
import numpy as np
arr = np.frombuffer(bgra_bytes, dtype=np.uint8).reshape(h, w, 4)
# arr[..., [0,1,2,3]] is B, G, R, A
```

## Error model

- Invalid paths: `load()` / `load_streamed()` return `False` (no exception).
- Invalid PSD data: `load*()` returns `False` and the object is left empty (`is_loaded == False`).
- Out-of-range `layer_image(index)`: raises `IndexError`.
- Bad `mode` string: raises `ValueError`.
- Reading `name` on a non-UTF-8 byte sequence: raises `UnicodeDecodeError` — see the tip above.

## Memory model

- `load()` mmaps the file. The mapping is held by the `PSDFile` instance; it is unmapped when the instance is destroyed or a new file is loaded.
- `load_streamed()` keeps a `std::ifstream` alive in `PSDFile`. Closed on destruction / re-load.
- `load_bytes()` copies the input into an internal vector. The Python `bytes` can go out of scope safely.
- Layer pixel decoding allocates a fresh BGRA buffer of `4 * w * h` bytes per call. There's no caching — call once and hold the result if you need it twice.

## Worked example: export every layer to PNG

```python
import psdparse
from PIL import Image

p = psdparse.PSDFile()
assert p.load("file.psd")

for i, layer in enumerate(p.layers):
    if layer.layer_type != psdparse.LayerType.NORMAL: continue
    if layer.width == 0 or layer.height == 0:        continue
    name = layer.name_unicode or f"layer_{i}"
    img = Image.frombytes(
        "RGBA", (layer.width, layer.height),
        p.layer_image(i, "masked"), "raw", "BGRA"
    )
    img.save(f"{i:03d}_{name}.png")
```

See `tools/psd_export.py` for a more complete extraction tool that also dumps metadata as JSON.
