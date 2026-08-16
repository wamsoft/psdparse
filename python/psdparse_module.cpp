// psdparse Python bindings (minimal)
//
// Exposes PSDFile / LayerInfo / Header just enough to load a PSD, enumerate
// layers, and pull raw BGRA pixels into Python bytes objects so tests can
// hash them or hand them to PIL / numpy.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "psdfile.h"
#include "psdparse.h"
#include "psddesc.h"
#include "psdwrite.h"
#include "psdengine.h"

#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace {

// std::u16string (PSD's UTF-16BE on-disk -> host-order UTF-16 in memory)
// to Python str. UTF-16 code units transfer cleanly via py::str(u16string).
py::object u16ToStr(const psd::u16str &s) {
  return py::cast(s);
}

// Wrap getMergedImage / getLayerImage results in a py::bytes (BGRA, 4 bytes/px).
py::bytes mergedImage(psd::PSDFile &self) {
  if (!self.isLoaded) throw std::runtime_error("PSD not loaded");
  if (!self.imageData) throw std::runtime_error("no merged image stored in this PSD");
  size_t n = (size_t)self.header.width * (size_t)self.header.height * 4;
  std::string buf(n, '\0');
  self.getMergedImage(buf.data(), psd::BGRA_LE, 0);
  return py::bytes(buf);
}

// Text-layer info ('TySh') as a dict, or None for non-text layers.
py::object layerText(const psd::LayerInfo &l) {
  if (!l.textData.present) return py::none();
  const psd::TextLayerData &t = l.textData;
  py::dict d;
  d["text"] = py::cast(t.text);            // str (\r line breaks, as authored)
  d["orientation"] = t.orientation;        // "horizontal" / "vertical"
  d["justification"] = t.justification;    // 0=left 1=right 2=center (first paragraph)
  py::list tf;
  for (int i = 0; i < 6; i++) tf.append(t.transform[i]);
  d["transform"] = tf;                     // affine xx,xy,yx,yy,tx,ty
  py::list runs;
  for (const auto &r : t.runs) {
    py::dict rd;
    rd["length"]       = r.length;         // UTF-16 code units covered by this run
    rd["font"]         = py::cast(r.font); // resolved font-set name
    rd["size_px"]      = r.fontSize;       // px (文書解像度で換算済み; 継承分は pt×dpi/72)
    rd["tracking"]     = r.tracking;       // 1/1000 em
    rd["kerning"]      = r.kerning;        // manual kerning
    rd["auto_kerning"] = r.autoKerning;    // metrics/optical kerning on
    rd["bold"]         = r.bold;           // FauxBold
    rd["italic"]       = r.italic;         // FauxItalic
    rd["underline"]    = r.underline;
    if (r.hasColor)
      rd["color"] = py::make_tuple(r.color[0], r.color[1], r.color[2], r.color[3]); // RGBA 0..1
    else
      rd["color"] = py::none();
    runs.append(rd);
  }
  d["runs"] = runs;
  py::list paras;
  for (const auto &p : t.paragraphs) {
    py::dict pd;
    pd["length"]        = p.length;          // UTF-16 code units
    pd["justification"] = p.justification;   // 0=left 1=right 2=center
    paras.append(pd);
  }
  d["paragraphs"] = paras;                    // 段落別行揃え (段落=改行区切り)
  return std::move(d);
}

// Layer mask ('layer mask / adjustment layer data') as a dict, or None when
// the layer carries no mask.
py::object layerMask(const psd::LayerInfo &l) {
  const psd::LayerMask &m = l.extraData.layerMask;
  if (!m.present) return py::none();
  py::dict d;
  d["top"]    = m.top;
  d["left"]   = m.left;
  d["bottom"] = m.bottom;
  d["right"]  = m.right;
  d["width"]  = m.width;
  d["height"] = m.height;
  d["default_color"] = m.defaultColor;   // 0..255
  d["flags"]      = m.flags;              // raw flag byte
  d["relative"]   = (bool)(m.flags & 1);  // position relative to layer
  d["disabled"]   = (bool)(m.flags & 2);  // mask disabled
  d["inverted"]   = (bool)(m.flags & 4);  // invert (obsolete)
  d["from_render"] = (bool)(m.flags & 8); // mask from rendering other data
  d["has_parameters"] = (bool)(m.flags & 16); // density/feather block present
  // マスクパラメータ (density 0..255 / feather px)。 未指定フィールドは None。
  d["user_density"]   = (m.userMaskDensity >= 0) ? py::object(py::cast(m.userMaskDensity))
                                                 : py::object(py::none());
  d["user_feather"]   = m.hasUserFeather   ? py::object(py::cast(m.userMaskFeather))
                                           : py::object(py::none());
  d["vector_density"] = (m.vectorMaskDensity >= 0) ? py::object(py::cast(m.vectorMaskDensity))
                                                   : py::object(py::none());
  d["vector_feather"] = m.hasVectorFeather ? py::object(py::cast(m.vectorMaskFeather))
                                           : py::object(py::none());
  if (m.hasReal) {                         // real/user mask (block size >= 36)
    py::dict rd;
    rd["flags"]      = m.realFlags;
    rd["background"] = m.realUserMaskBackground;
    rd["top"]    = m.enclosingTop;
    rd["left"]   = m.enclosingLeft;
    rd["bottom"] = m.enclosingBottom;
    rd["right"]  = m.enclosingRight;
    d["real"] = rd;
  } else {
    d["real"] = py::none();
  }
  return std::move(d);
}

// Layer blending ranges as a dict, or None when absent. source/dest are the
// raw 32-bit packed values (each holds two 16-bit black/white sub-ranges).
py::object layerBlendingRanges(const psd::LayerInfo &l) {
  const psd::LayerBlendingRange &b = l.extraData.layerBlendingRange;
  if (!b.present) return py::none();
  py::dict d;
  d["gray"] = py::make_tuple(b.grayBlendSource, b.grayBlendDest);
  py::list ch;
  for (const auto &c : b.channels)
    ch.append(py::make_tuple(c.source, c.dest));
  d["channels"] = ch;
  return std::move(d);
}

// Sheet (layer-panel) color label from the 'lclr' block: 2-byte index + 6
// padding bytes. Returns {"index", "name"} or None when the layer has no
// label (index 0 / "none" is still reported so callers can distinguish
// "explicitly none" from "no lclr block"). Names match Photoshop's order.
py::object layerSheetColor(const psd::LayerInfo &l) {
  static const char *kNames[] = {
    "none", "red", "orange", "yellow", "green", "blue", "violet", "gray",
    "seafoam", "indigo", "magenta", "fuschia",
  };
  for (const auto &a : l.extraData.additionalLayers) {
    if (a.key != 'lclr' || !a.data) continue;
    psd::IteratorBase *rd = a.data->clone();
    rd->init();
    int idx = (uint16_t)rd->getInt16(true);
    delete rd;
    py::dict d;
    d["index"] = idx;
    d["name"]  = (idx >= 0 && idx < (int)(sizeof(kNames) / sizeof(kNames[0])))
                 ? kNames[idx] : "unknown";
    return std::move(d);
  }
  return py::none();
}

// Per-layer layer-comp state: {comp_id: {"enabled", "offset_x", "offset_y"}}.
// Empty dict when the layer participates in no comps. `enabled` drives which
// layers are shown for a given document layer comp (PSDFile.layer_comps).
py::dict layerCompStates(const psd::LayerInfo &l) {
  py::dict out;
  for (const auto &kv : l.layerComps) {
    const psd::LayerCompInfo &ci = kv.second;
    py::dict s;
    s["enabled"]  = ci.isEnabled;
    s["offset_x"] = ci.offsetX;
    s["offset_y"] = ci.offsetY;
    out[py::int_(kv.first)] = s;
  }
  return out;
}

// Strip a single trailing NUL from a u16 string (Photoshop stores some names
// NUL-terminated in the count).
psd::u16str stripNul(const psd::u16str &s) {
  if (!s.empty() && s.back() == u'\0') return s.substr(0, s.size() - 1);
  return s;
}

// Grid & guides image resource (1032) as a dict, or None when the PSD has none.
py::object psdGuides(psd::PSDFile &self) {
  const psd::GridGuideResource &g = self.gridGuide;
  if (!g.isEnabled) return py::none();
  py::dict d;
  d["horizontal_grid"] = g.horizontalGrid;
  d["vertical_grid"]   = g.verticalGrid;
  py::list gl;
  for (const auto &gi : g.guides) {
    py::dict gd;
    gd["location"]  = gi.location;   // 1/32 px from origin
    gd["direction"] = (gi.direction == psd::GUIDE_DIR_VERTICAL) ? "vertical"
                                                                : "horizontal";
    gl.append(gd);
  }
  d["guides"] = gl;
  return std::move(d);
}

