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

#include <fstream>
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
    d["name"]    = py::cast(c.name);
    d["comment"] = py::cast(c.comment);
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
  m.doc() = "psdparse: PSD reader (Boost-free, no kirikiri deps).";

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
    .def_readonly("fill_opacity",  &psd::LayerInfo::fill_opacity)
    .def_readonly("blend_mode",    &psd::LayerInfo::blendMode)
    .def_readonly("layer_type",    &psd::LayerInfo::layerType)
    .def_readonly("layer_id",      &psd::LayerInfo::layerId)
    .def_readonly("channels",      &psd::LayerInfo::channels)
    .def_property_readonly("name", [](const psd::LayerInfo &l) {
        return l.extraData.layerName;  // std::string (raw bytes, original encoding)
    })
    .def_property_readonly("name_unicode", [](const psd::LayerInfo &l) {
        return u16ToStr(l.layerNameUnicode);
    })
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
    .def_property_readonly("sheet_color", &layerSheetColor,
        "Layer-panel color label ('lclr') as {'index', 'name'} (0/'none' .. "
        "11/'fuschia'), or None when the layer carries no lclr block.")
    .def_property_readonly("info_keys", &layerInfoKeys,
        "List of the 4cc keys of every additional-layer-info block present.")
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
         "(this is the code path the kirikiri plugin will use on top of "
         "iTJSBinaryStream).")
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
         "the list after removal). Reorders draw order on the next save().")
    .def("duplicate_layer",
         [](psd::PSDFile &self, int index) {
            int r = self.duplicateLayer(index);
            if (r < 0) throw std::out_of_range("layer index out of range");
            return r;
         },
         py::arg("index"),
         "Duplicate the layer at `index` (inserted right after it). Returns the "
         "new layer's index. The copy shares the source's pixel bytes lazily.")
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
         "before save()). Assumes matching color mode and bit depth.")
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
