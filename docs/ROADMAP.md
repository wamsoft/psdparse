# psdparse Roadmap

## Current state (2026-06-12)

- ✅ Pure C++17 parser (no Boost)
- ✅ mmap + StreamReader / Source abstraction
- ✅ Python bindings (pybind11)
- ✅ pytest regression suite (27 tests)
- ✅ Round-trip PSD save (byte-identical)
- ✅ UTF-8 path I/F (Win32 conversion internal only)

## Save: from round-trip to edit-and-save

The current `save()` implementation works correctly only when the loaded `Data` structure is **unmodified** between load and save. Supporting structural edits (delete, insert, rename, replace pixels) breaks down into three independent layers of work.

### Phase 4b — per-channel save (enables delete, duplicate)

**Problem:** `writeLayerInfo` currently dumps `Data::channelImageData` as one blob. This blob is the concatenated pre-RLE channel bytes for *every* channel of *every* layer in the original file. Deleting a `LayerInfo` from `layerList` doesn't shrink the blob — you'd write out the right number of layer records but the wrong amount of pixel data.

**Approach:**
- Replace the blob dump with a per-layer / per-channel loop that uses `LayerInfo::channels[i].imageData` (each is already a `cloneOffset` into the blob).
- Each `channel.imageData->size()` is exactly the right number of bytes; copy them through.
- The order of `LayerInfo` in `layerList` (after edits) determines the order of the channel data block — perfect for delete/duplicate.

**Estimated size:** ~50 lines in `psdwrite.cpp`.

**Test plan:** add a `test_save_after_delete` that loads, removes layer N, saves, reloads, asserts layer count -1 and remaining layer pixels match.

### Phase 4c — extra data field re-serialization (enables rename, blend-mode change)

**Problem:** `LayerExtraData::rawBytes` is the raw bytes of the entire extra-data block (layer mask, blending range, Pascal name, additional info entries). Changing `lay.extraData.layerName` doesn't update `rawBytes`. Save would emit the stale name.

**Approach:**
- Add `writeLayerExtraDataFromFields(WriterBase&, const LayerExtraData&)` that re-serializes each field:
  - `LayerMask` (0 / 20 / 36 / 40 byte variants — store a "size" hint or detect from presence of `enclosing*` fields)
  - `LayerBlendingRange` (gray + per-channel)
  - Pascal-string `layerName` with 4-byte padding
  - Each `AdditionalLayerInfo`: 8BIM/8B64 + key + size + data (the inner `data` iterator can still be reused for entries we don't intend to modify, e.g. shmd, lsct)
- Add a per-layer flag `LayerExtraData::useRawBytes` (default true). When the user mutates a field, drop to false; `writeLayerRecord` picks the reconstruction path.
- For `luni` (Unicode name) records specifically, expose a setter that updates `layerNameUnicode` AND drops `rawBytes`-based emission.

**Estimated size:** ~300 lines + 5 tests for rename / blend-mode change / clipping toggle.

### Phase 4d — RLE encoder + new layer / pixel replacement

**Problem:** No way to construct new channel data. Existing layers' `channel.imageData` iterators point into the loaded file; we have no encoder that takes raw BGRA in and produces RLE-compressed channel bytes.

**Approach:**
- Implement a PackBits / RLE encoder (Photoshop's per-row variant with 16-bit row-length table).
- Add a `psd::PSDFile::set_layer_pixels(int idx, const uint8_t *bgra, int w, int h)` API that:
  1. Splits BGRA into B / G / R / A planes,
  2. RLE-compresses each plane,
  3. Replaces `channel.imageData` with a fresh in-memory `MemoryReader` over the compressed bytes.
- Add `psd::PSDFile::add_layer(...)` for fully-synthesized layers (caller supplies bbox, name, blend mode, BGRA pixels).
- Python: `p.replace_layer_image(idx, image_bytes, w, h)` and `p.add_layer(name, bbox, image_bytes, blend_mode=...)`.

**Estimated size:** ~500 lines (mostly encoder) + a fixture-based round-trip test (encode, then decode through `getLayerImage`, then compare with input).

### Phase 4e — new-from-scratch PSD construction

Once 4d lands, the user can do:

```python
p = psdparse.PSDFile.create_blank(width=1024, height=768, mode=psdparse.COLOR_MODE_RGB)
p.add_layer("background", bbox=(0, 0, 1024, 768), pixels=bg_bgra)
p.add_layer("character", bbox=(100, 100, 800, 700), pixels=char_bgra)
p.save("out.psd")
```

This is mostly a constructor that fills `Data` with a minimal-but-valid skeleton (default header, empty image resources, empty color mode data, sentinel `channelImageData`, etc.).

## Other future work

- 16-bit (`Lr16`) and 32-bit-float (`Lr32`) layer data: currently captured in `layerAndMaskTrailing` for round-trip but not exposed as decoded pixels.
- Layer mask: parse + re-emission for masks > 20 bytes (real mask, vector mask flag, density / feather).
- Image resources: most are currently passed through as raw bytes. Higher-level accessors for ICC profile, EXIF, thumbnail, version info, etc. would be nice for tools.
- CMYK / Lab / Indexed color extraction in `getLayerImage` — currently optimized for RGB.
- Linux / macOS testing. mmap path uses POSIX `mmap` but hasn't been built / tested there.