// Slices image resource (1050, v6) as a dict, or None when absent.
py::object psdSlices(psd::PSDFile &self) {
  const psd::SliceResource &s = self.slice;
  if (!s.isEnabled) return py::none();
  py::dict d;
  d["group_name"] = py::cast(s.groupName);
  py::dict bb;
  bb["left"]   = s.boundingLeft;
  bb["top"]    = s.boundingTop;
  bb["right"]  = s.boundingRight;
  bb["bottom"] = s.boundingBottom;
  d["bounding"] = bb;
  py::list items;
  for (const auto &it : s.slices) {
    py::dict id;
    id["id"]       = it.id;
    id["group_id"] = it.groupId;
    id["origin"]   = it.origin;
    id["associated_layer_id"] = it.associatedLayerId;
    id["name"]     = py::cast(it.name);
    id["type"]     = it.type;
    id["left"]     = it.left;
    id["top"]      = it.top;
    id["right"]    = it.right;
    id["bottom"]   = it.bottom;
    id["url"]      = py::cast(it.url);
    id["target"]   = py::cast(it.target);
    id["message"]  = py::cast(it.message);
    id["alt_tag"]  = py::cast(it.altTag);
    id["cell_text"] = py::cast(it.cellText);
    id["is_cell_text_html"] = it.isCellTextHtml;
    id["horizontal_align"]  = it.horizontalAlign;
    id["vertical_align"]    = it.verticalAlign;
    id["color"] = py::make_tuple(it.colorR, it.colorG, it.colorB, it.colorA);
    items.append(id);
  }
  d["slices"] = items;
  return std::move(d);
}

// Layer comps image resource (1065) as a list of dicts (empty list if none).
py::list psdLayerComps(psd::PSDFile &self) {
  py::list out;
  for (const auto &c : self.layerComps) {
    py::dict d;
    d["id"]      = c.id;
    d["name"]    = py::cast(stripNul(c.name));
    d["comment"] = py::cast(stripNul(c.comment));
    d["record_visibility"] = c.isRecordVisibility;
    d["record_position"]   = c.isRecordPosition;
    d["record_appearance"] = c.isRecordAppearance;
    out.append(d);
  }
  return out;
}

// Indexed-color palette (from color mode data) as a dict, or None for
// non-indexed PSDs.
py::object psdColorTable(psd::PSDFile &self) {
  const psd::ColorTable &t = self.colorTable;
  if (t.colors.empty()) return py::none();
  py::dict d;
  d["valid_count"]        = t.validCount;
  d["transparency_index"] = t.transparencyIndex;
  py::list cols;
  for (const auto &c : t.colors)
    cols.append(py::make_tuple(c.r, c.g, c.b, c.a));
  d["colors"] = cols;
  return std::move(d);
}

// Global layer mask info (the document-level overlay color used to display
// masks) as a dict, or None when the block is empty/absent.
py::object psdGlobalLayerMask(psd::PSDFile &self) {
  const psd::GlobalLayerMaskInfo &g = self.globalLayerMaskInfo;
  if (!g.present) return py::none();
  py::dict d;
  d["overlay_color_space"] = g.overlayColorSpace;
  d["color"]   = py::make_tuple(g.color1, g.color2, g.color3, g.color4);
  d["opacity"] = g.opacity;   // 0..100
  d["kind"]    = g.kind;      // 0=inverted / 1=all-masks / 128=per-layer
  return std::move(d);
}

// -------------------------------------------------------------------------
// Generic Descriptor -> Python bridge (Tier 2).
//
// Photoshop stores layer effects (lfx2), fill layers (SoCo/GdFl/PtFl) and
// several other tagged blocks as its generic OSType "descriptor" tree. The
// C++ core already has a complete descriptor parser (psddesc.*); these helpers
// convert a parsed Descriptor into nested Python dicts/lists so the previously
// skipped blocks become readable without per-feature decoders.
// -------------------------------------------------------------------------

const char *descUnitName(psd::DescriptorUnit u) {
  switch (u) {
  case psd::UNIT_POINTS:      return "points";
  case psd::UNIT_MILLIMETERS: return "millimeters";
  case psd::UNIT_ANGLE:       return "angle";
  case psd::UNIT_DENSITY:     return "density";
  case psd::UNIT_DISTANCE:    return "distance";
  case psd::UNIT_NONE:        return "none";
  case psd::UNIT_PERCENT:     return "percent";
  case psd::UNIT_PIXELS:      return "pixels";
  default:                    return "unknown";
  }
}

py::dict descToPy(psd::Descriptor *d);

// Dispatch a single descriptor item to a Python value. Uses dynamic_cast
// rather than the `type` field because DescriptorReference and
// DescriptorRawData share the same type tag ('tdta').
py::object descItemToPy(psd::DescriptorItem *it) {
  if (!it) return py::none();
  if (auto *x = dynamic_cast<psd::DescriptorInteger*>(it))  return py::cast(x->val);
  if (auto *x = dynamic_cast<psd::DescriptorDouble*>(it))   return py::cast(x->val);
  if (auto *x = dynamic_cast<psd::DescriptorBoolean*>(it))  return py::cast(x->val);
  if (auto *x = dynamic_cast<psd::DescriptorString*>(it))   return py::cast(x->val); // u16str -> str
  if (auto *x = dynamic_cast<psd::DescriptorUnitFloat*>(it)) {
    py::dict u; u["value"] = x->val; u["unit"] = descUnitName(x->unit);
    return std::move(u);
  }
  if (auto *x = dynamic_cast<psd::DescriptorEnumerated*>(it)) {
    py::dict e; e["type"] = x->typeId; e["value"] = x->enumId;
    return std::move(e);
  }
  if (auto *x = dynamic_cast<psd::DescriptorList*>(it)) {
    py::list out;
    for (auto *item : x->items) out.append(descItemToPy(item));
    return std::move(out);
  }
  if (auto *x = dynamic_cast<psd::Descriptor*>(it))        return descToPy(x);
  if (auto *x = dynamic_cast<psd::DescriptorRawData*>(it)) return py::bytes(x->bytes);
  if (auto *x = dynamic_cast<psd::DescriptorClass*>(it))   return py::cast(x->classId);
  if (auto *x = dynamic_cast<psd::DescriptorAlias*>(it))   return py::cast(x->alias);
  // DescriptorReference and anything unrecognized -> None.
  return py::none();
}

py::dict descToPy(psd::Descriptor *d) {
  py::dict out;
  for (const auto &kv : d->itemMap)          // keys are raw 4cc (may end in space)
    out[py::str(kv.first)] = descItemToPy(kv.second);
  return out;
}

// Locate an additional-layer-info entry by 4cc key, parse its bytes as a
// descriptor (after skipping `skip` version-prefix bytes) and return a dict,
// or None when the key is absent / unparseable.
py::object keyDescriptor(const psd::LayerInfo &l, int key, int skip) {
  for (const auto &a : l.extraData.additionalLayers) {
    if (a.key != key || !a.data) continue;
    psd::IteratorBase *rd = a.data->clone();
    rd->init();                    // rewind to the start of this key's data
    if (skip > 0) rd->advance(skip);
    psd::Descriptor desc;
    desc.load(rd);                 // partial parse still leaves valid items
    delete rd;
    if (desc.itemMap.empty()) return py::none();
    return descToPy(&desc);
  }
  return py::none();
}

// Object-based layer effects ('lfx2'): objVer(4) + descVer(4) then descriptor.
py::object layerEffects(const psd::LayerInfo &l) {
  return keyDescriptor(l, 'lfx2', 8);
}

// Fill-layer content ('SoCo' solid / 'GdFl' gradient / 'PtFl' pattern):
// version(4) then descriptor. Returns {"type": ..., "data": {...}} or None.
py::object layerFill(const psd::LayerInfo &l) {
  const struct { int key; const char *type; } tbl[] = {
    {'SoCo', "solid"}, {'GdFl', "gradient"}, {'PtFl', "pattern"},
  };
  for (const auto &e : tbl) {
    py::object d = keyDescriptor(l, e.key, 4);
    if (!d.is_none()) {
      py::dict out;
      out["type"] = e.type;
      out["data"] = d;
      return std::move(out);
    }
  }
  return py::none();
}

// List the 4cc keys of all additional-layer-info blocks present on a layer.
py::list layerInfoKeys(const psd::LayerInfo &l) {
  py::list out;
  for (const auto &a : l.extraData.additionalLayers) {
    char s[4] = { (char)((a.key >> 24) & 0xff), (char)((a.key >> 16) & 0xff),
                  (char)((a.key >> 8) & 0xff),  (char)(a.key & 0xff) };
    out.append(py::str(s, 4));
  }
  return out;
}

