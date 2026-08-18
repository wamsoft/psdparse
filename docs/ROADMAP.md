# psdparse Roadmap

For a feature-by-feature account of what is and isn't supported today, see
[SUPPORT.md](SUPPORT.md). This file tracks planned work.

## Current state (2026-08-18, v0.11.0)

- ✅ Pure C++17 parser (no Boost)
- ✅ mmap + StreamReader / Source abstraction
- ✅ Python bindings (pybind11)
- ✅ pytest regression suite (201 tests)
- ✅ Python composite recipes (`examples/`) + `set_merged_image`; layer comps exposed (`layer.comp_states`)
- ✅ Round-trip PSD save (byte-identical)
- ✅ Edit & save: structure / pixels / mask / parameters / effects / text / new-from-scratch (E1–E6, byte-exact re-serialization)
- ✅ Text editing beyond E6 (0.9.0): rich-text run & paragraph rebuild, faux bold/italic/underline, font-by-name, justification, placement & text box
- ✅ Folder-aware layer moves + `parent_index` re-linking after every structural edit (0.9.0)
- ✅ UTF-8 path I/F (Win32 conversion internal only)

- ✅ Layer groups from scratch (0.11.0): `add_folder(name, from_index, count, closed=, blend_mode=, opacity=)` inserts the `</Layer group>` divider + folder pair around an existing contiguous run (or an empty folder with `count=0`), so a whole PSD — groups included — can be built or reorganised without Photoshop
- ✅ Python bindings for the whole editing surface — 0.10.0 closed the last gap (`set_rich_text`, `set_justification`, `text_fonts`, `font=` on `set_run_style`, `move_layer_sibling` / `move_layer_range` / `group_span`, `move_text_layer`, `text_transform` / `set_text_transform`, `text_bounds` / `set_text_bounds`)

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

  The layer mask & blending ranges are otherwise copied through byte-for-byte from `maskRaw`/`blendRaw` (captured at parse). Unmodified layers keep the exact-bytes path so round-trip identity is untouched. Learned the hard way that **layer-record tagged blocks use `padding=1` (no 4-byte alignment)** — only the *global* additional info and the Pascal name pad to 4. Validated with psd-tools reading back disabled/density/feather/bg/fill-opacity and no alignment warnings — see `tests/test_edit_name.py`, `tests/test_edit_mask.py`. ~~**Still to do in E3:** mask *geometry* (rectangle) edits~~ — closed the same day by `set_layer_mask_pixels` (see *Mask pixels & geometry editing* below); effect value edits are E3b above.

- ✅ **E6b — per-run text style editing.** *Done 2026-08-02 (0.7.0).* `PSDFile.set_run_style(i, run, size_px=/color=/tracking=/kerning=/bold=/italic=/underline=)` edits an existing style run's `StyleSheetData` values in the embedded EngineData (adding keys the run inherited), leaving text, run lengths and other runs untouched. Reuses the byte-exact EngineData serializer + the shared `editTextLayer` TySh flow. Validated with psd-tools — see `tests/test_edit_run_style.py`. ~~**Not covered:** changing a run's font by name (needs FontSet editing) or re-splitting text into new runs.~~ *Both closed in 0.9.0 — see E6c below (`set_run_style(font=…)` and `set_rich_text`).*

