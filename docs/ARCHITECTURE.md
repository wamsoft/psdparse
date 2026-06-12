# psdparse Architecture

## Goals (in priority order)

1. **Lazy I/O.** Loading a PSD must not require reading the full file into memory. Only structural metadata (a few hundred KB) is touched during parse. Pixel data is paged in when the user asks for a specific layer.
2. **Backend-agnostic.** The parser and the image decoder see one I/O abstraction (`IteratorBase`). mmap and arbitrary seekable streams (std::istream, kirikiri `iTJSBinaryStream`, …) all plug into the same code path.
3. **Round-trip save.** `load(p) -> save(q)` produces a byte-identical PSD. We achieve this by retaining raw-bytes iterators for blocks whose round-trip re-serialization would be painful (layer extra data, global mask info, trailing additional info).
4. **No Boost. No tp_stub.** Plain C++17 + zlib only.

## Read path

```
IteratorBase             (psdbase.h, pure virtual)
├── MemoryReader         (psdparse.h)
│     base_, [start_, end_), pos_   ← all just offsets into a const uint8_t*
│     Used for: mmap'd files (PSDFile::load) and contiguous buffers (loadFromMemory)
│
└── StreamReader         (psdparse.h)
      shared_ptr<Source> + 4KB cache + [start_, end_), pos_
      Used for: any seekable stream
      └── Source (pure virtual, nested in StreamReader)
            ├── IStreamSource              ... std::istream wrapper (psdfile.cpp)
            └── (your backend)             ... e.g. iTJSBinaryStream wrapper
```

`IteratorBase` exposes a tiny interface:

```cpp
virtual int  getCh();
virtual int  getData(void *buffer, int n);
virtual int16_t getInt16(bool convToNative=true);
virtual int32_t getInt32(bool convToNative=true);
virtual int64_t getInt64(bool convToNative=true);
virtual void getUnicodeString(u16str &out, bool convToNative=true);

virtual void advance(int n);
virtual void init();          // reset to start of this sub-range
virtual int  size();          // range length
virtual int  rest();          // remaining bytes from pos
virtual bool eoi();

virtual IteratorBase *clone();                       // same pos, same range
virtual IteratorBase *cloneOffset(int offset);       // pos+offset, end inherited
virtual IteratorBase *cloneRange(int offset, int len); // [pos+offset, pos+offset+len)
```

The parser only ever sees `IteratorBase &`. There is no virtual function for "switch backend" because the choice of backend is fixed at construction time and forwarded via the `clone*` operations.

### `cloneRange` is load-bearing

Size-prefixed blocks (image resource entries, layer extra data, global mask info, …) must be parsed inside an `IteratorBase` that is **strictly bounded** to the declared size. Otherwise a corrupt `dataSize` can drive the parser past block boundaries — in the worst case, into an infinite `push_back` loop that allocates gigabytes before being noticed.

The `SubBlock` RAII helper in `psdparse.cpp` wraps this pattern:

```cpp
SubBlock blk(outerReader, declaredSize);  // bounds sub-reader to declaredSize
parseInnerStuff(blk.reader());            // sub-parser cannot escape
// dtor: outerReader.advance(declaredSize) regardless of what sub consumed
```

### Forward-progress guards

`parseImageResources`, `parseLayerExtraData`, and similar loops must verify that each iteration advances the reader:

```cpp
while (r.rest() >= MIN_ENTRY) {
    int posBefore = r.size() - r.rest();
    if (!parseOneEntry(r, ...)) break;
    int posAfter = r.size() - r.rest();
    if (posAfter <= posBefore) break;   // garbage entry; bail
}
```

This is the second line of defense for corrupt input.

## Lifetime: who owns the bytes?

The parser stores `IteratorBase*` clones into `psd::Data` fields (`channelImageData`, `imageData`, per-resource `data`, per-layer-channel `imageData`, …). Each clone:

- For `MemoryReader`: holds a `const uint8_t*` into the underlying buffer (mmap or `ownedBuffer_`). The buffer is owned by `PSDFile` via `mapping_` or `ownedBuffer_`.
- For `StreamReader`: holds a `std::shared_ptr<Source>`. Last clone dies → `Source` dies → backend stream is closed.

`PSDFile::clearData()` drops the iterators in well-defined order. The mmap is unmapped, owned buffer is swapped to empty, owned `istream` is reset. The destructor chains through `~Data` → `clearData()` (virtual; the explicit call in `~PSD()` is intentional and **must not** be removed).

## Write path

`writePSD(WriterBase&, const Data&)` walks the same five PSD top-level sections:

