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
#include "psdfx.h"

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
  // 寸法がデータ量に見合わない (壊れた) ときは確保する前に断る
  if (!self.canDecodeMergedImage())
    throw std::runtime_error("merged image size does not match its data (damaged file?)");
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
    rd["leading"]          = r.autoLeading ? py::object(py::none()) : py::object(py::float_(r.leading)); // px, None = auto
    rd["baseline_shift"]   = r.baselineShift;      // px
    rd["strikethrough"]    = r.strikethrough;
    rd["font_caps"]        = r.fontCaps;           // 0 normal / 1 small caps / 2 all caps
    rd["font_baseline"]    = r.fontBaseline;       // 0 normal / 1 superscript / 2 subscript
    rd["horizontal_scale"] = r.horizontalScale;    // 1.0 = 100%
    rd["vertical_scale"]   = r.verticalScale;
    rd["ligatures"]        = r.ligatures;
    runs.append(rd);
  }
  d["runs"] = runs;
  py::list paras;
  for (const auto &p : t.paragraphs) {
    py::dict pd;
    pd["length"]            = p.length;          // UTF-16 code units
    pd["justification"]     = p.justification;   // 0=left 1=right 2=center
    pd["first_line_indent"] = p.firstLineIndent; // px
    pd["start_indent"]      = p.startIndent;
    pd["end_indent"]        = p.endIndent;
    pd["space_before"]      = p.spaceBefore;
    pd["space_after"]       = p.spaceAfter;
    pd["auto_leading"]      = p.autoLeading;     // multiplier for auto leading
    pd["hyphenate"]         = p.hyphenate;
    paras.append(pd);
  }
  d["paragraphs"] = paras;                    // 段落別行揃え (段落=改行区切り)
  if (t.warp.present) {
    py::dict w;
    w["style"]                 = t.warp.style;
    w["value"]                 = t.warp.value;
    w["horizontal_distortion"] = t.warp.perspective;
    w["vertical_distortion"]   = t.warp.perspectiveOther;
    w["rotate"]                = t.warp.rotate == "Vrtc" ? "vertical" : "horizontal";
    d["warp"] = w;
  } else {
    d["warp"] = py::none();
  }
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

// Path records (vector masks, saved / work paths) -> dict. Coordinates are
// stored as fractions of the document size; they are returned in document
// pixels as (x, y) tuples.
py::dict pathToPy(const psd::PathData &pd, double w, double h) {
  auto pt = [&](const psd::PathPoint &p) { return py::make_tuple(p.x * w, p.y * h); };
  py::list subs;
  for (const auto &sp : pd.subpaths) {
    py::list knots;
    for (const auto &k : sp.knots) {
      py::dict kd;
      kd["anchor"]    = pt(k.anchor);
      kd["preceding"] = pt(k.preceding);
      kd["leaving"]   = pt(k.leaving);
      kd["linked"]    = k.linked;
      knots.append(kd);
    }
    py::dict sd;
    sd["closed"]    = sp.closed;
    sd["operation"] = sp.operation;
    sd["index"]     = sp.index;
    sd["knots"]     = knots;
    subs.append(sd);
  }
  py::dict d;
  d["subpaths"]     = subs;
  d["initial_fill"] = pd.initialFill < 0 ? py::object(py::none()) : py::object(py::int_(pd.initialFill));
  if (pd.hasClipboard) {
    py::dict c;
    c["top"]        = pd.clipboardTop;
    c["left"]       = pd.clipboardLeft;
    c["bottom"]     = pd.clipboardBottom;
    c["right"]      = pd.clipboardRight;
    c["resolution"] = pd.clipboardResolution;
    d["clipboard"] = c;
  } else {
    d["clipboard"] = py::none();
  }
  return d;
}

// Layer vector mask ('vmsk', or 'vsms' on shape layers).
py::object layerVectorMask(const psd::LayerInfo &l) {
  const psd::VectorMask &vm = l.vectorMask;
  if (!vm.present || !l.owner) return py::none();
  const psd::Header &h = l.owner->header;
  py::dict d;
  char k[5] = { (char)((vm.key >> 24) & 0xff), (char)((vm.key >> 16) & 0xff),
                (char)((vm.key >> 8) & 0xff), (char)(vm.key & 0xff), 0 };
  d["key"]        = std::string(k);
  d["inverted"]   = vm.inverted();
  d["not_linked"] = vm.notLinked();
  d["disabled"]   = vm.disabled();
  d["path"]       = pathToPy(vm.path, h.width, h.height);
  return std::move(d);
}

// --- シェイプ ('vscg' / 'vstk' / 'vogk') とパスのラスタライズ ------------------

py::dict descToPy(psd::Descriptor *d);

py::object fillKindName(int k) {
  switch (k) {
  case 'SoCo': return py::str("solid");
  case 'GdFl': return py::str("gradient");
  case 'PtFl': return py::str("pattern");
  default:     return py::none();
  }
}

py::object layerShape(const psd::LayerInfo &l) {
  psd::ShapeInfo si;
  if (!psd::decodeShape(l, si)) return py::none();
  const double dpi = l.owner ? l.owner->header.hres : 72.0;
  py::dict d;
  d["fill_enabled"]   = si.hasStroke ? si.stroke.fillEnabled : true;
  d["stroke_enabled"] = si.hasStroke && si.stroke.strokeEnabled;
  if (si.hasFill) {
    py::dict f;
    f["kind"]       = fillKindName(si.fillKind);
    f["descriptor"] = si.fill ? py::object(descToPy(si.fill.get())) : py::object(py::none());
    d["fill"] = f;
  } else {
    d["fill"] = py::none();
  }
  if (si.hasStroke) {
    const psd::ShapeStroke &s = si.stroke;
    const double w = s.width * (s.widthInPoints ? (dpi > 0 ? dpi : 72.0) / 72.0 : 1.0);
    static const char *kAlign[] = { "outside", "inside", "center" };
    static const char *kCap[] = { "butt", "round", "square" };
    static const char *kJoin[] = { "miter", "round", "bevel" };
    py::dict st;
    st["width"]        = w;
    st["alignment"]    = kAlign[std::min(2, std::max(0, s.alignment))];
    st["cap"]          = kCap[std::min(2, std::max(0, s.cap))];
    st["join"]         = kJoin[std::min(2, std::max(0, s.join))];
    st["miter_limit"]  = s.miterLimit;
    py::list dashes;
    for (double v : s.dashes) dashes.append(v * w);
    st["dashes"]       = dashes;
    st["dash_offset"]  = s.dashOffset * w;
    st["opacity"]      = s.opacity;
    st["blend_mode"]   = s.blendMode;
    st["content_kind"] = fillKindName(s.contentKind);
    st["content"]      = s.content ? py::object(descToPy(s.content.get())) : py::object(py::none());
    d["stroke"] = st;
  } else {
    d["stroke"] = py::none();
  }
  py::list origins;
  for (const auto &o : si.origins) {
    py::dict od;
    const char *name = o.type == 1 ? "rectangle" : o.type == 2 ? "rounded_rectangle"
                     : o.type == 4 ? "line" : o.type == 5 ? "ellipse" : nullptr;
    od["type"]    = name ? py::object(py::str(name)) : py::object(py::none());
    od["type_id"] = o.type;
    od["index"]   = o.index;
    od["box"]     = o.hasBox ? py::object(py::make_tuple(o.box[0], o.box[1], o.box[2], o.box[3]))
                             : py::object(py::none());
    od["radii"]   = o.hasRadii ? py::object(py::make_tuple(o.radii[0], o.radii[1], o.radii[2], o.radii[3]))
                               : py::object(py::none());
    od["line"]    = o.hasLine ? py::object(py::make_tuple(o.line[0], o.line[1], o.line[2], o.line[3]))
                              : py::object(py::none());
    od["line_weight"] = o.hasLine ? py::object(py::float_(o.lineWeight)) : py::object(py::none());
    od["invalidated"] = o.invalidated;
    origins.append(od);
  }
  d["origins"] = origins;
  if (l.vectorMask.present && l.owner)
    d["path"] = pathToPy(l.vectorMask.path, l.owner->header.width, l.owner->header.height);
  else
    d["path"] = py::none();
  return std::move(d);
}