// Generic escape hatch: parse an arbitrary additional-info key as a descriptor.
// `skip` defaults (-1) to the known version-prefix length for well-known keys,
// or 0 otherwise.
py::object layerDescriptor(const psd::LayerInfo &l, const std::string &keyStr, int skip) {
  if (keyStr.size() != 4)
    throw std::invalid_argument("key must be a 4-character string");
  int key = ((int)(uint8_t)keyStr[0] << 24) | ((int)(uint8_t)keyStr[1] << 16) |
            ((int)(uint8_t)keyStr[2] << 8)  |  (int)(uint8_t)keyStr[3];
  if (skip < 0) {
    switch (key) {
    case 'lfx2':                             skip = 8;  break;  // objVer + descVer
    case 'SoCo': case 'GdFl': case 'PtFl':   skip = 4;  break;  // descVer
    case 'SoLd': case 'SoLE':                skip = 12; break;  // 'soLD' + ver + descVer
    case 'vstk': case 'CgEd':                skip = 4;  break;  // descVer
    case 'vscg':                             skip = 8;  break;  // key + ver
    case 'vogk':                             skip = 8;  break;  // ver + dataVer
    default:                                 skip = 0;  break;
    }
  }
  return keyDescriptor(l, key, skip);
}

// -------------------------------------------------------------------------
// Descriptor editing: merge a (partial) Python dict onto a parsed Descriptor.
//
// To edit effect (lfx2) / fill values without the lossy dict->descriptor
// round-trip, we keep the parsed typed Descriptor and only overwrite the leaf
// values present in the changes dict; structure, classIDs and types are kept.
// Unknown keys are ignored. Then the descriptor is re-serialized byte-for-byte
// (except the changed leaves) and swapped into the layer's extra data.
// -------------------------------------------------------------------------

void mergeValueIntoItem(psd::DescriptorItem *item, py::handle val);

void mergeDictIntoDescriptor(psd::Descriptor *d, const py::dict &changes) {
  for (auto kv : changes) {
    std::string key = py::str(kv.first);
    auto it = d->itemMap.find(key);
    if (it == d->itemMap.end()) continue;   // only edit existing keys
    mergeValueIntoItem(it->second, kv.second);
  }
}

void mergeValueIntoItem(psd::DescriptorItem *item, py::handle val) {
  using namespace psd;
  if (auto *x = dynamic_cast<DescriptorInteger*>(item)) {
    if (py::isinstance<py::int_>(val) && !py::isinstance<py::bool_>(val)) x->val = val.cast<int32_t>();
  } else if (auto *x = dynamic_cast<DescriptorDouble*>(item)) {
    if (py::isinstance<py::float_>(val) || py::isinstance<py::int_>(val)) x->val = val.cast<double>();
  } else if (auto *x = dynamic_cast<DescriptorBoolean*>(item)) {
    if (py::isinstance<py::bool_>(val)) x->val = val.cast<bool>();
  } else if (auto *x = dynamic_cast<DescriptorString*>(item)) {
    if (py::isinstance<py::str>(val)) x->val = psd::utf8ToU16(val.cast<std::string>());
  } else if (auto *x = dynamic_cast<DescriptorUnitFloat*>(item)) {
    if (py::isinstance<py::dict>(val)) {
      auto d = val.cast<py::dict>();
      if (d.contains("value")) x->val = d["value"].cast<double>();
    } else if (py::isinstance<py::float_>(val) || py::isinstance<py::int_>(val)) {
      x->val = val.cast<double>();
    }
  } else if (auto *x = dynamic_cast<DescriptorEnumerated*>(item)) {
    if (py::isinstance<py::dict>(val)) {
      auto d = val.cast<py::dict>();
      if (d.contains("type"))  x->typeId = d["type"].cast<std::string>();
      if (d.contains("value")) x->enumId = d["value"].cast<std::string>();
    } else if (py::isinstance<py::str>(val)) {
      x->enumId = val.cast<std::string>();
    }
  } else if (auto *x = dynamic_cast<DescriptorList*>(item)) {
    if (py::isinstance<py::list>(val)) {
      auto l = val.cast<py::list>();
      size_t n = std::min(l.size(), x->items.size());
      for (size_t i = 0; i < n; i++) mergeValueIntoItem(x->items[i], l[i]);
    }
  } else if (auto *x = dynamic_cast<Descriptor*>(item)) {
    if (py::isinstance<py::dict>(val)) mergeDictIntoDescriptor(x, val.cast<py::dict>());
  }
  // RawData / Reference / Class / Alias: not mergeable (ignored)
}

// Parse the descriptor block `key` (after `skip` version-prefix bytes), merge
// `changes`, re-serialize, and swap it back into the layer's extra data.
void editLayerDescriptor(psd::PSDFile &self, int index, int key, int skip,
                         const py::dict &changes) {
  if (index < 0 || index >= (int)self.layerList.size())
    throw std::out_of_range("layer index out of range");
  psd::LayerInfo &lay = self.layerList[(size_t)index];
  for (auto &a : lay.extraData.additionalLayers) {
    if (a.key != key || !a.data) continue;
    psd::IteratorBase *rd = a.data->clone();
    rd->init();
    std::vector<uint8_t> prefix((size_t)(skip > 0 ? skip : 0));
    if (skip > 0) rd->getData(prefix.data(), skip);   // objVer/descVer 等をそのまま保持
    psd::Descriptor desc;
    desc.load(rd);
    delete rd;
    mergeDictIntoDescriptor(&desc, changes);
    std::vector<uint8_t> buf;
    psd::MemoryWriter w(buf);
    if (!prefix.empty()) w.putData(prefix.data(), prefix.size());
    psd::writeDescriptorBody(w, &desc);
    while (buf.size() & 3u) buf.push_back(0);   // descriptor data を 4 バイト境界へ
    self.setAdditionalInfoBytes(index, key, buf.data(), (int)buf.size());
    return;
  }
  throw std::runtime_error("layer has no descriptor block for that key");
}

// Text-layer editing lives in the C++ library (PSDFile::setLayerText /
// setLayerRunStyle / editTextLayer, psdfile.cpp). These wrappers only turn the
// bool + message result into a Python exception.
void raiseIfFailed(bool ok, const std::string &err) {
  if (ok) return;
  if (err == "layer index out of range") throw std::out_of_range(err);
  throw std::runtime_error(err.empty() ? "text layer edit failed" : err);
}

// Replace a text layer's body text (+ collapse run lengths, update 'Txt ').
void setLayerText(psd::PSDFile &self, int index, const psd::u16str &newText) {
  std::string err;
  raiseIfFailed(self.setLayerText(index, newText, &err), err);
}

// Edit an existing run's style values (no text/length change).
void setLayerRunStyle(psd::PSDFile &self, int index, int runIndex,
                      const psd::RunStyleEdit &edit) {
  std::string err;
  raiseIfFailed(self.setLayerRunStyle(index, runIndex, edit, &err), err);
}

// --- run style: Python の値 -> RunStyleEdit -------------------------------
// 指定された (None でない) フィールドだけ has* を立てる。返り値は「ひとつでも
// 指定されたか」。set_run_style (キーワード引数) と set_rich_text (runs[] の
// 辞書) の両方から使う。
bool fillRunStyleEdit(psd::RunStyleEdit &e, py::handle font, py::handle size_px,
                      py::handle color, py::handle tracking, py::handle kerning,
                      py::handle bold, py::handle italic, py::handle underline) {
  bool any = false;
  auto given = [](py::handle h) { return (bool)h && !h.is_none(); };
  if (given(font))      { e.hasFont = true; e.font = font.cast<std::string>(); any = true; }
  if (given(size_px))   { e.hasSize = true; e.size = size_px.cast<double>(); any = true; }
  if (given(tracking))  { e.hasTracking = true; e.tracking = tracking.cast<int>(); any = true; }
  if (given(kerning))   { e.hasKerning = true; e.kerning = kerning.cast<int>(); any = true; }
  if (given(bold))      { e.hasBold = true; e.bold = bold.cast<bool>(); any = true; }
  if (given(italic))    { e.hasItalic = true; e.italic = italic.cast<bool>(); any = true; }
  if (given(underline)) { e.hasUnderline = true; e.underline = underline.cast<bool>(); any = true; }
  if (given(color)) {
    auto seq = color.cast<py::sequence>();
    size_t n = py::len(seq);
    if (n < 3 || n > 4)
      throw std::invalid_argument("color must be (r,g,b) or (r,g,b,a), each 0..1");
    e.hasColor = true;
    e.color[0] = seq[0].cast<float>(); e.color[1] = seq[1].cast<float>();
    e.color[2] = seq[2].cast<float>(); e.color[3] = (n == 4) ? seq[3].cast<float>() : 1.0f;
    any = true;
  }
  return any;
}

// 辞書から key を引く (無ければ空ハンドル = 未指定)。
py::handle dictGet(const py::dict &d, const char *key) {
  return d.contains(key) ? d[key] : py::handle();
}

