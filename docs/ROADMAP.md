# psdparse Roadmap

For a feature-by-feature account of what is and isn't supported today, see
[SUPPORT.md](SUPPORT.md). This file tracks planned work.

## Current state (2026-08-02, v0.7.0)

- ✅ Pure C++17 parser (no Boost)
- ✅ mmap + StreamReader / Source abstraction
- ✅ Python bindings (pybind11)
- ✅ pytest regression suite (114 tests)
- ✅ Round-trip PSD save (byte-identical)
- ✅ Edit & save: structure / pixels / mask / parameters / effects / text / new-from-scratch (E1–E6, byte-exact re-serialization)
- ✅ UTF-8 path I/F (Win32 conversion internal only)

## Save: from round-trip to edit-and-save

The `save()` path started as round-trip-only (correct only when the loaded `Data` is unmodified). Structural editing is being added incrementally. The編集 API is split into phases E1–E6; each builds on the last.

- ✅ **E1 — parameter edits.** *Done 2026-08-02 (0.7.0).* Record-level fields are re-serialized from the struct, so `layer.opacity` / `layer.clipping` / `layer.visible` are writable properties and `layer.set_blend_mode("mul ")` / `layer.blend_mode_key` set the blend mode. No file-format work was needed — `writeLayerRecord` already emitted these from fields.
- ✅ **E2 — delete / move / duplicate / cross-file copy.** *Done 2026-08-02 (0.7.0).* `writeLayerInfo` now serializes channel data **per-layer/per-channel** (bounded `copyNFrom`) when the layer list is dirty, instead of dumping the original concatenated blob — so `PSDFile.delete_layer` / `move_layer` / `duplicate_layer` / `copy_layer_from(src, i)` all produce correct pixel data. A `Data::layersDirty` flag keeps unmodified files on the exact-blob path so byte-identical round-trip is preserved. Cross-file copy holds the source alive via pybind11 `keep_alive`. Also fixed a **pre-existing crash**: the group-linking loop in `processParsed` under-flowed its `parent` stack on unbalanced FOLDER/HIDDEN dividers (now guarded) — exposed by deleting one half of a divider pair. Validated against psd-tools (all edited files open + composite) — see `tests/test_edit.py`.

- ✅ **E4 — RLE encoder + pixel replacement + new image layers.** *Done 2026-08-02 (0.7.0).* A PackBits(RLE) encoder (`psdimage.cpp`, the exact inverse of `decodePackBits`) plus an owning `VectorReader` (a `MemoryReader` that keeps its bytes alive via `shared_ptr`) back two new APIs: `PSDFile.set_layer_pixels(i, bgra, w, h)` replaces a layer's channels, and `PSDFile.add_layer(name, l, t, bgra, w, h, blend, opacity)` builds a whole new RGBA layer — channels in `(-1,0,1,2)` order and a minimal extra-data block (empty mask/ranges + Pascal name + `luni` Unicode name + `lyid`). 8-bit RGB only. Encoder round-trip is pixel-exact across edge cases (1×1, constant, runs, 128/129/255 widths); the Unicode name (incl. emoji) and all output files were cross-checked against psd-tools (`composite()`), see `tests/test_edit_pixels.py`. Also surfaced (and documented) that saving over an mmap'd path returns `False` rather than corrupting.

- ✅ **E5 — new-from-scratch PSD construction.** *Done 2026-08-02 (0.7.0).* `PSDFile.create_blank(width, height, mode=RGB)` fills a minimal valid skeleton — header (v1, 3ch, 8-bit RGB), empty color-mode/resources, empty dirty layer list, and a white raw composite (`VectorReader`) — so you can `create_blank()` → `add_layer()` → `save()` with no source file. Validated empty and multi-layer, cross-checked with psd-tools `composite()` — see `tests/test_create.py`.