py::object psdShapeMask(psd::PSDFile &self, int index, const std::string &part) {
  if (index < 0 || index >= (int)self.layerList.size())
    throw std::out_of_range("layer index out of range");
  psd::ShapePart p;
  if (part == "fill") p = psd::SHAPE_PART_FILL;
  else if (part == "stroke") p = psd::SHAPE_PART_STROKE;
  else if (part == "both") p = psd::SHAPE_PART_BOTH;
  else throw std::invalid_argument("part must be 'fill', 'stroke' or 'both'");
  std::vector<uint8_t> m;
  int left = 0, top = 0, w = 0, h = 0;
  bool ok;
  {
    py::gil_scoped_release release;
    ok = self.shapeMask(index, p, m, left, top, w, h);
  }
  if (!ok) return py::none();
  return py::make_tuple(py::bytes((const char *)m.data(), m.size()), left, top, w, h);
}

// Python のパス (layer.vector_mask['path'] / PSDFile.paths[i]['path'] と同じ形、
// またはサブパスの list) を psdfx の形へ。座標は文書ピクセル。
struct PyPath {
  std::vector<std::vector<psdfx_knot>> knots;
  std::vector<psdfx_subpath> subs;
  int initialFill = 0;
};

void pyToPath(py::handle path, PyPath &out) {
  py::object subs;
  if (py::isinstance<py::dict>(path)) {
    py::dict d = py::reinterpret_borrow<py::dict>(path);
    if (!d.contains("subpaths")) throw std::invalid_argument("path dict needs 'subpaths'");
    subs = d["subpaths"];
    if (d.contains("initial_fill") && !d["initial_fill"].is_none())
      out.initialFill = d["initial_fill"].cast<int>() == 1 ? 1 : 0;
  } else {
    subs = py::reinterpret_borrow<py::object>(path);
  }
  auto xy = [](py::handle t, double &x, double &y) {
    auto s = py::reinterpret_borrow<py::sequence>(t);
    if (py::len(s) != 2) throw std::invalid_argument("points must be (x, y)");
    x = s[0].cast<double>(); y = s[1].cast<double>();
  };
  std::vector<int> closed, ops;
  for (py::handle sp : subs) {
    py::dict sd = py::reinterpret_borrow<py::dict>(sp);
    std::vector<psdfx_knot> ks;
    for (py::handle kh : sd["knots"]) {
      psdfx_knot k{};
      if (py::isinstance<py::dict>(kh)) {
        py::dict kd = py::reinterpret_borrow<py::dict>(kh);
        xy(kd["anchor"], k.x, k.y);
        k.in_x = k.x; k.in_y = k.y; k.out_x = k.x; k.out_y = k.y;
        if (kd.contains("preceding")) xy(kd["preceding"], k.in_x, k.in_y);
        if (kd.contains("leaving")) xy(kd["leaving"], k.out_x, k.out_y);
      } else {   // (x, y) だけなら直線の頂点
        xy(kh, k.x, k.y);
        k.in_x = k.out_x = k.x; k.in_y = k.out_y = k.y;
      }
      ks.push_back(k);
    }
    out.knots.push_back(std::move(ks));
    closed.push_back(sd.contains("closed") ? (sd["closed"].cast<bool>() ? 1 : 0) : 1);
    ops.push_back(sd.contains("operation") ? sd["operation"].cast<int>() : -1);
  }
  for (size_t i = 0; i < out.knots.size(); i++)
    out.subs.push_back({ out.knots[i].data(), (int)out.knots[i].size(), closed[i], ops[i] });
}

void checkSize(int w, int h) {
  if (w <= 0 || h <= 0) throw std::invalid_argument("width and height must be positive");
  if ((int64_t)w * h > (1LL << 28)) throw std::invalid_argument("raster too large");
}

py::list pyFlattenPath(py::object path, double tolerance) {
  PyPath p;
  pyToPath(path, p);
  py::list out;
  for (const auto &s : p.subs) {
    const int n = psdfx_flatten_subpath(&s, tolerance, nullptr, 0);
    std::vector<double> xy((size_t)n * 2);
    psdfx_flatten_subpath(&s, tolerance, xy.data(), n);
    py::list pts;
    for (int i = 0; i < n; i++) pts.append(py::make_tuple(xy[(size_t)i * 2], xy[(size_t)i * 2 + 1]));
    py::dict d;
    d["closed"] = s.closed != 0;
    d["operation"] = s.operation;
    d["points"] = pts;
    out.append(d);
  }
  return out;
}

py::bytes pyRasterizePath(py::object path, int width, int height, double left, double top) {
  checkSize(width, height);
  PyPath p;
  pyToPath(path, p);
  std::vector<uint8_t> m((size_t)width * height);
  {
    py::gil_scoped_release release;
    psdfx_fill_path(p.subs.data(), (int)p.subs.size(), p.initialFill, m.data(), width, height,
                    width, left, top);
  }
  return py::bytes((const char *)m.data(), m.size());
}

int pickName(const std::string &v, std::initializer_list<const char *> names, const char *what) {
  int i = 0;
  for (const char *n : names) { if (v == n) return i; i++; }
  throw std::invalid_argument(std::string("unknown ") + what + ": " + v);
}

py::bytes pyStrokePath(py::object path, int width, int height, double left, double top,
                       double lineWidth, const std::string &alignment, const std::string &cap,
                       const std::string &join, double miterLimit, std::vector<double> dashes,
                       double dashOffset) {
  checkSize(width, height);
  PyPath p;
  pyToPath(path, p);
  psdfx_stroke_style st{};
  st.width = lineWidth;
  const int a = pickName(alignment, { "outside", "inside", "center" }, "alignment");
  st.alignment = a == 0 ? PSDFX_STROKE_OUTSIDE : a == 1 ? PSDFX_STROKE_INSIDE : PSDFX_STROKE_CENTER;
  st.cap = pickName(cap, { "butt", "round", "square" }, "cap");
  st.join = pickName(join, { "miter", "round", "bevel" }, "join");
  st.miter_limit = miterLimit;
  st.dashes = dashes.empty() ? nullptr : dashes.data();
  st.dash_count = (int)dashes.size();
  st.dash_offset = dashOffset;
  std::vector<uint8_t> m((size_t)width * height);
  {
    py::gil_scoped_release release;
    psdfx_stroke_path(p.subs.data(), (int)p.subs.size(), p.initialFill, &st, m.data(), width,
                      height, width, left, top);
  }
  return py::bytes((const char *)m.data(), m.size());
}