1. **File header** (26 bytes): re-serialized from `data.header` fields.
2. **Color mode data**: copies `data.colorModeIterator` bytes back.
3. **Image resources**: re-emits each resource's 8BIM header / Pascal name / size, then dumps `res.data` iterator bytes.
4. **Layer and mask info**: re-emits the layer info subsection header + per-record metadata, then dumps `data.channelImageData` (all channel data for all layers, in one blob), then dumps `data.globalLayerMaskInfoRaw` and `data.layerAndMaskTrailing` raw bytes for high-fidelity.
5. **Image data**: dumps `data.imageData` (composite image, including compression word).

### patch-back size headers

Variable-length sections start with a 4-byte size field. The writer uses a placeholder-then-seek-back pattern:

```cpp
int64_t sizePos = w.tell();
w.putUint32BE(0);             // placeholder
int64_t bodyStart = w.tell();
writeBody(w);
int64_t bodyEnd = w.tell();
w.seek(sizePos);              // back-patch
w.putUint32BE(bodyEnd - bodyStart);
w.seek(bodyEnd);
```

`WriterBase` requires `tell()` and `seek()` (pure virtual). `FileWriter` implements them with `_ftelli64` / `_fseeki64`.

### Raw-bytes iterators for round-trip fidelity

Re-serializing the full extra-data block (layer mask: 0 / 20 / 36 / 40 bytes with conditional fields; layer blending ranges; Pascal name with 4-byte padding; nested additional layer info entries) is tedious and error-prone. Instead, the parser captures the entire extra-data block as a raw IteratorBase clone alongside the parsed fields:

```cpp
// psdparse.cpp parseLayerRecord:
uint32_t extraSize = r.getInt32(true);
if (extraSize > 0) lay.extraData.rawBytes = r.cloneRange(0, extraSize);
SubBlock blk(r, extraSize);
if (extraSize > 0) parseLayerExtraData(blk.reader(), lay.extraData);
```

`writeLayerRecord` dumps `lay.extraData.rawBytes` directly. The parsed `LayerMask`, `LayerBlendingRange`, etc. fields are used for read access (`getLayerImage` etc.) but ignored on save.

Same trick for `Data::globalLayerMaskInfoRaw` and `Data::layerAndMaskTrailing`. The latter is the unparsed Lr16/Lr32 secondary layer info — capturing it added 19KB of fidelity to one of the test fixtures.

### Why this matters

The round-trip guarantee assumes the user didn't modify the loaded data. Adding/removing layers or changing names breaks it because:

- `channelImageData` is the **concatenation** of all channel bytes for all layers. Deleting a `LayerInfo` from `layerList` doesn't shrink this blob.
- `LayerExtraData::rawBytes` doesn't reflect a mutated layer name. Save would write stale bytes.

The roadmap for supporting structural edits is in [ROADMAP.md](ROADMAP.md). Briefly: per-channel save, then field-based extra-data emission, then an RLE encoder for new pixel data.

## File / class index

| File | Role |
|---|---|
| `psdparse/psdbase.h` | Endian macros, type atoms (`u16str`), `IteratorBase`, `utf8ToWide` |
| `psdparse/psddata.h` | `Header`, `LayerInfo`, `ChannelInfo`, `ImageResourceInfo`, `LayerExtraData`, `Data` |
| `psdparse/psddesc.h` | Photoshop Descriptor data |
| `psdparse/psdparse.h` | `MemoryReader`, `StreamReader` (+ nested `Source`), `parsePSD` decl |
| `psdparse/psdparse.cpp` | The parser. `SubBlock` RAII, hand-rolled binary reading, `processParsed` post-parse fixup |
| `psdparse/psdfile.h` | `PSDFile` (load/save public API) |
| `psdparse/psdfile.cpp` | `PSDFile` impl, Win32 mmap pimpl, `IStreamSource` adapter |
| `psdparse/psdimage.cpp` | Per-channel and merged-image decoders (RLE / zip / raw) |
| `psdparse/psdwrite.h` | `WriterBase`, `FileWriter`, `writePSD` decl |
| `psdparse/psdwrite.cpp` | Writer implementation with patch-back size handling |
| `psdparse/psdresource.cpp` | Image resource handlers (slices, grids, color tables, layer comps) |
| `psdparse/psdlayer.cpp` | Layer-level additional info handlers (luni, lsct, lyid, …) |
| `psdparse/psddesc.cpp` | Descriptor parser |
| `psdparse/bmp.cpp` | BMP scratch-buffer helper |
| `psdparse/psd_cli.cpp` | Smoke-test CLI |
| `python/psdparse_module.cpp` | pybind11 bindings (PSDFile, LayerInfo, Header, enums) |