- ✅ **E6c — rich text, faux styles, justification, placement.** *Done 2026-08-14 (0.9.0).* The text-editing entry points moved from the Python binding down into the C++ library (`PSDFile::setLayerText*`, `setLayerRunStyle`, …) so any embedder gets them, and the remaining E6 gaps were closed on top:
  - **Rich text** — `editEngineDataRichText` / `PSDFile::setLayerRichText(i, text, runs, paragraphs)` replaces the body *and* the run / paragraph structure (`TextRunSpec` / `TextParagraphSpec`). Each run starts from the original first run as a template and overrides only the fields you set; length mismatches are absorbed by the last run; a trailing `\r` is appended as Photoshop expects.
  - **Font by name** — `RunStyleEdit::font` appends to `ResourceDict/FontSet` when the name isn't there and points `StyleSheetData/Font` at the index.
  - **Faux styles** — `FauxBold` / `FauxItalic` / `Underline` on `TextStyleRun` (read) and `RunStyleEdit` (write); the Python `layer.text["runs"]` dict gained `bold` / `italic` / `underline` so writes read back.
  - **Justification** — `editEngineDataJustification` / `setLayerJustification(i, paraIndex, j)` (`paraIndex < 0` = all paragraphs).
  - **Placement & text box** — `editTyShBlock` exposes the `TySh` prefix + descriptor; on top of it `get/setLayerTextTransform`, `moveTextLayer(i, dx, dy)` (transform `tx`/`ty` + the layer and mask rectangles, so the baked raster moves with the text) and `get/setLayerTextBounds` (the descriptor's `bounds`, in the transform's local coordinates — re-flow only applies to paragraph/box text).
  - `getLayerFonts(i, names)` lists the document's font set for UI pickers.

  *Python bindings (0.10.0, 2026-08-15):* `set_rich_text(i, text, runs, paragraphs)`, `set_justification(i, j, para_index=-1)`, `text_fonts(i)`, `font=` on `set_run_style`, `move_text_layer`, `text_transform` / `set_text_transform`, `text_bounds` / `set_text_bounds` — plus `move_layer_sibling` / `move_layer_range` / `group_span` for the folder moves. Runs and paragraphs cross the boundary as lists of dicts (`{"length": …, "size_px": …}`), lengths in UTF-16 code units. Covered by `tests/test_edit_rich_text.py`, `tests/test_edit_text_placement.py`, `tests/test_move_group.py` (39 tests, psd-tools cross-checked, `tests/data/textboxsample.psd` as the fixture).

  **Not covered:** leading, paragraph indent/spacing (keys exist in EngineData, still not extracted), warp, non-RGB `FillColor`.
- ✅ **E6 — text-layer content editing.** *Text content done 2026-08-02 (0.7.0); per-run style in E6b, the rest in E6c.* A **byte-exact Adobe EngineData serializer** (`psdengine.cpp`, the inverse of the parser — replicating psd-tools/Photoshop formatting: tab indentation by depth, `%.8f` float trimming with `0.`→`.`, inline-vs-multiline arrays, `(BOM …)` string escaping, and `Node.keyOrder`/`isInt` to preserve dict order and int-vs-float). Verified byte-exact on all 6 sample text layers. `PSDFile.set_text(i, str)` rewrites `EngineDict/Editor/Text` + collapses run-length arrays to a single run, updates the `Txt ` descriptor string, and re-serializes the `TySh` block (reusing the E3b descriptor serializer) with the version/transform prefix and warp/bounds suffix preserved verbatim. psd-tools reads the new text (incl. emoji) with no warnings — see `tests/test_edit_text.py`. ~~**Still to do in E6:** per-run style editing (font/size/colour per range)~~ — `set_text` still collapses to the first run's style by design; per-run editing is E6b and structural run rebuilds are E6c (`setLayerRichText`).

- ✅ **Mask pixels & geometry editing.** *Done 2026-08-02 (0.7.0).* `PSDFile.set_layer_mask_pixels(i, gray, top, left, w, h)` RLE-encodes a grayscale buffer into the layer's user-mask channel (`-2`) and sets the mask rectangle (creating the mask if absent), so mask geometry is editable together with its pixels. Also fixed `set_layer_pixels` to **preserve** an existing mask channel instead of dropping it. Cross-checked mask pixels + rectangle with psd-tools — see `tests/test_edit_mask_pixels.py`.