// ファイル由来のバイト列 (ID や 4 文字コード) を str へ。UTF-8 として読めない
// バイト (壊れたファイル) は置換文字にして、例外にはしない。
py::object lossyStr(const std::string &b) {
  return py::reinterpret_steal<py::object>(
      PyUnicode_DecodeUTF8(b.data(), (Py_ssize_t)b.size(), "replace"));
}

py::dict descToPy(psd::Descriptor *d);

// AdjustmentInfo (名前付きの値の入れ物) を dict へ: {"type", "key", <named values>}。
py::dict namedValuesToPy(const psd::AdjustmentInfo &a) {
  py::dict d;
  d["type"] = a.type;
  char k[5] = { (char)((a.key >> 24) & 0xff), (char)((a.key >> 16) & 0xff),
                (char)((a.key >> 8) & 0xff), (char)(a.key & 0xff), 0 };
  d["key"] = std::string(k);
  for (const auto &kv : a.scalars) {
    double v = kv.second;
    if (v == (double)(long long)v) d[kv.first.c_str()] = (long long)v;
    else d[kv.first.c_str()] = v;
  }
  auto toList = [](const std::vector<double> &vs) {
    py::list out;
    for (double v : vs) {
      if (v == (double)(long long)v) out.append((long long)v); else out.append(v);
    }
    return out;
  };
  for (const auto &kv : a.arrays) d[kv.first.c_str()] = toList(kv.second);
  for (const auto &kv : a.tables) {
    py::list rows;
    for (const auto &row : kv.second) {
      if (a.type == "curves" && kv.first == "points") {
        py::list pts;                       // (入力, 出力) の組にして返す
        for (size_t i = 0; i + 1 < row.size(); i += 2)
          pts.append(py::make_tuple((long long)row[i], (long long)row[i + 1]));
        rows.append(pts);
      } else {
        rows.append(toList(row));
      }
    }
    d[kv.first.c_str()] = rows;
  }
  for (const auto &kv : a.text) d[kv.first.c_str()] = lossyStr(kv.second);   // 4 文字コード
  for (const auto &kv : a.unicode) d[kv.first.c_str()] = u16ToStr(kv.second);
  if (a.descriptor) d["descriptor"] = descToPy(a.descriptor.get());
  if (!a.valid) d["incomplete"] = true;
  return d;
}

// Adjustment layer parameters as a flat dict: {"type", "key", <named values>}.
py::object layerAdjustment(const psd::LayerInfo &l) {
  psd::AdjustmentInfo a;
  if (!psd::decodeAdjustment(l, a)) return py::none();
  return namedValuesToPy(a);
}

// Legacy layer effects ('lrFX'): {effect type: {named values}}.
py::object layerLegacyEffects(const psd::LayerInfo &l) {
  std::vector<psd::AdjustmentInfo> fx;
  if (!psd::decodeLegacyEffects(l, fx)) return py::none();
  py::dict d;
  for (const auto &e : fx) {
    py::dict v = namedValuesToPy(e);
    v.attr("pop")("type");
    d[e.type.c_str()] = v;
  }
  return std::move(d);
}

// Smart object placement ('SoLd' / 'SoLE', or the older 'PlLd').
py::object layerSmartObject(const psd::LayerInfo &l) {
  const psd::SmartObjectInfo &so = l.smartObject;
  if (!so.present) return py::none();
  py::dict d;
  char k[5] = { (char)((so.key >> 24) & 0xff), (char)((so.key >> 16) & 0xff),
                (char)((so.key >> 8) & 0xff), (char)(so.key & 0xff), 0 };
  d["key"]         = std::string(k);
  d["uuid"]        = lossyStr(so.uuid);
  d["placed_id"]   = so.placedId.empty() ? py::object(py::none()) : lossyStr(so.placedId);
  d["page"]        = so.page;
  d["total_pages"] = so.totalPages;
  d["anti_alias"]  = so.antiAlias;
  d["placed_type"] = so.placedType;
  if (so.hasTransform) {
    py::list q;
    for (int i = 0; i < 4; i++) q.append(py::make_tuple(so.transform[i * 2], so.transform[i * 2 + 1]));
    d["transform"] = q;
  } else {
    d["transform"] = py::none();
  }
  d["size"] = so.hasSize ? py::object(py::make_tuple(so.width, so.height)) : py::object(py::none());
  d["filters"] = so.hasFilters ? py::object(py::bool_(so.filtersEnabled)) : py::object(py::none());
  // 中身のファイルが文書内にあれば、その linked_files の添字
  int idx = -1;
  if (l.owner && !so.uuid.empty()) {
    const auto &files = l.owner->linkedFiles;
    for (size_t i = 0; i < files.size(); i++)
      if (files[i].uuid == so.uuid) { idx = (int)i; break; }
  }
  d["linked_file"] = idx >= 0 ? py::object(py::int_(idx)) : py::object(py::none());
  return std::move(d);
}

// Layer-based composite of the document.
py::tuple psdComposite(psd::PSDFile &self, bool effects, py::object background) {
  psd::CompositeOptions opt;
  opt.effects = effects;
  if (!background.is_none()) {
    auto seq = background.cast<py::sequence>();
    if (py::len(seq) != 3) throw std::invalid_argument("background must be (r, g, b)");
    opt.background = true;
    for (int i = 0; i < 3; i++) opt.backgroundColor[i] = (uint8_t)seq[(size_t)i].cast<int>();
  }
  std::vector<uint8_t> out;
  psd::CompositeStats st;
  {
    py::gil_scoped_release release;
    if (!self.compositeImage(out, opt, &st))
      throw std::runtime_error("cannot composite this document (size)");
  }
  py::dict s;
  s["skipped_adjustments"]   = st.skippedAdjustments;
  s["unsupported_clip_base"] = st.unsupportedClipBase;
  s["unsupported_effects"]   = st.unsupportedEffects;
  return py::make_tuple(py::bytes((const char *)out.data(), out.size()), s);
}

py::object psdRenderLayer(psd::PSDFile &self, int index, bool effects) {
  if (index < 0 || index >= (int)self.layerList.size())
    throw std::out_of_range("layer index out of range");
  psd::CompositeOptions opt;
  opt.effects = effects;
  std::vector<uint8_t> out;
  int left = 0, top = 0, w = 0, h = 0;
  bool ok;
  {
    py::gil_scoped_release release;
    ok = self.renderLayer(index, out, left, top, w, h, opt);
  }
  if (!ok) return py::none();
  return py::make_tuple(py::bytes((const char *)out.data(), out.size()), left, top, w, h);
}