// set_rich_text の runs=[{...}] を TextRunSpec[] へ。
std::vector<psd::TextRunSpec> toRunSpecs(py::handle runs) {
  std::vector<psd::TextRunSpec> out;
  if (!runs || runs.is_none()) return out;
  for (py::handle h : runs.cast<py::sequence>()) {
    if (!py::isinstance<py::dict>(h))
      throw std::invalid_argument("set_rich_text: each run must be a dict");
    py::dict d = py::reinterpret_borrow<py::dict>(h);
    if (!d.contains("length"))
      throw std::invalid_argument("set_rich_text: each run needs a 'length' "
                                  "(UTF-16 code units)");
    psd::TextRunSpec spec;
    spec.length = d["length"].cast<int>();
    fillRunStyleEdit(spec.style, dictGet(d, "font"), dictGet(d, "size_px"),
                     dictGet(d, "color"), dictGet(d, "tracking"),
                     dictGet(d, "kerning"), dictGet(d, "bold"),
                     dictGet(d, "italic"), dictGet(d, "underline"));
    out.push_back(spec);
  }
  return out;
}

// set_rich_text の paragraphs=[{...}] を TextParagraphSpec[] へ。
std::vector<psd::TextParagraphSpec> toParagraphSpecs(py::handle paragraphs) {
  std::vector<psd::TextParagraphSpec> out;
  if (!paragraphs || paragraphs.is_none()) return out;
  for (py::handle h : paragraphs.cast<py::sequence>()) {
    if (!py::isinstance<py::dict>(h))
      throw std::invalid_argument("set_rich_text: each paragraph must be a dict");
    py::dict d = py::reinterpret_borrow<py::dict>(h);
    if (!d.contains("length"))
      throw std::invalid_argument("set_rich_text: each paragraph needs a 'length' "
                                  "(UTF-16 code units)");
    psd::TextParagraphSpec spec;
    spec.length = d["length"].cast<int>();
    py::handle j = dictGet(d, "justification");
    if (j && !j.is_none()) { spec.hasJustification = true; spec.justification = j.cast<int>(); }
    out.push_back(spec);
  }
  return out;
}

// 構造編集系: 範囲外は IndexError にしたいので事前に見る。
void checkLayerIndex(const psd::PSDFile &self, int index) {
  if (index < 0 || index >= (int)self.layerList.size())
    throw std::out_of_range("layer index out of range");
}

// Raw bytes of an additional-layer-info block (payload after the size field),
// or None. Useful for round-trip validation and low-level inspection.
py::object layerDescriptorBytes(const psd::LayerInfo &l, const std::string &keyStr) {
  if (keyStr.size() != 4) throw std::invalid_argument("key must be a 4-character string");
  int key = ((int)(uint8_t)keyStr[0] << 24) | ((int)(uint8_t)keyStr[1] << 16) |
            ((int)(uint8_t)keyStr[2] << 8)  |  (int)(uint8_t)keyStr[3];
  for (const auto &a : l.extraData.additionalLayers) {
    if (a.key != key || !a.data) continue;
    std::string buf((size_t)(a.size > 0 ? a.size : 0), '\0');
    if (a.size > 0) {
      psd::IteratorBase *rd = a.data->clone();
      rd->init();
      rd->getData(&buf[0], a.size);
      delete rd;
    }
    return py::bytes(buf);
  }
  return py::none();
}

// -------------------------------------------------------------------------
// Image resource raw-bytes access.
//
// Most image resources are kept as raw bytes internally (imageResourceList)
// but were never reachable from Python. These helpers expose them: a generic
// by-ID accessor plus typed shortcuts for the common ones (ICC / EXIF / XMP /
// thumbnail). Decoding (parsing EXIF tags, rendering the thumbnail) is left to
// the caller with e.g. Pillow.
// -------------------------------------------------------------------------

std::string resourceBytes(const psd::ImageResourceInfo &res) {
  std::string buf((size_t)(res.size > 0 ? res.size : 0), '\0');
  if (res.size > 0 && res.data) {
    psd::IteratorBase *rd = res.data->clone();
    rd->init();                       // rewind to the start of this resource
    rd->getData(&buf[0], res.size);
    delete rd;
  }
  return buf;
}

const psd::ImageResourceInfo *findResource(const psd::PSDFile &self, int id) {
  for (const auto &res : self.imageResourceList)
    if (res.id == id) return &res;
  return nullptr;
}

py::list imageResourceIds(psd::PSDFile &self) {
  py::list out;
  for (const auto &res : self.imageResourceList) out.append((int)res.id);
  return out;
}

py::object imageResource(psd::PSDFile &self, int id) {
  const psd::ImageResourceInfo *res = findResource(self, id);
  return res ? py::object(py::bytes(resourceBytes(*res))) : py::none();
}

py::object iccProfile(psd::PSDFile &self) { return imageResource(self, 1039); }
py::object exifData(psd::PSDFile &self)   { return imageResource(self, 1058); }

// XMP packet (resource 1060) is UTF-8 XML. Returned as str; use
// image_resource(1060) for the raw bytes if the packet is not valid UTF-8.
py::object xmpMetadata(psd::PSDFile &self) {
  const psd::ImageResourceInfo *res = findResource(self, 1060);
  if (!res) return py::none();
  return py::str(resourceBytes(*res));
}

// Embedded thumbnail (resource 1036 = RGB / legacy 1033 = BGR). Header is 28
// bytes; for format==1 the payload is a JFIF JPEG. Returns a dict with the
// header fields and the raw payload, or None.
py::object thumbnail(psd::PSDFile &self) {
  const psd::ImageResourceInfo *res = findResource(self, 1036);
  if (!res) res = findResource(self, 1033);
  if (!res) return py::none();
  std::string raw = resourceBytes(*res);
  if (raw.size() < 28) return py::none();
  auto be32 = [&](size_t o) {
    return (uint32_t)(((uint8_t)raw[o] << 24) | ((uint8_t)raw[o+1] << 16) |
                      ((uint8_t)raw[o+2] << 8) | (uint8_t)raw[o+3]);
  };
  auto be16 = [&](size_t o) {
    return (uint16_t)(((uint8_t)raw[o] << 8) | (uint8_t)raw[o+1]);
  };
  py::dict d;
  uint32_t fmt = be32(0);
  d["format"]      = (fmt == 1) ? "jpeg" : "raw";
  d["width"]       = be32(4);
  d["height"]      = be32(8);
  d["bits"]        = be16(24);
  d["resource_id"] = (int)res->id;   // 1036 = RGB order, 1033 = BGR order
  d["data"]        = py::bytes(raw.data() + 28, raw.size() - 28);
  return std::move(d);
}

py::bytes layerImage(psd::PSDFile &self, int index, const std::string &mode) {
  if (!self.isLoaded) throw std::runtime_error("PSD not loaded");
  if (index < 0 || index >= (int)self.layerList.size())
    throw std::out_of_range("layer index out of range");
  psd::ImageMode m;
  if      (mode == "image")  m = psd::IMAGE_MODE_IMAGE;
  else if (mode == "mask")   m = psd::IMAGE_MODE_MASK;
  else if (mode == "masked") m = psd::IMAGE_MODE_MASKEDIMAGE;
  else throw std::invalid_argument("mode must be 'image', 'mask' or 'masked'");
  psd::LayerInfo &lay = self.layerList[(size_t)index];
  if (lay.width <= 0 || lay.height <= 0) return py::bytes();
  size_t n = (size_t)lay.width * (size_t)lay.height * 4;
  std::string buf(n, '\0');
  self.getLayerImage(lay, buf.data(), psd::BGRA_LE, lay.width * 4, m);
  return py::bytes(buf);
}

} // namespace