- ✅ **Folder-aware moves + hierarchy re-linking.** *Done 2026-08-14 (0.9.0).* `parent_index` used to keep the values `processParsed` computed at load time, so any structural edit left the tree stale. The linking pass is now `Data::relinkGroups()`, called at the end of `deleteLayer` / `moveLayer` / `duplicateLayer` / `copyLayerFrom` / `addLayer`. `Data::groupSpan(i)` returns the `[HIDDEN divider … FOLDER]` span a folder occupies (nested groups included), and on top of it `PSDFile::moveLayerRange(from, count, to)` moves a span, while `moveLayerSibling(i, up, &newIndex)` swaps a layer (or a whole folder) with its next sibling *at the same level*, never crossing into another folder. Verified on a 186-layer / 25-folder production PSD: span correctness for every folder, move-up-then-back restores the structure for all 135 layers, folder contents keep count and order, parents unchanged after a move, and the hierarchy survives save → reload.

The image- and text-layer editing suite (E1–E6) is feature-complete for the
common cases. Remaining niche gaps are noted per-phase above (leading /
paragraph spacing and warp in text; composite re-rendering; smart-object
embedded data), plus the Python binding debt listed under *Current state*.

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

- ✅ **`duplicate_layer` assigns a fresh `lyid`.** *Done 2026-08-12.* The
  duplicated layer used to copy the source's `lyid` (layer ID) verbatim, so the
  output PSD contained duplicate layer IDs — Photoshop itself assigns a *new*
  ID when duplicating. Downstream tools that use `lyid` as a persistent layer
  identity (e.g. elements_console's uitool reference resolution) then saw
  ambiguous matches. Fix (`psdfile.cpp`): `assignFreshLayerId` sets
  `layerId = max existing lyid + 1` and replaces (or appends) the `lyid`
  additional-info entry on the copy, flipping `useRawBytes=false` so save
  re-serializes it; applied in both `duplicateLayer` and `copyLayerFrom` (the
  latter picks an ID unique within the *destination* document). `add_layer`
  already picked max+1 — verified. Tests (`tests/test_edit.py`): duplicate →
  all `layer.layer_id` distinct → save → reload → still distinct; double
  duplicate cross-checked via psd-tools tagged blocks; `copy_layer_from`
  uniqueness in the destination.
- ✅ **Text layer content extraction (`TySh` type-tool additional info).** *Done 2026-07-27.* `lay.text` returns `{"text", "orientation", "justification", "transform", "runs":[{"length","font","size_px","color","tracking","kerning","auto_kerning"}]}` (or `None` for non-text layers). Implementation: `psddesc` now reads `tdta` raw data (length-prefixed), `loadLayerTypeTool` (`psdlayer.cpp`) parses the `TySh` header + text descriptor, and `psdengine.cpp` parses the embedded Adobe *EngineData* mini-language (FontSet / StyleRun / ParagraphRun) into per-run styling. Validated against `tests/data/fontsample.psd` (multi-font/size/color, **vertical**, emoji, tracking) — see `tests/test_text.py`.

  **Deferred (need targeted sample PSDs):**
  - **Non-RGB `FillColor`** — only `/Type 1` (RGB) decoded today; grayscale-mode / CMYK-mode text needs a sample to confirm `/Type` + `/Values` layout.
  - **Warp text** — lives in the `TySh` *warp* descriptor (currently skipped, not in EngineData); needs samples with each warp style + non-zero bend/distortion. Edits preserve the warp bytes verbatim.
  - ✅ **Text box bounds** — *done 2026-08-14 (0.9.0)*, `get/setLayerTextBounds` on the descriptor's `bounds` (`tests/data/textboxsample.psd`). **Text-on-path** still needs a sample.
  - ✅ **Faux bold / italic / underline** — *done 2026-08-14 (0.9.0)*, read (`text["runs"]`) and write (`set_run_style`). **Leading / strikethrough / paragraph indent+spacing** still unextracted — keys exist in EngineData but the current samples are default-valued.
- ✅ **Tier-1 reference metadata exposed to Python.** *Done 2026-08-02 (0.3.0).* Data the C++ core already parsed but Python couldn't reach is now bound: `layer.parent_index` (folder hierarchy), `layer.mask` (bbox / flags / real-mask dict), `layer.blending_ranges`, and document-level `PSDFile.guides` / `.slices` / `.layer_comps` / `.color_table`. All dict-shaped, matching the existing `layer.text` style. Validated against `config.psd` / `system.psd` (hierarchy, blend ranges, guides, slices) and a psd-tools-synthesized `masktest.psd` (mask bbox/flags cross-checked) — see `tests/test_metadata.py`.
- ✅ **Tier-2 generic Descriptor → dict bridge.** *Done 2026-08-02 (0.4.0).* `layer.effects` (`lfx2`), `layer.fill` (`SoCo`/`GdFl`/`PtFl`), `layer.info_keys`, and a generic `layer.descriptor(key, skip)` escape hatch route previously-skipped tagged blocks through the existing complete descriptor parser (`psddesc.*`). A `Descriptor → py::dict` converter in the binding (dynamic_cast dispatch; UnitFloat→`{value,unit}`, Enumerated→`{type,value}`, nested Descriptor→dict, List→list, tdta→bytes) means no per-feature decoders were needed. Decoding is lazy: each access clones the block's reader (`clone()`+`init()`) and re-parses, so `save()` round-trips remain byte-identical. Cross-checked value-for-value against psd-tools 1.17 on `config.psd`'s PatternOverlay (opacity/scale/angle/blend/pattern-name all matched) — see `tests/test_descriptors.py`. **Deferred:** typed high-level effect/fill objects, smart-object `SoLd`/`lnkD` (embedded-file extraction), and binary adjustment layers (`levl`/`curv`) all still need work — the raw descriptor dict is the current interface.
- ✅ **Image-resource raw bytes exposed.** *Done 2026-08-02 (0.5.0).* `PSDFile.image_resource(id)` / `.image_resource_ids` plus typed shortcuts `.icc_profile` (1039), `.exif` (1058), `.xmp` (1060, UTF-8 str) and `.thumbnail` (1036/1033 → dict with JPEG bytes + dimensions). Raw bytes only — decoding EXIF tags / rendering the thumbnail is left to the caller. EXIF and ICC bytes cross-checked byte-identical against psd-tools 1.17; thumbnail JPEG verified decodable with Pillow — see `tests/test_resources.py`. **Still raw-only / unexposed:** higher-level decode of these (parsed EXIF, ICC transform) and the structured `GlobalLayerMaskInfo`.
- ✅ **Mask parameters + color label + global mask + Lab/Duotone pixels.** *Done 2026-08-02 (0.6.0).* A batch of the remaining reference gaps: `layer.mask` now decodes the density/feather `MaskParameters` (`user_density`/`user_feather`/`vector_density`/`vector_feather`) and the real/user-mask section is byte-correct again (the old parser had an off-by-one that shifted the enclosing rect — no filler byte follows `flags`, and the real section is gated on size≥36, matching psd-tools). `layer.sheet_color` exposes the `lclr` layer-panel color label (index+name). `PSDFile.global_layer_mask` exposes the document overlay color/opacity/kind. `getLayerImage`/`getMergedImage` now render **Lab** (standard D65 CIELAB→sRGB approximation; Photoshop uses D50 so saturated colors differ slightly) and **Duotone** (as grayscale per the Adobe spec). The generic `descriptor()` escape hatch gained known default skips for `SoLd`/`vstk`/`vscg`/`vogk`/`CgEd`. Validated against psd-tools-authored fixtures (`maskparams.psd`: real+params mask & lclr; `labsample.psd`: neutral/red swatches; `duosample.psd` vs `graysample.psd`) — see `tests/test_mask_extras.py` and `tests/test_colormodes.py`. **Still open:** Multichannel pixels (no canonical RGB), smart-object embedded-file extraction (`lnkD`), and the SoLd/vector default skips remain unverified against a real smart-object sample.
- 16-bit (`Lr16`) and 32-bit-float (`Lr32`) layer data: currently captured in `layerAndMaskTrailing` for round-trip but not exposed as decoded pixels.
- Layer mask re-emission for masks > 20 bytes (the field-based save path; round-trip already preserves them via raw bytes).
- Multichannel color extraction in `getLayerImage` (no canonical RGB representation); smart-object embedded file (`lnkD`) extraction.
- Linux / macOS testing. mmap path uses POSIX `mmap` but hasn't been built / tested there.