py::list psdAnnotations(psd::PSDFile &self) {
  py::list out;
  for (const auto &a : self.annotations) {
    py::dict d;
    d["kind"]       = a.kind == "txtA" ? "text" : a.kind == "sndA" ? "sound" : a.kind;
    d["open"]       = a.open;
    d["icon_rect"]  = py::make_tuple(a.iconRect[0], a.iconRect[1], a.iconRect[2], a.iconRect[3]);
    d["popup_rect"] = py::make_tuple(a.popupRect[0], a.popupRect[1], a.popupRect[2], a.popupRect[3]);
    d["color_space"] = a.colorSpace;
    d["color"]      = py::make_tuple(a.color[0], a.color[1], a.color[2], a.color[3]);
    d["author"]     = py::bytes(a.author);
    d["name"]       = py::bytes(a.name);
    d["mod_date"]   = py::bytes(a.modDate);
    d["text"]       = a.kind == "txtA" ? u16ToStr(a.text) : py::object(py::none());
    d["data_size"]  = a.dataSize;
    out.append(d);
  }
  return out;
}

py::object layerArtboard(const psd::LayerInfo &l) {
  const psd::ArtboardInfo &a = l.artboard;
  if (!a.present) return py::none();
  py::dict d;
  d["rect"]            = py::make_tuple(a.left, a.top, a.right, a.bottom);
  d["preset_name"]     = u16ToStr(a.presetName);
  d["background_type"] = a.backgroundType;
  d["color"] = a.hasColor ? py::object(py::make_tuple(a.color[0], a.color[1], a.color[2]))
                          : py::object(py::none());
  return std::move(d);
}

py::list psdAlphaChannels(psd::PSDFile &self) {
  static const char *kKinds[] = { "selected", "masked", "spot" };
  py::list out;
  for (const auto &a : self.alphaChannels) {
    py::dict d;
    d["plane"] = a.plane;
    d["name"]  = u16ToStr(a.name);
    if (a.hasDisplay) {
      d["color_space"] = a.colorSpace;
      d["color"]       = py::make_tuple(a.color[0], a.color[1], a.color[2], a.color[3]);
      d["opacity"]     = a.opacity;
      d["kind"]        = (a.kind >= 0 && a.kind <= 2) ? kKinds[a.kind] : "unknown";
    } else {
      d["color_space"] = py::none(); d["color"] = py::none();
      d["opacity"] = py::none();     d["kind"] = py::none();
    }
    out.append(d);
  }
  return out;
}

py::object psdMergedChannel(psd::PSDFile &self, int plane) {
  if (plane < 0 || plane >= self.header.channels)
    throw std::out_of_range("channel out of range");
  std::vector<uint8_t> gray;
  if (!self.getMergedChannel(plane, gray)) return py::none();
  return py::bytes((const char *)gray.data(), gray.size());
}

py::list psdPatterns(psd::PSDFile &self) {
  py::list out;
  for (const auto &pt : self.patterns) {
    py::dict d;
    d["id"]     = lossyStr(pt.id);
    d["name"]   = u16ToStr(pt.name);
    d["mode"]   = pt.mode;
    d["width"]  = pt.width;
    d["height"] = pt.height;
    char k[5] = { (char)((pt.blockKey >> 24) & 0xff), (char)((pt.blockKey >> 16) & 0xff),
                  (char)((pt.blockKey >> 8) & 0xff), (char)(pt.blockKey & 0xff), 0 };
    d["block"] = std::string(k);
    out.append(d);
  }
  return out;
}

py::object psdPatternImage(psd::PSDFile &self, py::object which) {
  int index = -1;
  if (py::isinstance<py::int_>(which)) {
    index = which.cast<int>();
  } else {
    std::string id = which.cast<std::string>();
    for (size_t i = 0; i < self.patterns.size(); i++)
      if (self.patterns[i].id == id) { index = (int)i; break; }
  }
  if (index < 0 || index >= (int)self.patterns.size())
    throw std::out_of_range("no such pattern");
  std::vector<uint8_t> bgra;
  int w = 0, h = 0;
  if (!self.getPatternImage(index, bgra, w, h)) return py::none();
  return py::make_tuple(py::bytes((const char *)bgra.data(), bgra.size()), w, h);
}

py::list psdLinkedFiles(psd::PSDFile &self) {
  py::list out;
  for (const auto &f : self.linkedFiles) {
    py::dict d;
    d["kind"] = f.kind == "liFD" ? "data" : f.kind == "liFE" ? "external"
              : f.kind == "liFA" ? "alias" : "unknown";
    d["uuid"]      = lossyStr(f.uuid);
    d["name"]      = u16ToStr(f.fileName);
    d["file_type"] = py::bytes(f.fileType);
    d["creator"]   = py::bytes(f.creator);
    d["size"]      = f.dataSize;
    d["has_data"]  = f.hasData;
    d["version"]   = f.version;
    char k[5] = { (char)((f.blockKey >> 24) & 0xff), (char)((f.blockKey >> 16) & 0xff),
                  (char)((f.blockKey >> 8) & 0xff), (char)(f.blockKey & 0xff), 0 };
    d["block"] = std::string(k);
    out.append(d);
  }
  return out;
}

py::object psdLinkedFileData(psd::PSDFile &self, py::object which) {
  int index = -1;
  if (py::isinstance<py::int_>(which)) {
    index = which.cast<int>();
  } else {
    std::string uuid = which.cast<std::string>();
    for (size_t i = 0; i < self.linkedFiles.size(); i++)
      if (self.linkedFiles[i].uuid == uuid) { index = (int)i; break; }
  }
  if (index < 0 || index >= (int)self.linkedFiles.size())
    throw std::out_of_range("no such linked file");
  std::string out;
  if (!self.getLinkedFileData(index, out)) return py::none();
  return py::bytes(out);
}