- ✅ **E3b — effect / descriptor value editing + Descriptor serializer.** *Done 2026-08-02 (0.7.0).* A full **Photoshop descriptor serializer** (`writeDescriptorBody`/`writeDescriptorItem` in `psdwrite.cpp`, the exact inverse of `psddesc`'s loader, incl. references) plus a `MemoryWriter`. `Descriptor` now records `keyOrder` so re-serialization reproduces the on-disk key order, and descriptor data is padded to 4 bytes — together these make round-trip **byte-exact** (verified: `set_effects(i, {})` then save yields a byte-identical file). `PSDFile.set_effects(i, changes)` / `set_layer_descriptor(i, key, changes)` merge a partial dict onto the parsed typed descriptor (only leaf values overwritten; classIDs/types/order preserved) and swap the re-serialized block into the extra data. `layer.descriptor_bytes(key)` exposes raw block bytes. Validated against psd-tools — see `tests/test_edit_effects.py`. **This serializer is the shared foundation E6 (text editing) needs.**
- ✅ **E3 — extra-data field edits (rename + mask + fill opacity).** *Done 2026-08-02 (0.7.0).* Editing of extra-data-resident fields, via a per-layer `LayerExtraData::useRawBytes=false` flag that routes save through `writeLayerExtraFromFields` (reconstruct from fields) instead of the raw-bytes passthrough:
  - **Rename** — `layer.name_unicode = "..."` / `set_layer_name(i, name)` (Pascal + `luni`).
  - **Fill opacity** — `layer.fill_opacity = ...` (the `iOpa` block; written as the 1-byte value + 3 filler to match Photoshop / psd-tools' `B3x` reader).
  - **Mask values** — `set_layer_mask(i, disabled=, density=, feather=, default_color=)`; a `LayerMask::edited` flag makes the mask sub-block re-serialize from fields (`serializeLayerMask`), matching `parseLayerMask` exactly (real section gated on size≥36, params on flags bit4).

  The layer mask & blending ranges are otherwise copied through byte-for-byte from `maskRaw`/`blendRaw` (captured at parse). Unmodified layers keep the exact-bytes path so round-trip identity is untouched. Learned the hard way that **layer-record tagged blocks use `padding=1` (no 4-byte alignment)** — only the *global* additional info and the Pascal name pad to 4. Validated with psd-tools reading back disabled/density/feather/bg/fill-opacity and no alignment warnings — see `tests/test_edit_name.py`, `tests/test_edit_mask.py`. **Still to do in E3:** mask *geometry* (rectangle) edits (effect value edits are done — see E3b above).

- 🟡 **E6 — text-layer content editing.** *Text content done 2026-08-02 (0.7.0).* A **byte-exact Adobe EngineData serializer** (`psdengine.cpp`, the inverse of the parser — replicating psd-tools/Photoshop formatting: tab indentation by depth, `%.8f` float trimming with `0.`→`.`, inline-vs-multiline arrays, `(BOM …)` string escaping, and `Node.keyOrder`/`isInt` to preserve dict order and int-vs-float). Verified byte-exact on all 6 sample text layers. `PSDFile.set_text(i, str)` rewrites `EngineDict/Editor/Text` + collapses run-length arrays to a single run, updates the `Txt ` descriptor string, and re-serializes the `TySh` block (reusing the E3b descriptor serializer) with the version/transform prefix and warp/bounds suffix preserved verbatim. psd-tools reads the new text (incl. emoji) with no warnings — see `tests/test_edit_text.py`. **Still to do in E6:** per-run style editing (font/size/colour per range) — currently `set_text` collapses to the first run's style.

- ✅ **Mask pixels & geometry editing.** *Done 2026-08-02 (0.7.0).* `PSDFile.set_layer_mask_pixels(i, gray, top, left, w, h)` RLE-encodes a grayscale buffer into the layer's user-mask channel (`-2`) and sets the mask rectangle (creating the mask if absent), so mask geometry is editable together with its pixels. Also fixed `set_layer_pixels` to **preserve** an existing mask channel instead of dropping it. Cross-checked mask pixels + rectangle with psd-tools — see `tests/test_edit_mask_pixels.py`.

**Remaining edit work** (E6 per-run text styling) is described below.

### Phase E3 (was 4c) — extra data field re-serialization (enables rename, blend-mode change)

**Problem:** `LayerExtraData::rawBytes` is the raw bytes of the entire extra-data block (layer mask, blending range, Pascal name, additional info entries). Changing `lay.extraData.layerName` doesn't update `rawBytes`. Save would emit the stale name.

**Approach:**
- Add `writeLayerExtraDataFromFields(WriterBase&, const LayerExtraData&)` that re-serializes each field:
  - `LayerMask` (0 / 20 / 36 / 40 byte variants — store a "size" hint or detect from presence of `enclosing*` fields)
  - `LayerBlendingRange` (gray + per-channel)
  - Pascal-string `layerName` with 4-byte padding
  - Each `AdditionalLayerInfo`: 8BIM/8B64 + key + size + data (the inner `data` iterator can still be reused for entries we don't intend to modify, e.g. shmd, lsct)
- Add a per-layer flag `LayerExtraData::useRawBytes` (default true). When the user mutates a field, drop to false; `writeLayerRecord` picks the reconstruction path.
- For `luni` (Unicode name) records specifically, expose a setter that updates `layerNameUnicode` AND drops `rawBytes`-based emission.

**Estimated size:** ~300 lines + tests for rename / mask edit. (Blend mode, opacity and clipping already land via E1's record-level setters, so E3 is specifically the *extra-data*-resident fields: Unicode name, mask geometry/params, fill opacity, effects.)

### Phase E4 (was 4d) — RLE encoder + new layer / pixel replacement

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

### Phase E5 (was 4e) — new-from-scratch PSD construction

Once E4 lands, the user can do:

```python
p = psdparse.PSDFile.create_blank(width=1024, height=768, mode=psdparse.COLOR_MODE_RGB)
p.add_layer("background", bbox=(0, 0, 1024, 768), pixels=bg_bgra)
p.add_layer("character", bbox=(100, 100, 800, 700), pixels=char_bgra)
p.save("out.psd")
```

This is mostly a constructor that fills `Data` with a minimal-but-valid skeleton (default header, empty image resources, empty color mode data, sentinel `channelImageData`, etc.).

### Phase E6 — text-layer editing

Editing text content/style means writing the `TySh` block back: re-serializing the type-tool **descriptor** (currently read-only in `psddesc.*`) and, harder, re-emitting the embedded Adobe **EngineData** mini-language (`psdengine.cpp` only parses it). Needs a Descriptor serializer + an EngineData writer. Deferred until the image-layer editing set (E3–E5) is complete, per the stated priority (image first, then text).

## Other future work

- ✅ **Text layer content extraction (`TySh` type-tool additional info).** *Done 2026-07-27.* `lay.text` returns `{"text", "orientation", "justification", "transform", "runs":[{"length","font","size_px","color","tracking","kerning","auto_kerning"}]}` (or `None` for non-text layers). Implementation: `psddesc` now reads `tdta` raw data (length-prefixed), `loadLayerTypeTool` (`psdlayer.cpp`) parses the `TySh` header + text descriptor, and `psdengine.cpp` parses the embedded Adobe *EngineData* mini-language (FontSet / StyleRun / ParagraphRun) into per-run styling. Validated against `tests/data/fontsample.psd` (multi-font/size/color, **vertical**, emoji, tracking) — see `tests/test_text.py`.

  **Deferred (need targeted sample PSDs, next turn):**
  - **Non-RGB `FillColor`** — only `/Type 1` (RGB) decoded today; grayscale-mode / CMYK-mode text needs a sample to confirm `/Type` + `/Values` layout.
  - **Warp text** — lives in the `TySh` *warp* descriptor (currently skipped, not in EngineData); needs samples with each warp style + non-zero bend/distortion.
  - **Area (paragraph) vs point text / text box bounds** and **text-on-path** — need samples.
  - **Leading / faux bold-italic / underline / strikethrough / paragraph indent+spacing** — keys exist in EngineData but are default-valued in the current sample, so per-run extraction can't be verified yet; needs a sample authored with non-default values.
- ✅ **Tier-1 reference metadata exposed to Python.** *Done 2026-08-02 (0.3.0).* Data the C++ core already parsed but Python couldn't reach is now bound: `layer.parent_index` (folder hierarchy), `layer.mask` (bbox / flags / real-mask dict), `layer.blending_ranges`, and document-level `PSDFile.guides` / `.slices` / `.layer_comps` / `.color_table`. All dict-shaped, matching the existing `layer.text` style. Validated against `config.psd` / `system.psd` (hierarchy, blend ranges, guides, slices) and a psd-tools-synthesized `masktest.psd` (mask bbox/flags cross-checked) — see `tests/test_metadata.py`.
- ✅ **Tier-2 generic Descriptor → dict bridge.** *Done 2026-08-02 (0.4.0).* `layer.effects` (`lfx2`), `layer.fill` (`SoCo`/`GdFl`/`PtFl`), `layer.info_keys`, and a generic `layer.descriptor(key, skip)` escape hatch route previously-skipped tagged blocks through the existing complete descriptor parser (`psddesc.*`). A `Descriptor → py::dict` converter in the binding (dynamic_cast dispatch; UnitFloat→`{value,unit}`, Enumerated→`{type,value}`, nested Descriptor→dict, List→list, tdta→bytes) means no per-feature decoders were needed. Decoding is lazy: each access clones the block's reader (`clone()`+`init()`) and re-parses, so `save()` round-trips remain byte-identical. Cross-checked value-for-value against psd-tools 1.17 on `config.psd`'s PatternOverlay (opacity/scale/angle/blend/pattern-name all matched) — see `tests/test_descriptors.py`. **Deferred:** typed high-level effect/fill objects, smart-object `SoLd`/`lnkD` (embedded-file extraction), and binary adjustment layers (`levl`/`curv`) all still need work — the raw descriptor dict is the current interface.
- ✅ **Image-resource raw bytes exposed.** *Done 2026-08-02 (0.5.0).* `PSDFile.image_resource(id)` / `.image_resource_ids` plus typed shortcuts `.icc_profile` (1039), `.exif` (1058), `.xmp` (1060, UTF-8 str) and `.thumbnail` (1036/1033 → dict with JPEG bytes + dimensions). Raw bytes only — decoding EXIF tags / rendering the thumbnail is left to the caller. EXIF and ICC bytes cross-checked byte-identical against psd-tools 1.17; thumbnail JPEG verified decodable with Pillow — see `tests/test_resources.py`. **Still raw-only / unexposed:** higher-level decode of these (parsed EXIF, ICC transform) and the structured `GlobalLayerMaskInfo`.
- ✅ **Mask parameters + color label + global mask + Lab/Duotone pixels.** *Done 2026-08-02 (0.6.0).* A batch of the remaining reference gaps: `layer.mask` now decodes the density/feather `MaskParameters` (`user_density`/`user_feather`/`vector_density`/`vector_feather`) and the real/user-mask section is byte-correct again (the old parser had an off-by-one that shifted the enclosing rect — no filler byte follows `flags`, and the real section is gated on size≥36, matching psd-tools). `layer.sheet_color` exposes the `lclr` layer-panel color label (index+name). `PSDFile.global_layer_mask` exposes the document overlay color/opacity/kind. `getLayerImage`/`getMergedImage` now render **Lab** (standard D65 CIELAB→sRGB approximation; Photoshop uses D50 so saturated colors differ slightly) and **Duotone** (as grayscale per the Adobe spec). The generic `descriptor()` escape hatch gained known default skips for `SoLd`/`vstk`/`vscg`/`vogk`/`CgEd`. Validated against psd-tools-authored fixtures (`maskparams.psd`: real+params mask & lclr; `labsample.psd`: neutral/red swatches; `duosample.psd` vs `graysample.psd`) — see `tests/test_mask_extras.py` and `tests/test_colormodes.py`. **Still open:** Multichannel pixels (no canonical RGB), smart-object embedded-file extraction (`lnkD`), and the SoLd/vector default skips remain unverified against a real smart-object sample.
- 16-bit (`Lr16`) and 32-bit-float (`Lr32`) layer data: currently captured in `layerAndMaskTrailing` for round-trip but not exposed as decoded pixels.
- Layer mask re-emission for masks > 20 bytes (the field-based save path; round-trip already preserves them via raw bytes).
- Multichannel color extraction in `getLayerImage` (no canonical RGB representation); smart-object embedded file (`lnkD`) extraction.
- Linux / macOS testing. mmap path uses POSIX `mmap` but hasn't been built / tested there.