PYBIND11_MODULE(psdparse, m) {
  m.doc() = "psdparse: PSD reader/writer (pure C++17, zlib only).";

  // Internal: parse + re-serialize EngineData for byte-exact round-trip tests.
  m.def("_reserialize_engine_data", [](py::bytes b) -> py::object {
      py::buffer_info info(py::buffer(b).request());
      std::string out;
      if (!psd::reserializeEngineData((const char *)info.ptr, (size_t)info.size, out))
          return py::none();
      return py::bytes(out);
  }, py::arg("data"));

  py::enum_<psd::LayerType>(m, "LayerType")
    .value("NORMAL", psd::LAYER_TYPE_NORMAL)
    .value("HIDDEN", psd::LAYER_TYPE_HIDDEN)
    .value("FOLDER", psd::LAYER_TYPE_FOLDER)
    .value("ADJUST", psd::LAYER_TYPE_ADJUST)
    .value("FILL",   psd::LAYER_TYPE_FILL)
    .value("TEXT",   psd::LAYER_TYPE_TEXT)
    .export_values();

  py::enum_<psd::BlendMode>(m, "BlendMode")
    .value("INVALID",      psd::BLEND_MODE_INVALID)
    .value("NORMAL",       psd::BLEND_MODE_NORMAL)
    .value("DISSOLVE",     psd::BLEND_MODE_DISSOLVE)
    .value("DARKEN",       psd::BLEND_MODE_DARKEN)
    .value("MULTIPLY",     psd::BLEND_MODE_MULTIPLY)
    .value("COLOR_BURN",   psd::BLEND_MODE_COLOR_BURN)
    .value("LINEAR_BURN",  psd::BLEND_MODE_LINEAR_BURN)
    .value("LIGHTEN",      psd::BLEND_MODE_LIGHTEN)
    .value("SCREEN",       psd::BLEND_MODE_SCREEN)
    .value("COLOR_DODGE",  psd::BLEND_MODE_COLOR_DODGE)
    .value("LINEAR_DODGE", psd::BLEND_MODE_LINEAR_DODGE)
    .value("OVERLAY",      psd::BLEND_MODE_OVERLAY)
    .value("SOFT_LIGHT",   psd::BLEND_MODE_SOFT_LIGHT)
    .value("HARD_LIGHT",   psd::BLEND_MODE_HARD_LIGHT)
    .value("VIVID_LIGHT",  psd::BLEND_MODE_VIVID_LIGHT)
    .value("LINEAR_LIGHT", psd::BLEND_MODE_LINEAR_LIGHT)
    .value("PIN_LIGHT",    psd::BLEND_MODE_PIN_LIGHT)
    .value("HARD_MIX",     psd::BLEND_MODE_HARD_MIX)
    .value("DIFFERENCE",   psd::BLEND_MODE_DIFFERENCE)
    .value("EXCLUSION",    psd::BLEND_MODE_EXCLUSION)
    .value("HUE",          psd::BLEND_MODE_HUE)
    .value("SATURATION",   psd::BLEND_MODE_SATURATION)
    .value("COLOR",        psd::BLEND_MODE_COLOR)
    .value("LUMINOSITY",   psd::BLEND_MODE_LUMINOSITY)
    .value("PASS_THROUGH", psd::BLEND_MODE_PASS_THROUGH)
    .value("DARKER_COLOR", psd::BLEND_MODE_DARKER_COLOR)
    .value("LIGHTER_COLOR",psd::BLEND_MODE_LIGHTER_COLOR)
    .value("SUBTRACT",     psd::BLEND_MODE_SUBTRACT)
    .value("DIVIDE",       psd::BLEND_MODE_DIVIDE);

  py::class_<psd::Header>(m, "Header")
    .def_readonly("version",  &psd::Header::version)
    .def_readonly("channels", &psd::Header::channels)
    .def_readonly("height",   &psd::Header::height)
    .def_readonly("width",    &psd::Header::width)
    .def_readonly("depth",    &psd::Header::depth)
    .def_readonly("mode",     &psd::Header::mode)
    .def_readonly("hres",     &psd::Header::hres)   // 水平解像度 dpi (既定 72)
    .def_readonly("vres",     &psd::Header::vres);  // 垂直解像度 dpi

  py::class_<psd::ChannelInfo>(m, "ChannelInfo")
    .def_readonly("id",     &psd::ChannelInfo::id)
    .def_readonly("length", &psd::ChannelInfo::length)
    .def("is_mask", &psd::ChannelInfo::isMaskChannel);

  py::class_<psd::LayerInfo>(m, "LayerInfo")
    .def_readonly("top",     &psd::LayerInfo::top)
    .def_readonly("left",    &psd::LayerInfo::left)
    .def_readonly("bottom",  &psd::LayerInfo::bottom)
    .def_readonly("right",   &psd::LayerInfo::right)
    .def_readonly("width",   &psd::LayerInfo::width)
    .def_readonly("height",  &psd::LayerInfo::height)
    // --- 書き換え可能なレコード項目 (E1: save() 時にフィールドから再出力) ---
    .def_readwrite("opacity", &psd::LayerInfo::opacity,
        "Layer opacity 0..255. Writable — the new value is re-serialized on save().")
    .def_readwrite("clipping", &psd::LayerInfo::clipping,
        "Clipping 0=base / 1=non-base. Writable.")
    .def_property("blend_mode_key",
        [](const psd::LayerInfo &l) { return l.blendModeKey; },
        [](psd::LayerInfo &l, int key) {
            l.blendModeKey = key; l.blendMode = psd::blendKeyToMode(key);
        },
        "Blend-mode 4cc as an int (e.g. 0x6D756C20 == 'mul '). Writable; also "
        "updates blend_mode. Use set_blend_mode(str) for a friendlier setter.")
    .def("set_blend_mode",
        [](psd::LayerInfo &l, const std::string &k) {
            if (k.size() != 4) throw std::invalid_argument("blend mode must be a 4-char key, e.g. 'mul '");
            int key = ((int)(uint8_t)k[0] << 24) | ((int)(uint8_t)k[1] << 16) |
                      ((int)(uint8_t)k[2] << 8)  |  (int)(uint8_t)k[3];
            l.blendModeKey = key; l.blendMode = psd::blendKeyToMode(key);
        },
        py::arg("key"),
        "Set the blend mode from a 4-char key string (e.g. 'norm', 'mul ', "
        "'scrn'). Note the trailing space on 3-letter keys.")
    .def_property("fill_opacity",
        [](const psd::LayerInfo &l) { return l.fill_opacity; },
        [](psd::LayerInfo &l, int v) {
            l.fill_opacity = v < 0 ? 0 : (v > 255 ? 255 : v);
            l.extraData.useRawBytes = false;    // reconstruct extra data (iOpa) on save
        },
        "Fill opacity 0..255 (the 'iOpa' block). Writable.")
    .def_readonly("blend_mode",    &psd::LayerInfo::blendMode)
    .def_readonly("layer_type",    &psd::LayerInfo::layerType)
    .def_readonly("layer_id",      &psd::LayerInfo::layerId)
    .def_readonly("channels",      &psd::LayerInfo::channels)
    .def_property_readonly("name", [](const psd::LayerInfo &l) {
        return l.extraData.layerName;  // std::string (raw bytes, original encoding)
    })
    .def_property("name_unicode",
        [](const psd::LayerInfo &l) { return u16ToStr(l.layerNameUnicode); },
        [](psd::LayerInfo &l, const std::string &s) {
            l.layerName            = s;                 // pascal (UTF-8 bytes)
            l.layerNameUnicode     = psd::utf8ToU16(s); // luni (Unicode)
            l.extraData.layerName  = s;
            l.extraData.useRawBytes = false;            // reconstruct extra data on save
        },
        "Unicode layer name (luni). Writable — assigning renames the layer "
        "(updates both the Pascal name and the luni block; extra data is "
        "reconstructed on save with mask/blending ranges preserved).")
    .def_readonly("parent_index", &psd::LayerInfo::parentIndex,
        "Index into PSDFile.layers of the enclosing folder layer, or -1 for "
        "top-level layers. Build the layer tree from these.")
    .def_property_readonly("text", &layerText,
        "Text-layer content/style as a dict (keys: text, orientation, "
        "justification, transform, runs[]), or None for non-text layers.")
    .def_property_readonly("mask", &layerMask,
        "Layer mask as a dict (bbox, default_color, flags, disabled, real{}), "
        "or None when the layer has no mask.")
    .def_property_readonly("blending_ranges", &layerBlendingRanges,
        "Layer blending ranges as a dict (gray, channels[]), or None.")
    .def_property_readonly("effects", &layerEffects,
        "Object-based layer effects ('lfx2') as a nested descriptor dict "
        "(drop shadow / glow / overlay / stroke / bevel ...), or None.")
    .def_property_readonly("fill", &layerFill,
        "Fill-layer content as {'type': 'solid'|'gradient'|'pattern', "
        "'data': {...}} from SoCo/GdFl/PtFl, or None.")
    .def_property_readonly("comp_states", &layerCompStates,
        "Per-layer layer-comp state as {comp_id: {'enabled', 'offset_x', "
        "'offset_y'}} (empty when the layer is in no comps). `enabled` says "
        "whether this layer is shown in that document comp (PSDFile.layer_comps).")
    .def_property_readonly("sheet_color", &layerSheetColor,
        "Layer-panel color label ('lclr') as {'index', 'name'} (0/'none' .. "
        "11/'fuschia'), or None when the layer carries no lclr block.")
    .def_property_readonly("info_keys", &layerInfoKeys,
        "List of the 4cc keys of every additional-layer-info block present.")
    .def("descriptor_bytes", &layerDescriptorBytes, py::arg("key"),
        "Raw bytes of the additional-layer-info block with this 4cc key "
        "(payload after the size field), or None if absent.")
    .def("descriptor", &layerDescriptor,
        py::arg("key"), py::arg("skip") = -1,
        "Parse an arbitrary additional-info `key` (4-char str) as a Photoshop "
        "descriptor dict. `skip` = version-prefix bytes before the descriptor "
        "(-1 = auto for known keys, else 0). Returns None if absent/unparseable.")
    .def_property("visible",
        [](const psd::LayerInfo &l){ return l.isVisible(); },
        [](psd::LayerInfo &l, bool v){
            if (v) l.flag &= ~(1 << 1);   // visible = clear "hidden" bit
            else   l.flag |=  (1 << 1);
        },
        "Layer visibility. Writable — toggles flag bit 1 and is re-serialized on save().")
    .def_property_readonly("transparency_protected", [](const psd::LayerInfo &l){ return l.isTransparencyProtected(); })
    .def_property_readonly("obsolete",               [](const psd::LayerInfo &l){ return l.isObsolete(); })
    .def_property_readonly("pixel_data_irrelevant",  [](const psd::LayerInfo &l){ return l.isPixelDataIrrelevant(); });

  py::class_<psd::PSDFile>(m, "PSDFile")
    .def(py::init<>())
    .def("load",
         [](psd::PSDFile &self, const std::string &path) {
            return self.load(path.c_str());
         },
         py::arg("path"),
         "Memory-map the file at `path` (UTF-8) and parse. The file stays "
         "open and layer pixels are paged in lazily.")
    .def("load_bytes",
         [](psd::PSDFile &self, py::bytes b) {
            py::buffer_info info(py::buffer(b).request());
            return self.loadFromMemory((const uint8_t *)info.ptr, (size_t)info.size);
         },
         py::arg("data"),
         "Parse a PSD already loaded into a Python bytes object. The bytes "
         "are copied into an internal vector.")
    .def("load_streamed",
         [](psd::PSDFile &self, const std::string &path) {
            // std::ifstream + StreamReader 経由 (mmap を使わずシークと read のみで処理)。
            // Win32 では ifstream を unicode path で開くため UTF-8 → wide。
#ifdef _WIN32
            std::wstring wpath = psd::utf8ToWide(path.c_str());
            if (wpath.empty()) return false;
            auto s = std::make_unique<std::ifstream>(wpath, std::ios::binary);
#else
            auto s = std::make_unique<std::ifstream>(path, std::ios::binary);
#endif
            if (!s || !*s) return false;
            return self.loadFromStream(std::move(s));
         },
         py::arg("path"),
         "Open `path` (UTF-8) as a std::ifstream and parse via StreamReader. "
         "Demonstrates that the parser also accepts arbitrary seekable streams "
         "(the same code path an embedder uses to plug in its own stream "
         "type via StreamReader::Source).")
    .def("save",
         [](psd::PSDFile &self, const std::string &path) {
            return self.save(path.c_str());
         },
         py::arg("path"),
         "Save the currently loaded data as a PSD file at `path` (UTF-8). "
         "Round-trip-fidelity is the target: load(p) -> save(q) yields a PSD "
         "with structurally identical layers/header/image data. Structural "
         "edits (delete/move/duplicate/copy_layer_from) are re-serialized here.")
    .def("delete_layer",
         [](psd::PSDFile &self, int index) {
            if (!self.deleteLayer(index))
                throw std::out_of_range("layer index out of range");
         },
         py::arg("index"),
         "Delete the layer at `index`. Pixels are dropped on the next save(). "
         "Note: deleting one half of a folder's FOLDER/HIDDEN divider pair "
         "unbalances the group — delete whole groups for clean results.")
    .def("move_layer",
         [](psd::PSDFile &self, int from_index, int to_index) {
            if (!self.moveLayer(from_index, to_index))
                throw std::out_of_range("layer index out of range");
         },
         py::arg("from_index"), py::arg("to_index"),
         "Move the layer at `from_index` so it lands at `to_index` (index in "
         "the list after removal). Reorders draw order on the next save(). "
         "One entry only — moving a FOLDER this way leaves its divider and "
         "contents behind; use move_layer_sibling / move_layer_range for "
         "whole groups.")
    .def("move_layer_sibling",
         [](psd::PSDFile &self, int index, bool up) -> py::object {
            checkLayerIndex(self, index);
            int newIndex = index;
            if (!self.moveLayerSibling(index, up, &newIndex))
                return py::none();          // 端まで来ていて動かせない
            return py::int_(newIndex);
         },
         py::arg("index"), py::arg("up") = true,
         "Swap the layer at `index` with its next sibling *at the same level*. "
         "`up=True` moves it one step up in Photoshop's layer panel (later in "
         "the flat list). A FOLDER moves as a whole block (divider + contents, "
         "nested groups included) and steps over a sibling folder in one go; "
         "the move never crosses into another folder. Returns the layer's new "
         "index, or None when it is already at the end of its level.")
    .def("move_layer_range",
         [](psd::PSDFile &self, int from_index, int count, int to_index) {
            if (!self.moveLayerRange(from_index, count, to_index))
                throw std::out_of_range("layer range out of range");
         },
         py::arg("from_index"), py::arg("count"), py::arg("to_index"),
         "Low-level block move: relocate [from_index, from_index+count) to "
         "`to_index`, which is given as an index in the list *before* removal. "
         "Moving a range onto itself is a no-op. Pair it with group_span() to "
         "move a whole folder.")
    .def("group_span",
         [](const psd::PSDFile &self, int index) {
            checkLayerIndex(self, index);
            int start = index, count = 1;
            self.groupSpan(index, start, count);
            return py::make_tuple(start, count);
         },
         py::arg("index"),
         "The (start, count) span the layer at `index` occupies in the flat "
         "list. For a FOLDER that is [HIDDEN divider … FOLDER] including any "
         "nested groups; for anything else it is (index, 1).")
    .def("duplicate_layer",
         [](psd::PSDFile &self, int index) {
            int r = self.duplicateLayer(index);
            if (r < 0) throw std::out_of_range("layer index out of range");
            return r;
         },
         py::arg("index"),
         "Duplicate the layer at `index` (inserted right after it). Returns the "
         "new layer's index. The copy shares the source's pixel bytes lazily "
         "but gets a fresh layer_id (max existing lyid + 1, like Photoshop).")
    .def("copy_layer_from",
         [](psd::PSDFile &self, const psd::PSDFile &src, int src_index, int dest_index) {
            int r = self.copyLayerFrom(src, src_index, dest_index);
            if (r < 0) throw std::out_of_range("source layer index out of range");
            return r;
         },
         py::arg("source"), py::arg("src_index"), py::arg("dest_index") = -1,
         py::keep_alive<1, 2>(),   // keep `source` alive as long as self lives
         "Copy a layer from another loaded PSDFile into this one at "
         "`dest_index` (default: append). Returns the new index. The copied "
         "layer references `source`'s pixel/extra bytes lazily, so `source` is "
         "kept alive until this file is garbage-collected (do not let it close "
         "before save()). The copy gets a layer_id unique within this document. "
         "Assumes matching color mode and bit depth.")
    .def("set_effects",
         [](psd::PSDFile &self, int index, py::dict changes) {
            editLayerDescriptor(self, index, 'lfx2', 8, changes);
         },
         py::arg("index"), py::arg("changes"),
         "Edit layer effect (lfx2) values. `changes` is a partial dict shaped "
         "like layer.effects: only the leaf values present are overwritten "
         "(numbers, {'value':..} for UnitFloat, {'value':..} / str for enums, "
         "nested dicts for sub-descriptors, lists per-index). Structure, class "
         "IDs and types are preserved; unknown keys are ignored. Raises if the "
         "layer has no lfx2 block.")
    .def("set_layer_descriptor",
         [](psd::PSDFile &self, int index, const std::string &keyStr, py::dict changes, int skip) {
            if (keyStr.size() != 4)
                throw std::invalid_argument("key must be a 4-character string");
            int key = ((int)(uint8_t)keyStr[0] << 24) | ((int)(uint8_t)keyStr[1] << 16) |
                      ((int)(uint8_t)keyStr[2] << 8)  |  (int)(uint8_t)keyStr[3];
            if (skip < 0) {
                switch (key) {
                case 'lfx2':                             skip = 8; break;
                case 'SoCo': case 'GdFl': case 'PtFl':   skip = 4; break;
                default:                                 skip = 0; break;
                }
            }
            editLayerDescriptor(self, index, key, skip, changes);
         },
         py::arg("index"), py::arg("key"), py::arg("changes"), py::arg("skip") = -1,
         "Generic version of set_effects for an arbitrary descriptor key "
         "(e.g. 'SoCo'/'GdFl'/'PtFl' fill layers). `skip` = version-prefix "
         "bytes (-1 = auto for known keys).")
    .def("set_text",
         [](psd::PSDFile &self, int index, const std::string &text) {
            setLayerText(self, index, psd::utf8ToU16(text));
         },
         py::arg("index"), py::arg("text"),
         "Replace a text layer's body text (rewrites the embedded EngineData + "
         "'Txt '). Multi-run styling collapses to the first run's style; a "
         "trailing newline is added if missing. Raises for non-text layers.")
    .def("set_run_style",
         [](psd::PSDFile &self, int index, int run_index,
            py::object size_px, py::object color, py::object tracking,
            py::object kerning, py::object bold, py::object italic,
            py::object underline, py::object font) {
            psd::RunStyleEdit e;
            if (!fillRunStyleEdit(e, font, size_px, color, tracking, kerning,
                                  bold, italic, underline))
                throw std::invalid_argument("set_run_style: pass at least one of "
                    "font/size_px/color/tracking/kerning/bold/italic/underline");
            setLayerRunStyle(self, index, run_index, e);
         },
         py::arg("index"), py::arg("run_index"),
         py::arg("size_px") = py::none(), py::arg("color") = py::none(),
         py::arg("tracking") = py::none(), py::arg("kerning") = py::none(),
         py::arg("bold") = py::none(), py::arg("italic") = py::none(),
         py::arg("underline") = py::none(),
         // font は 0.9.0 で後から足したので、既存の位置引数の並びを崩さない
         // ように末尾に置く。
         py::arg("font") = py::none(),
         "Edit style values of an existing style run (see text['runs']). Any of: "
         "font (str; appended to the document's font set if new), size_px "
         "(float), color ((r,g,b[,a]) 0..1), tracking (int), kerning (int), "
         "bold/italic/underline (bool). Text and run lengths are unchanged; "
         "keys are added to the run if inherited. Raises for non-text layers or "
         "an out-of-range run index.")
    .def("set_rich_text",
         [](psd::PSDFile &self, int index, const std::string &text,
            py::object runs, py::object paragraphs) {
            std::vector<psd::TextRunSpec> r = toRunSpecs(runs);
            std::vector<psd::TextParagraphSpec> p = toParagraphSpecs(paragraphs);
            std::string err;
            raiseIfFailed(self.setLayerRichText(index, psd::utf8ToU16(text), r, p, &err), err);
         },
         py::arg("index"), py::arg("text"), py::arg("runs") = py::none(),
         py::arg("paragraphs") = py::none(),
         "Replace a text layer's body text together with its run / paragraph "
         "structure (set_text collapses everything to one run instead). "
         "`runs` is a list of dicts: {'length': int (UTF-16 code units), plus "
         "any of font/size_px/color/tracking/kerning/bold/italic/underline}; "
         "unspecified style fields are inherited from the original first run. "
         "`paragraphs` is a list of {'length': int, 'justification': int}. "
         "An empty/omitted list collapses to a single run / paragraph. If the "
         "run lengths don't add up to the text length, the last run absorbs "
         "the difference. A trailing '\\r' is added if missing.")
    .def("set_justification",
         [](psd::PSDFile &self, int index, int justification, int para_index) {
            std::string err;
            raiseIfFailed(self.setLayerJustification(index, para_index, justification, &err), err);
         },
         py::arg("index"), py::arg("justification"), py::arg("para_index") = -1,
         "Set paragraph alignment on a text layer: 0=left, 1=right, 2=center. "
         "`para_index` selects one paragraph (see text['paragraphs']); the "
         "default -1 applies it to every paragraph. Text and run structure are "
         "unchanged.")
    .def("text_fonts",
         [](const psd::PSDFile &self, int index) {
            std::vector<std::string> names;
            std::string err;
            raiseIfFailed(self.getLayerFonts(index, names, &err), err);
            return names;
         },
         py::arg("index"),
         "List the font names in this text layer's EngineData font set "
         "(ResourceDict/FontSet) — the candidates a font picker would show. "
         "Raises for non-text layers.")
    .def("text_transform",
         [](const psd::PSDFile &self, int index) {
            double m[6];
            std::string err;
            raiseIfFailed(self.getLayerTextTransform(index, m, &err), err);
            return py::make_tuple(m[0], m[1], m[2], m[3], m[4], m[5]);
         },
         py::arg("index"),
         "The text layer's affine placement transform (xx, xy, yx, yy, tx, ty) "
         "read from the TySh prefix. Same values as layer.text['transform'].")
    .def("set_text_transform",
         [](psd::PSDFile &self, int index, py::sequence m) {
            if (py::len(m) != 6)
                throw std::invalid_argument("transform must be 6 numbers "
                                            "(xx, xy, yx, yy, tx, ty)");
            double v[6];
            for (int i = 0; i < 6; i++) v[i] = m[i].cast<double>();
            std::string err;
            raiseIfFailed(self.setLayerTextTransform(index, v, &err), err);
         },
         py::arg("index"), py::arg("transform"),
         "Replace the text layer's affine transform (xx, xy, yx, yy, tx, ty). "
         "Only the TySh block changes — the layer rectangle stays put, so use "
         "move_text_layer() for plain translation.")
    .def("move_text_layer",
         [](psd::PSDFile &self, int index, double dx, double dy) {
            std::string err;
            raiseIfFailed(self.moveTextLayer(index, dx, dy, &err), err);
         },
         py::arg("index"), py::arg("dx"), py::arg("dy"),
         "Translate a text layer by (dx, dy) pixels: the transform's tx/ty and "
         "the layer (and mask) rectangle all shift, so the PSD's baked raster "
         "moves with the text. Photoshop re-renders it at the new position on "
         "open.")
    .def("text_bounds",
         [](const psd::PSDFile &self, int index) {
            double l, t, r, b;
            std::string err;
            raiseIfFailed(self.getLayerTextBounds(index, l, t, r, b, &err), err);
            return py::make_tuple(l, t, r, b);
         },
         py::arg("index"),
         "The text layer's flow box as (left, top, right, bottom), in the "
         "transform's local coordinates (the descriptor's 'bounds').")
    .def("set_text_bounds",
         [](psd::PSDFile &self, int index, double left, double top,
            double right, double bottom) {
            std::string err;
            raiseIfFailed(self.setLayerTextBounds(index, left, top, right, bottom, &err), err);
         },
         py::arg("index"), py::arg("left"), py::arg("top"), py::arg("right"),
         py::arg("bottom"),
         "Resize the text layer's flow box (transform-local coordinates). Only "
         "paragraph (box) text actually re-flows into a new box — for point "
         "text Photoshop rebuilds the box from the glyphs. 'boundingBox' is "
         "clamped into the new box so the layer still displays sanely.")
    .def("set_layer_name",
         [](psd::PSDFile &self, int index, const std::string &name) {
            if (!self.setLayerName(index, name.c_str()))
                throw std::out_of_range("layer index out of range");
         },
         py::arg("index"), py::arg("name"),
         "Rename layer `index` (updates Pascal + luni names). Equivalent to "
         "`layer.name_unicode = name`. Re-serialized on save().")
    .def("set_layer_mask",
         [](psd::PSDFile &self, int index, py::object disabled, py::object density,
            py::object feather, py::object default_color) {
            bool any = false, ok = true;
            if (!disabled.is_none())      { ok &= self.setMaskDisabled(index, disabled.cast<bool>()); any = true; }
            if (!density.is_none())       { ok &= self.setMaskDensity(index, density.cast<int>()); any = true; }
            if (!feather.is_none())       { ok &= self.setMaskFeather(index, feather.cast<double>()); any = true; }
            if (!default_color.is_none()) { ok &= self.setMaskDefaultColor(index, default_color.cast<int>()); any = true; }
            if (!any)
                throw std::invalid_argument("set_layer_mask: pass at least one of "
                                            "disabled/density/feather/default_color");
            if (!ok)
                throw std::runtime_error("set_layer_mask failed (index out of range, "
                                         "or the layer has no mask)");
         },
         py::arg("index"), py::arg("disabled") = py::none(),
         py::arg("density") = py::none(), py::arg("feather") = py::none(),
         py::arg("default_color") = py::none(),
         "Edit an existing layer mask's values (the layer must already have a "
         "mask). Any of: disabled (bool), density (0..255), feather (px, float), "
         "default_color (0..255). The mask rectangle and pixels are unchanged; "
         "the mask block is re-serialized on save().")
    .def("create_blank",
         [](psd::PSDFile &self, int width, int height, int mode) {
            if (!self.createBlank(width, height, mode))
                throw std::invalid_argument("create_blank failed (size must be > 0 "
                                            "and mode must be COLOR_MODE_RGB)");
         },
         py::arg("width"), py::arg("height"),
         py::arg("mode") = (int)psd::COLOR_MODE_RGB,
         "Initialize this PSDFile as a blank width×height 8-bit RGB document "
         "(white composite). Then build it up with add_layer(...) and save(). "
         "RGB only. The stored composite stays white until an editor recomposites.")
    .def("set_layer_pixels",
         [](psd::PSDFile &self, int index, py::bytes data, int width, int height) {
            py::buffer_info info(py::buffer(data).request());
            size_t need = (size_t)width * (size_t)height * 4;
            if (width <= 0 || height <= 0 || (size_t)info.size != need)
                throw std::invalid_argument("data must be width*height*4 BGRA bytes");
            if (!self.setLayerPixels(index, (const uint8_t *)info.ptr, width, height))
                throw std::runtime_error("set_layer_pixels failed (index out of range, "
                                         "or document is not 8-bit RGB)");
         },
         py::arg("index"), py::arg("data"), py::arg("width"), py::arg("height"),
         "Replace layer `index`'s pixels with BGRA bytes (width*height*4). The "
         "layer's left/top are kept; width/height are updated. Channels are "
         "RLE-encoded on the next save(). 8-bit RGB documents only. The layer's "
         "mask/extra data is left unchanged — avoid resizing a masked layer.")
    .def("set_merged_image",
         [](psd::PSDFile &self, py::bytes data) {
            py::buffer_info info(py::buffer(data).request());
            size_t need = (size_t)self.header.width * (size_t)self.header.height * 4;
            if ((size_t)info.size != need)
                throw std::invalid_argument("data must be header.width*header.height*4 BGRA bytes");
            if (!self.setMergedImage((const uint8_t *)info.ptr,
                                     self.header.width, self.header.height))
                throw std::runtime_error("set_merged_image failed (document is not 8-bit RGB)");
         },
         py::arg("data"),
         "Replace the stored composite (merged) image with canvas-sized BGRA "
         "bytes (header.width*header.height*4). Use this to write a "
         "Python-composited preview back into the PSD. 8-bit RGB only.")
    .def("set_layer_mask_pixels",
         [](psd::PSDFile &self, int index, py::bytes data,
            int top, int left, int width, int height) {
            py::buffer_info info(py::buffer(data).request());
            size_t need = (size_t)width * (size_t)height;
            if (width <= 0 || height <= 0 || (size_t)info.size != need)
                throw std::invalid_argument("data must be width*height grayscale bytes");
            if (!self.setLayerMaskPixels(index, (const uint8_t *)info.ptr,
                                         top, left, width, height))
                throw std::runtime_error("set_layer_mask_pixels failed (index out of "
                                         "range, or document is not 8-bit)");
         },
         py::arg("index"), py::arg("data"), py::arg("top"), py::arg("left"),
         py::arg("width"), py::arg("height"),
         "Set/replace the layer's mask with grayscale bytes (width*height, one "
         "byte/px; 0=hidden, 255=shown). Positions the mask rectangle at "
         "(left, top) — this also sets mask geometry. Creates the mask if the "
         "layer had none. Color channels are preserved. 8-bit documents only.")
    .def("add_layer",
         [](psd::PSDFile &self, const std::string &name, int left, int top,
            py::bytes data, int width, int height,
            const std::string &blend_mode, int opacity, int dest_index) {
            py::buffer_info info(py::buffer(data).request());
            size_t need = (size_t)width * (size_t)height * 4;
            if (width <= 0 || height <= 0 || (size_t)info.size != need)
                throw std::invalid_argument("data must be width*height*4 BGRA bytes");
            if (blend_mode.size() != 4)
                throw std::invalid_argument("blend_mode must be a 4-char key, e.g. 'norm'");
            int key = ((int)(uint8_t)blend_mode[0] << 24) | ((int)(uint8_t)blend_mode[1] << 16) |
                      ((int)(uint8_t)blend_mode[2] << 8)  |  (int)(uint8_t)blend_mode[3];
            int r = self.addLayer(name.c_str(), left, top, (const uint8_t *)info.ptr,
                                  width, height, key, opacity, dest_index);
            if (r < 0)
                throw std::runtime_error("add_layer failed (document is not 8-bit RGB)");
            return r;
         },
         py::arg("name"), py::arg("left"), py::arg("top"),
         py::arg("data"), py::arg("width"), py::arg("height"),
         py::arg("blend_mode") = "norm", py::arg("opacity") = 255,
         py::arg("dest_index") = -1,
         "Add a new image layer at (left, top) from BGRA bytes "
         "(width*height*4). Returns the new layer index. `blend_mode` is a "
         "4-char key ('norm', 'mul ', ...). 8-bit RGB documents only.")
    .def("add_folder",
         [](psd::PSDFile &self, const std::string &name, int from, int count,
            bool closed, const std::string &blend_mode, int opacity) {
            if (blend_mode.size() != 4)
                throw std::invalid_argument("blend_mode must be a 4-char key, e.g. 'pass'");
            int key = ((int)(uint8_t)blend_mode[0] << 24) | ((int)(uint8_t)blend_mode[1] << 16) |
                      ((int)(uint8_t)blend_mode[2] << 8)  |  (int)(uint8_t)blend_mode[3];
            int r = self.addFolder(name.c_str(), from, count, closed, key, opacity);
            if (r < 0)
                throw std::runtime_error("add_folder failed (bad range, or document "
                                         "is not 8-bit RGB)");
            return r;
         },
         py::arg("name"), py::arg("from_index"), py::arg("count"),
         py::arg("closed") = false, py::arg("blend_mode") = "pass",
         py::arg("opacity") = 255,
         "Wrap layers[from_index : from_index+count] in a layer group. Inserts "
         "the two marker layers PSD uses for a folder (a '</Layer group>' divider "
         "below the contents and the folder layer above them) and returns the "
         "folder layer's index. `blend_mode` defaults to 'pass' (pass-through), "
         "matching Photoshop's new-group default. Pass count=0 for an empty folder.")
    .def_readonly("is_loaded", &psd::PSDFile::isLoaded)
    .def_readonly("header",    &psd::PSDFile::header)
    .def_readonly("layers",    &psd::PSDFile::layerList)
    .def_readonly("merged_alpha", &psd::PSDFile::mergedAlpha)
    .def("merged_image", &mergedImage)
    .def("layer_image", &layerImage,
         py::arg("index"), py::arg("mode") = "masked",
         "Extract pixels for layer `index` as BGRA bytes. "
         "mode: 'masked' (default), 'image' (no mask), 'mask' (mask only).")
    .def_property_readonly("guides", &psdGuides,
         "Grid & guides (image resource 1032) as a dict "
         "(horizontal_grid, vertical_grid, guides[]), or None.")
    .def_property_readonly("slices", &psdSlices,
         "Slices (image resource 1050 v6) as a dict "
         "(group_name, bounding, slices[]), or None.")
    .def_property_readonly("layer_comps", &psdLayerComps,
         "Document layer comps (image resource 1065) as a list of dicts "
         "(id, name, comment, record_*). Empty list when there are none.")
    .def_property_readonly("color_table", &psdColorTable,
         "Indexed-color palette as a dict (colors[], valid_count, "
         "transparency_index), or None for non-indexed PSDs.")
    .def_property_readonly("global_layer_mask", &psdGlobalLayerMask,
         "Global layer mask info as a dict (overlay_color_space, color, "
         "opacity, kind), or None when the block is empty/absent.")
    .def_property_readonly("image_resource_ids", &imageResourceIds,
         "List of the integer IDs of every image resource present.")
    .def("image_resource", &imageResource, py::arg("id"),
         "Raw bytes of the image resource with the given integer ID, or None.")
    .def_property_readonly("icc_profile", &iccProfile,
         "ICC profile (resource 1039) as raw bytes, or None.")
    .def_property_readonly("exif", &exifData,
         "EXIF block (resource 1058) as raw bytes, or None.")
    .def_property_readonly("xmp", &xmpMetadata,
         "XMP metadata (resource 1060) as a UTF-8 str, or None. Use "
         "image_resource(1060) for raw bytes if not valid UTF-8.")
    .def_property_readonly("thumbnail", &thumbnail,
         "Embedded thumbnail (resource 1036/1033) as a dict "
         "(format, width, height, bits, resource_id, data), or None. "
         "For format=='jpeg', data is JFIF JPEG bytes.");

  // Enum-like ints exposed as module attributes for convenience
  m.attr("LAYER_TYPE_NORMAL") = (int)psd::LAYER_TYPE_NORMAL;
  m.attr("LAYER_TYPE_HIDDEN") = (int)psd::LAYER_TYPE_HIDDEN;
  m.attr("LAYER_TYPE_FOLDER") = (int)psd::LAYER_TYPE_FOLDER;
  m.attr("LAYER_TYPE_ADJUST") = (int)psd::LAYER_TYPE_ADJUST;
  m.attr("LAYER_TYPE_FILL")   = (int)psd::LAYER_TYPE_FILL;

  m.attr("COLOR_MODE_BITMAP")       = (int)psd::COLOR_MODE_BITMAP;
  m.attr("COLOR_MODE_GRAYSCALE")    = (int)psd::COLOR_MODE_GRAYSCALE;
  m.attr("COLOR_MODE_INDEXED")      = (int)psd::COLOR_MODE_INDEXED;
  m.attr("COLOR_MODE_RGB")          = (int)psd::COLOR_MODE_RGB;
  m.attr("COLOR_MODE_CMYK")         = (int)psd::COLOR_MODE_CMYK;
  m.attr("COLOR_MODE_MULTICHANNEL") = (int)psd::COLOR_MODE_MULTICHANNEL;
  m.attr("COLOR_MODE_DUOTONE")      = (int)psd::COLOR_MODE_DUOTONE;
  m.attr("COLOR_MODE_LAB")          = (int)psd::COLOR_MODE_LAB;
}