// Saved paths (image resources 2000-2997) and the work path (1025).
py::list psdPaths(psd::PSDFile &self) {
  py::list out;
  for (const auto &sp : self.savedPaths) {
    py::dict d;
    d["id"]   = sp.id;
    d["kind"] = sp.id == 1025 ? "work" : "saved";
    d["name"] = py::bytes(sp.name);
    d["unicode_name"] = sp.hasNameUnicode ? u16ToStr(sp.nameUnicode) : py::object(py::none());
    d["path"] = pathToPy(sp.path, self.header.width, self.header.height);
    out.append(d);
  }
  return out;
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
  d["version"]    = s.version;
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
  if (auto *x = dynamic_cast<psd::DescriptorLargeInteger*>(it)) return py::cast(x->val);
  if (auto *x = dynamic_cast<psd::DescriptorUnitFloats*>(it)) {
    py::dict u; u["values"] = x->values; u["unit"] = descUnitName(x->unit);
    return std::move(u);
  }
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
    case 'PlLd': {
      // 'plcL' + ver + uuid (Pascal, 詰め物なし) + page/total/antiAlias/type +
      // transform (8 doubles) + warp ver + descVer。uuid の長さで位置が変わる。
      skip = 0;
      for (const auto &a : l.extraData.additionalLayers) {
        if (a.key != key || !a.data) continue;
        psd::IteratorBase *rd = a.data->clone();
        rd->init();
        rd->advance(8);
        int n = rd->getCh();
        delete rd;
        if (n >= 0) skip = 8 + 1 + n + 16 + 64 + 8;
        break;
      }
      break;
    }
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
    const int bodyEnd = rd->size() - rd->rest();   // 元の descriptor の終わり
    // descriptor の後ろに続いていたバイト列 (詰め物、またはブロック固有の続き)
    std::vector<uint8_t> tail((size_t)(rd->rest() > 0 ? rd->rest() : 0));
    if (!tail.empty()) rd->getData(tail.data(), (int)tail.size());
    const int origTotal = rd->size();
    delete rd;
    mergeDictIntoDescriptor(&desc, changes);
    std::vector<uint8_t> buf;
    psd::MemoryWriter w(buf);
    if (!prefix.empty()) w.putData(prefix.data(), prefix.size());
    psd::writeDescriptorBody(w, &desc);
    // 後ろの続きはそのまま残す。ただし詰め物 (全部 0) だけで、descriptor の長さが
    // 変わったときは、元と同じ揃え方 (4 の倍数だったなら 4 の倍数) で詰め直す。
    bool padOnly = true;
    for (uint8_t c : tail) if (c != 0) padOnly = false;
    if (!padOnly || (int)buf.size() == bodyEnd) {
      buf.insert(buf.end(), tail.begin(), tail.end());
    } else if ((origTotal & 3) == 0) {
      while (buf.size() & 3u) buf.push_back(0);
    }
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

// 0.13 で足した文字書式 (行送り / ベースライン / 取り消し線 / 大文字化 / 上付き・
// 下付き / 比率 / 合字)。get(name) が未指定なら空ハンドルを返す。
template <class Get>
bool fillRunStyleExtras(psd::RunStyleEdit &e, Get get) {
  bool any = false;
  auto given = [](py::handle h) { return (bool)h && !h.is_none(); };
  py::handle h;
  h = get("leading");
  if (given(h)) {
    if (py::isinstance<py::str>(h)) {
      if (h.cast<std::string>() != "auto")
        throw std::invalid_argument("leading must be a number (px) or 'auto'");
      e.hasAutoLeading = true; e.autoLeading = true;
    } else {
      e.hasLeading = true; e.leading = h.cast<double>();
    }
    any = true;
  }
  if (given(h = get("baseline_shift")))   { e.hasBaselineShift = true; e.baselineShift = h.cast<double>(); any = true; }
  if (given(h = get("strikethrough")))    { e.hasStrikethrough = true; e.strikethrough = h.cast<bool>(); any = true; }
  if (given(h = get("font_caps")))        { e.hasFontCaps = true; e.fontCaps = h.cast<int>(); any = true; }
  if (given(h = get("font_baseline")))    { e.hasFontBaseline = true; e.fontBaseline = h.cast<int>(); any = true; }
  if (given(h = get("horizontal_scale"))) { e.hasHorizontalScale = true; e.horizontalScale = h.cast<double>(); any = true; }
  if (given(h = get("vertical_scale")))   { e.hasVerticalScale = true; e.verticalScale = h.cast<double>(); any = true; }
  if (given(h = get("ligatures")))        { e.hasLigatures = true; e.ligatures = h.cast<bool>(); any = true; }
  return any;
}

// 段落書式 (行揃え / インデント / アキ / 自動行送り / ハイフネーション)。
template <class Get>
bool fillParagraphStyle(psd::TextParagraphSpec &s, Get get) {
  bool any = false;
  auto given = [](py::handle h) { return (bool)h && !h.is_none(); };
  py::handle h;
  if (given(h = get("justification")))     { s.hasJustification = true; s.justification = h.cast<int>(); any = true; }
  if (given(h = get("first_line_indent"))) { s.hasFirstLineIndent = true; s.firstLineIndent = h.cast<double>(); any = true; }
  if (given(h = get("start_indent")))      { s.hasStartIndent = true; s.startIndent = h.cast<double>(); any = true; }
  if (given(h = get("end_indent")))        { s.hasEndIndent = true; s.endIndent = h.cast<double>(); any = true; }
  if (given(h = get("space_before")))      { s.hasSpaceBefore = true; s.spaceBefore = h.cast<double>(); any = true; }
  if (given(h = get("space_after")))       { s.hasSpaceAfter = true; s.spaceAfter = h.cast<double>(); any = true; }
  if (given(h = get("auto_leading")))      { s.hasAutoLeading = true; s.autoLeading = h.cast<double>(); any = true; }
  if (given(h = get("hyphenate")))         { s.hasHyphenate = true; s.hyphenate = h.cast<bool>(); any = true; }
  return any;
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
    fillRunStyleExtras(spec.style, [&](const char *k) { return dictGet(d, k); });
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
    fillParagraphStyle(spec, [&](const char *k) { return dictGet(d, k); });
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
  // mask モードはマスク矩形の大きさで返る (レイヤ矩形と違うことがある)
  int w = lay.width, h = lay.height;
  if (m == psd::IMAGE_MODE_MASK) {
    w = lay.extraData.layerMask.width;
    h = lay.extraData.layerMask.height;
  }
  if (w <= 0 || h <= 0 || !self.canDecodeLayerImage(lay, m)) return py::bytes();
  size_t n = (size_t)w * (size_t)h * 4;
  std::string buf(n, '\0');
  if (!self.getLayerImage(lay, buf.data(), psd::BGRA_LE, w * 4, m)) return py::bytes();
  return py::bytes(buf);
}

} // namespace

PYBIND11_MODULE(psdparse, m) {
  m.doc() = "psdparse: PSD reader/writer (pure C++17, zlib only).";

  // Internal: parse + re-serialize EngineData for byte-exact round-trip tests.
  m.def("flatten_path", &pyFlattenPath, py::arg("path"), py::arg("tolerance") = 0.1,
        "Flatten a path (layer.vector_mask['path'], PSDFile.paths[i]['path'], or a "
        "list of subpath dicts with 'knots' / 'closed' / 'operation') into "
        "polylines: a list of {'closed', 'operation', 'points': [(x, y), ...]}. "
        "tolerance is the maximum distance from the curve in pixels.");
  m.def("rasterize_path", &pyRasterizePath, py::arg("path"), py::arg("width"),
        py::arg("height"), py::arg("left") = 0.0, py::arg("top") = 0.0,
        "Fill a path into width x height 8-bit coverage (bytes, anti-aliased). "
        "The raster's top-left is (left, top) in path coordinates. Subpaths are "
        "combined the way Photoshop combines shape operations (operation -1 joins "
        "the previous subpath; 0 xor, 1 union, 2 subtract, 3 intersect); "
        "'initial_fill' 1 starts from a filled raster. Knots may also be plain "
        "(x, y) tuples for straight segments.");
  m.def("stroke_path", &pyStrokePath, py::arg("path"), py::arg("width"), py::arg("height"),
        py::arg("left") = 0.0, py::arg("top") = 0.0, py::arg("line_width") = 1.0,
        py::arg("alignment") = "center", py::arg("cap") = "butt", py::arg("join") = "miter",
        py::arg("miter_limit") = 4.0, py::arg("dashes") = std::vector<double>(),
        py::arg("dash_offset") = 0.0,
        "Stroke a path into width x height 8-bit coverage (bytes, anti-aliased). "
        "alignment 'inside' / 'outside' puts the whole line width on one side of "
        "closed subpaths (open subpaths are always centered); cap 'butt' / 'round' "
        "/ 'square'; join 'miter' / 'round' / 'bevel'; miter_limit as a ratio of "
        "the line width; dashes = (on, off, ...) lengths in pixels.");
  m.def("_reserialize_engine_data", [](py::bytes b) -> py::object {
      py::buffer_info info(py::buffer(b).request());
      std::string out;
      if (!psd::reserializeEngineData((const char *)info.ptr, (size_t)info.size, out))
          return py::none();
      return py::bytes(out);
  }, py::arg("data"));

  // Internal: same, for a Txt2 (document Text Engine Data) blob.
  m.def("_reserialize_text_engine_data", [](py::bytes b) -> py::object {
      py::buffer_info info(py::buffer(b).request());
      std::string out;
      if (!psd::reserializeTextEngineData((const char *)info.ptr, (size_t)info.size, out))
          return py::none();
      return py::bytes(out);
  }, py::arg("data"));

  // Internal: list the texts a Txt2 blob carries, in TextIndex order.
  m.def("_list_text_engine_texts", [](py::bytes b) -> py::object {
      py::buffer_info info(py::buffer(b).request());
      std::vector<psd::u16str> texts;
      if (!psd::listTextEngineDataTexts((const char *)info.ptr, (size_t)info.size, texts))
          return py::none();
      py::list out;
      for (const psd::u16str &t : texts) out.append(u16ToStr(t));
      return out;
  }, py::arg("data"));

  // Internal: replace one text inside a Txt2 blob.
  m.def("_edit_text_engine_text", [](py::bytes b, int index, const std::string &text,
                                     std::vector<int> paras, std::vector<int> styles) -> py::object {
      py::buffer_info info(py::buffer(b).request());
      std::string out;
      if (!psd::editTextEngineDataText((const char *)info.ptr, (size_t)info.size, index,
                                       psd::utf8ToU16(text), paras, styles, out))
          return py::none();
      return py::bytes(out);
  }, py::arg("data"), py::arg("index"), py::arg("text"),
     py::arg("paragraph_lengths") = std::vector<int>(),
     py::arg("style_lengths") = std::vector<int>());

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
    .def_readonly("vres",     &psd::Header::vres)   // 垂直解像度 dpi
    .def_property_readonly("is_psb", &psd::Header::isPSB,
         "True for a PSB (large document format, version 2) file.");

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
    .def_property_readonly("name", [](const psd::LayerInfo &l) -> py::object {
        // Pascal 名の生バイトはシステムの文字コード (Shift-JIS や MacRoman) の
        // ことがある。UTF-8 として読めなければ Unicode 名 (luni) を、それも
        // 無ければ置換文字で読む (例外にはしない)。生バイトは name_raw。
        const std::string &raw = l.extraData.layerName;
        PyObject *s = PyUnicode_DecodeUTF8(raw.data(), (Py_ssize_t)raw.size(), nullptr);
        if (s) return py::reinterpret_steal<py::object>(s);
        PyErr_Clear();
        if (!l.layerNameUnicode.empty()) return u16ToStr(l.layerNameUnicode);
        return py::reinterpret_steal<py::object>(
            PyUnicode_DecodeUTF8(raw.data(), (Py_ssize_t)raw.size(), "replace"));
    }, "Layer name from the Pascal name. Decoded as UTF-8; if the bytes are in "
       "another encoding, falls back to name_unicode (or replacement characters). "
       "See name_raw for the bytes.")
    .def_property_readonly("name_raw", [](const psd::LayerInfo &l) {
        return py::bytes(l.extraData.layerName);
    }, "The Pascal layer name as stored (raw bytes in the system encoding).")
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
    .def_property_readonly("is_group",
        [](const psd::LayerInfo &self) {
            return self.layerType == psd::LAYER_TYPE_FOLDER;
        },
        "True when this layer is a folder (layer group).")
    .def_property_readonly("children",
        [](const psd::LayerInfo &self) {
            if (!self.owner) return std::vector<int>();
            const auto &list = self.owner->layerList;
            if (list.empty()) return std::vector<int>();
            const int idx = int(&self - &list[0]);
            if (idx < 0 || idx >= (int)list.size()) return std::vector<int>();
            return self.owner->childIndices(idx);
        },
        "Indices of this layer's direct children, bottom-to-top. Empty for "
        "non-folder layers. The '</Layer group>' divider PSD uses to encode a "
        "group is left out — it is an encoding artifact, not content.")
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
    .def_property_readonly("legacy_effects", &layerLegacyEffects,
        "Old-style layer effects ('lrFX', Photoshop 5) as {type: values}, or None. "
        "Types: common_state, drop_shadow, inner_shadow, outer_glow, inner_glow, "
        "bevel, solid_fill. Colors are [color_space, c0, c1, c2, c3]; blur / "
        "intensity / distance are the stored 32-bit values. Newer files keep the "
        "same effects in 'lfx2' (layer.effects), which Photoshop prefers.")
    .def_property_readonly("adjustment", &layerAdjustment,
        "Adjustment layer parameters as a dict {'type', 'key', ...}, or None. "
        "type is one of levels, curves, hue_saturation, color_balance, "
        "brightness_contrast, selective_color, threshold, posterize, invert, "
        "channel_mixer, photo_filter, exposure, gradient_map, vibrance, "
        "black_white, color_lookup. Binary blocks are decoded into named values; "
        "descriptor-based ones come as 'descriptor'. See docs/PYTHON_API.md.")
    .def_property_readonly("artboard", &layerArtboard,
        "Artboard ('artb', older 'artd' / 'abdd') as {'rect' (left, top, right, "
        "bottom), 'preset_name', 'background_type' (1 white / 2 black / 3 "
        "transparent / 4 other), 'color' (r, g, b) or None}, or None.")
    .def_property_readonly("smart_object", &layerSmartObject,
        "Smart object placement ('SoLd' / 'SoLE', or the older 'PlLd') as "
        "{'key', 'uuid', 'placed_id', 'page', 'total_pages', 'anti_alias', "
        "'placed_type', 'transform' (4 corners (x, y): top-left, top-right, "
        "bottom-right, bottom-left), 'size' ((w, h) of the source) or None, "
        "'filters' (smart filters enabled, None when none), 'linked_file' "
        "(index into PSDFile.linked_files, or None)}, or None.")
    .def_property_readonly("shape", &layerShape,
         "Shape layer data, or None: {'fill_enabled', 'stroke_enabled', 'fill' "
         "({'kind': 'solid'/'gradient'/'pattern', 'descriptor'} from 'vscg', or None), "
         "'stroke' (from 'vstk', or None: {'width' (px), 'alignment' "
         "('inside'/'center'/'outside'), 'cap' ('butt'/'round'/'square'), 'join' "
         "('miter'/'round'/'bevel'), 'miter_limit', 'dashes' and 'dash_offset' (px), "
         "'opacity' (0..1), 'blend_mode', 'content_kind', 'content'}), 'origins' "
         "(live-shape origins from 'vogk': {'type' ('rectangle'/'rounded_rectangle'/"
         "'line'/'ellipse' or None), 'type_id', 'index', 'box', 'radii', 'line', "
         "'line_weight', 'invalidated'}), 'path' (the vector mask path, document "
         "pixels)}. Rasterize with PSDFile.shape_mask(i) or psdparse.rasterize_path / "
         "stroke_path.")
    .def_property_readonly("vector_mask", &layerVectorMask,
        "Vector mask ('vmsk', or 'vsms' on shape layers) as {'key', 'inverted', "
        "'not_linked', 'disabled', 'path'}, or None. 'path' holds 'subpaths' "
        "(each {'closed', 'operation', 'index', 'knots'}), 'initial_fill' and "
        "'clipboard'; knot points ('anchor', 'preceding', 'leaving') are (x, y) "
        "in document pixels.")
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
            if (src_index < 0 || src_index >= (int)src.layerList.size())
              throw std::out_of_range("source layer index out of range");
            int r = self.copyLayerFrom(src, src_index, dest_index);
            if (r < 0)
              throw std::invalid_argument("source must have the same bit depth and the same "
                                          "PSD/PSB format as this file");
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
            py::object underline, py::object font, py::kwargs extra) {
            psd::RunStyleEdit e;
            static const char *kExtra[] = { "leading", "baseline_shift", "strikethrough",
              "font_caps", "font_baseline", "horizontal_scale", "vertical_scale", "ligatures" };
            for (auto kv : extra) {
              std::string k = py::str(kv.first);
              bool known = false;
              for (const char *x : kExtra) if (k == x) known = true;
              if (!known) throw std::invalid_argument("set_run_style: unknown argument '" + k + "'");
            }
            bool any = fillRunStyleEdit(e, font, size_px, color, tracking, kerning,
                                        bold, italic, underline);
            any |= fillRunStyleExtras(e, [&](const char *k) {
              return extra.contains(k) ? py::handle(extra[k]) : py::handle(); });
            if (!any)
                throw std::invalid_argument("set_run_style: pass at least one style value");
            setLayerRunStyle(self, index, run_index, e);
         },
         py::arg("index"), py::arg("run_index"),
         py::arg("size_px") = py::none(), py::arg("color") = py::none(),
         py::arg("tracking") = py::none(), py::arg("kerning") = py::none(),
         py::arg("bold") = py::none(), py::arg("italic") = py::none(),
         py::arg("underline") = py::none(),
         // font は 0.9.0 で後から足したので、既存の位置引数の並びを崩さない
         // ように末尾に置く。leading 以降 (0.13) はキーワード専用。
         py::arg("font") = py::none(),
         "Edit style values of an existing style run (see text['runs']). Any of: "
         "font (str; appended to the document's font set if new), size_px "
         "(float), color ((r,g,b[,a]) 0..1), tracking (int), kerning (int), "
         "bold/italic/underline (bool); keyword-only: leading (px, or 'auto'), "
         "baseline_shift (px), strikethrough (bool), font_caps (0/1 small/2 all), "
         "font_baseline (0/1 super/2 sub), horizontal_scale / vertical_scale (1.0 = "
         "100%), ligatures (bool). Text and run lengths are unchanged; "
         "keys are added to the run if inherited. Raises for non-text layers or "
         "an out-of-range run index.")
    .def("set_rich_text",
         [](psd::PSDFile &self, int index, const std::string &text,
            py::object runs, py::object paragraphs, bool formatting_unchanged) {
            std::vector<psd::TextRunSpec> r = toRunSpecs(runs);
            std::vector<psd::TextParagraphSpec> p = toParagraphSpecs(paragraphs);
            std::string err;
            raiseIfFailed(self.setLayerRichText(index, psd::utf8ToU16(text), r, p, &err,
                                                formatting_unchanged), err);
         },
         py::arg("index"), py::arg("text"), py::arg("runs") = py::none(),
         py::arg("paragraphs") = py::none(),
         py::arg("formatting_unchanged") = false,
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
    .def("set_paragraph_style",
         [](psd::PSDFile &self, int index, int para_index, py::kwargs style) {
            static const char *kKeys[] = { "justification", "first_line_indent",
              "start_indent", "end_indent", "space_before", "space_after",
              "auto_leading", "hyphenate" };
            for (auto kv : style) {
              std::string k = py::str(kv.first);
              bool known = false;
              for (const char *x : kKeys) if (k == x) known = true;
              if (!known) throw std::invalid_argument("set_paragraph_style: unknown argument '" + k + "'");
            }
            psd::TextParagraphSpec spec;
            if (!fillParagraphStyle(spec, [&](const char *k) {
                  return style.contains(k) ? py::handle(style[k]) : py::handle(); }))
              throw std::invalid_argument("set_paragraph_style: pass at least one style value");
            std::string err;
            raiseIfFailed(self.setLayerParagraphStyle(index, para_index, spec, &err), err);
         },
         py::arg("index"), py::arg("para_index") = -1,
         "Edit paragraph formatting on a text layer. Keyword arguments: "
         "justification (0 left / 1 right / 2 center), first_line_indent, "
         "start_indent, end_indent, space_before, space_after (px, same units as "
         "size_px), auto_leading (multiplier), hyphenate (bool). Only the given "
         "values change. para_index=-1 (default) applies to every paragraph.")
    .def("set_text_engine_policy",
         [](psd::PSDFile &self, int policy) {
            self.setTextEngineDataPolicy((psd::PSDFile::TextEngineDataPolicy)policy);
         },
         py::arg("policy"),
         "How to treat Txt2, the document-wide Text Engine Data that Photoshop "
         "reads in preference to each layer's TySh. 0=SYNC (default: mirror text "
         "and run lengths, drop Txt2 when a change cannot be mirrored), "
         "1=REMOVE (always drop it so Photoshop falls back to TySh), "
         "2=KEEP (leave it alone; edits stay invisible to Photoshop).")
    .def_property_readonly("text_engine_policy",
         [](const psd::PSDFile &self) { return (int)self.textEngineDataPolicy(); })
    .def_property_readonly("text_engine_dropped",
         [](const psd::PSDFile &self) { return self.textEngineDataDropped(); },
         "True once a change that could not be mirrored made psdparse drop Txt2.")
    .def("has_text_engine_data",
         [](psd::PSDFile &self) { return self.hasDocumentAdditionalInfo('Txt2'); },
         "Whether the document still carries a Txt2 block.")
    .def("drop_text_engine_data",
         [](psd::PSDFile &self) { return self.dropTextEngineData(); },
         "Drop Txt2 so Photoshop falls back to the per-layer TySh.")
    .def("layer_text_index",
         [](const psd::PSDFile &self, int index) -> py::object {
            int v = -1;
            if (!self.getLayerTextIndex(index, v)) return py::none();
            return py::cast(v);
         },
         py::arg("index"),
         "TextIndex from the layer's TySh: its position within Txt2.")
    .def("document_additional_info",
         [](psd::PSDFile &self, const std::string &key) -> py::object {
            if (key.size() != 4) return py::none();
            int k = ((int)(unsigned char)key[0] << 24) | ((int)(unsigned char)key[1] << 16) |
                    ((int)(unsigned char)key[2] <<  8) |  (int)(unsigned char)key[3];
            std::string out;
            if (!self.getDocumentAdditionalInfo(k, out)) return py::none();
            return py::bytes(out);
         },
         py::arg("key"),
         "Raw bytes of a document-scope additional info block (e.g. 'Txt2'), "
         "or None when absent.")
    .def("text_engine_texts",
         [](psd::PSDFile &self) -> py::object {
            std::string blob;
            if (!self.getDocumentAdditionalInfo('Txt2', blob)) return py::none();
            std::vector<psd::u16str> texts;
            if (!psd::listTextEngineDataTexts(blob.data(), blob.size(), texts))
                return py::none();
            py::list out;
            for (const psd::u16str &t : texts) out.append(u16ToStr(t));
            return out;
         },
         "Texts carried by Txt2, in TextIndex order (None when absent).")
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
    .def("set_merged_image_solid",
         [](psd::PSDFile &self, int r, int g, int b) {
            return self.setMergedImageSolid((uint8_t)r, (uint8_t)g, (uint8_t)b);
         },
         py::arg("r") = 255, py::arg("g") = 255, py::arg("b") = 255,
         "Replace the stored composite with a solid colour, RLE-compressed so "
         "it stays small on any canvas. This is the shape Photoshop writes when "
         "'Maximize PSD compatibility' is off: a preview with nothing in it. "
         "Use it after edits that leave the composite stale, so the file does "
         "not hand out a picture that is no longer true. Photoshop composites "
         "from the layers, so what it displays is unaffected.")
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
    .def_property_readonly("roots",
         [](const psd::PSDFile &self) { return self.childIndices(-1); },
         "Indices of the top-level layers, bottom-to-top. Together with "
         "LayerInfo.children this gives a tree view over the flat `layers` "
         "list; the flat list stays canonical for drawing order and editing.")
    .def("children",
         [](const psd::PSDFile &self, int index) { return self.childIndices(index); },
         py::arg("index"),
         "Indices of the direct children of layers[index], bottom-to-top. "
         "Pass -1 for the top level (same as `roots`).")
    .def_readonly("is_loaded", &psd::PSDFile::isLoaded)
    .def_readonly("header",    &psd::PSDFile::header)
    .def_readonly("layers",    &psd::PSDFile::layerList)
    .def_property_readonly("layer_source", [](const psd::PSDFile &self) -> py::object {
           if (self.layerSourceKey == 0) return py::none();
           char k[5] = { (char)((self.layerSourceKey >> 24) & 0xff),
                         (char)((self.layerSourceKey >> 16) & 0xff),
                         (char)((self.layerSourceKey >> 8) & 0xff),
                         (char)(self.layerSourceKey & 0xff), 0 };
           return py::str(k);
         },
         "Where the layers were read from: None for the regular layer info, or "
         "'Lr16' / 'Lr32' for 16/32-bit documents, where Photoshop stores the "
         "layers in a document-level block and leaves the layer info empty. "
         "save() rewrites that block from the current layer list.")
    .def_readonly("merged_alpha", &psd::PSDFile::mergedAlpha)
    .def("merged_image", &mergedImage)
    .def("layer_image", &layerImage,
         py::arg("index"), py::arg("mode") = "masked",
         "Extract pixels for layer `index` as BGRA bytes. "
         "mode: 'masked' (default), 'image' (no mask), 'mask' (mask only).")
    .def_property_readonly("merged_has_transparency", &psd::PSDFile::mergedHasTransparency,
         "True when the stored merged image has a transparency channel (the extra "
         "channel after the color channels is the merged transparency, not an "
         "alpha / spot channel). merged_image() is opaque otherwise.")
    .def("composite", &psdComposite, py::arg("effects") = true, py::arg("background") = py::none(),
         "Composite the document from its layers (not the stored merged image): "
         "blend modes, groups (pass-through and isolated), clipping, masks, "
         "opacity / fill opacity, and layer effects when effects=True. Returns "
         "(bgra_bytes, stats); stats counts what could not be reproduced "
         "(adjustment layers are skipped for now). background=(r, g, b) "
         "composites onto an opaque color instead of transparency.")
    .def("shape_mask", &psdShapeMask, py::arg("index"), py::arg("part") = "both",
         "Rasterize a layer's vector mask / shape path to 8-bit coverage "
         "(anti-aliased, 0..255): part='fill' (the path area), 'stroke' (the "
         "shape stroke from 'vstk': width, alignment, caps, joins, dashes) or "
         "'both'. Returns (bytes, left, top, width, height) — a rectangle in "
         "document pixels that holds the path and its stroke (it can extend past "
         "the canvas) — or None without a vector mask (or without a stroke for "
         "'stroke'). Mask density / feather are not applied.")
    .def("render_layer", &psdRenderLayer, py::arg("index"), py::arg("effects") = true,
         "Render one layer with its effects onto a transparent surface (not "
         "composited with the layers below): returns (bgra_bytes, left, top, "
         "width, height), where the rectangle includes what the effects add "
         "around the layer (shadow, glow, stroke). Masks, fill / shape content "
         "and opacity are applied. None for groups, dividers, adjustment "
         "layers or empty layers.")
    .def_property_readonly("annotations", &psdAnnotations,
         "Notes ('Anno') as dicts {'kind' ('text' / 'sound'), 'open', 'icon_rect', "
         "'popup_rect' (top, left, bottom, right), 'color_space', 'color', 'author', "
         "'name', 'mod_date' (raw Pascal bytes), 'text' (str, CR line breaks), "
         "'data_size'}.")
    .def_property_readonly("alpha_channels", &psdAlphaChannels,
         "The merged image's extra channels after the color channels (alpha "
         "channels, spot colors, the merged transparency) as dicts {'plane', "
         "'name', 'kind' ('selected' / 'masked' / 'spot'), 'color_space', 'color', "
         "'opacity'} from image resources 1045 / 1006 / 1077. Pixels via "
         "merged_channel(plane). For Multichannel documents every channel is listed.")
    .def("merged_channel", &psdMergedChannel, py::arg("plane"),
         "One channel of the merged image as 8-bit grayscale bytes "
         "(width*height). plane counts from 0 across all header.channels.")
    .def_property_readonly("patterns", &psdPatterns,
         "Patterns stored in the document ('Patt' / 'Pat2' / 'Pat3') as dicts "
         "{'id', 'name', 'mode', 'width', 'height', 'block'}. Pattern fills refer "
         "to them by 'id'. Pixels via pattern_image().")
    .def("pattern_image", &psdPatternImage, py::arg("which"),
         "Pixels of a pattern (an index into patterns, or its id) as "
         "(bgra_bytes, width, height): 8-bit display values, opaque when the "
         "pattern has no transparency. None if it cannot be decoded.")
    .def_property_readonly("linked_files", &psdLinkedFiles,
         "Smart-object source files from the document's lnk2 / lnk3 / lnkD / lnkE "
         "blocks, as dicts {'kind' ('data' embedded / 'external' / 'alias'), "
         "'uuid', 'name', 'file_type', 'creator', 'size', 'has_data', 'version', "
         "'block'}. The bytes are read on demand with linked_file_data().")
    .def("linked_file_data", &psdLinkedFileData, py::arg("which"),
         "Bytes of a linked file (an index into linked_files, or its uuid): the "
         "embedded file, or the copy Photoshop keeps of an external one. None when "
         "the entry carries no data.")
    .def_property_readonly("paths", &psdPaths,
         "Saved paths (image resources 2000-2997, kind 'saved') and the work "
         "path (1025, kind 'work') in resource order, as dicts {'id', 'kind', "
         "'name' (raw bytes), 'unicode_name' (from the 'pths' block, or None), "
         "'path'}. 'path' has the same shape as "
         "layer.vector_mask['path'].")
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

  
