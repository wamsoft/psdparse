// 文書の合成 (レイヤを重ねて 1 枚にする)。画素の計算は psdfx (C API) に任せ、
// ここではレイヤ木のたどり方 (グループ / クリッピング / マスク / 不透明度) を扱う。
//
//   - レイヤは下から上へ重ねる。非表示のレイヤとグループは飛ばす
//   - グループ: 通過 ('pass') は下の結果へ直接重ね、不透明度・マスクは前後の補間で
//     掛ける。それ以外は透明な面へ中身を重ねてから、グループのブレンドで重ねる
//   - クリッピング: 下地レイヤとそれにクリップされたレイヤを 1 つの面にまとめ、
//     クリップ側は下地の不透明な所にだけ重ねる (source-atop)
//   - レイヤの不透明度と塗りの不透明度は掛け合わせる (効果が無い場合)
//   - 調整レイヤはまだ反映しない (飛ばして数を数える)
#include "psdparse.h"
#include "psdfile.h"
#include "psdfx.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace psd {

namespace {

// パス (座標は文書に対する割合) を psdfx の形 (文書ピクセル) へ
struct PathBuf {
  std::vector<std::vector<psdfx_knot>> knots;
  std::vector<psdfx_subpath> subs;
  int initialFill = 0;
};

void toPsdfx(const PathData &p, double W, double H, PathBuf &b) {
  b.knots.clear(); b.subs.clear();
  for (const auto &sp : p.subpaths) {
    std::vector<psdfx_knot> ks;
    for (const auto &k : sp.knots)
      ks.push_back({ k.preceding.x * W, k.preceding.y * H, k.anchor.x * W, k.anchor.y * H,
                     k.leaving.x * W, k.leaving.y * H });
    b.knots.push_back(std::move(ks));
  }
  for (size_t i = 0; i < b.knots.size(); i++) {
    const auto &sp = p.subpaths[i];
    b.subs.push_back({ b.knots[i].data(), (int)b.knots[i].size(), sp.closed ? 1 : 0, sp.operation });
  }
  b.initialFill = p.initialFill == 1 ? 1 : 0;
}

// シェイプの線 ('vstk') を psdfx の線の描き方へ。破線の長さは線幅を 1 とした値
// なので線幅を掛ける。pt 指定の線幅は文書の解像度で px にする。
bool toStrokeStyle(const ShapeStroke &s, double dpi, psdfx_stroke_style &st,
                   std::vector<double> &dashes) {
  st = psdfx_stroke_style();
  const double w = s.width * (s.widthInPoints ? (dpi > 0 ? dpi : 72.0) / 72.0 : 1.0);
  if (!(w > 0)) return false;
  st.width = w;
  st.alignment = s.alignment == 0 ? PSDFX_STROKE_OUTSIDE : s.alignment == 1 ? PSDFX_STROKE_INSIDE
                                                                           : PSDFX_STROKE_CENTER;
  st.cap = s.cap;
  st.join = s.join;
  st.miter_limit = s.miterLimit;
  dashes.clear();
  for (double d : s.dashes) dashes.push_back(d * w);
  st.dashes = dashes.empty() ? nullptr : dashes.data();
  st.dash_count = (int)dashes.size();
  st.dash_offset = s.dashOffset * w;
  return true;
}

struct Canvas {
  int width = 0, height = 0;
  std::vector<uint8_t> px;
  // 効果の形 (塗りつぶしレイヤ / シェイプのマスク)。空なら画素のアルファを使う
  std::vector<uint8_t> shape;
  Canvas() = default;
  Canvas(int w, int h) : width(w), height(h), px((size_t)w * h * 4, 0) {}
  psdfx_surface surface() { return psdfx_surface{ px.data(), width, height, width * 4 }; }
};

class Compositor {
public:
  Compositor(PSDFile &psd, const CompositeOptions &opt, CompositeStats &st)
    : psd_(psd), opt_(opt), st_(st) {}

  // レイヤ 1 枚を (効果込みで) 透明な面へ描く。面は効果のはみ出しを含む矩形。
  bool renderSingle(int idx, std::vector<uint8_t> &out, int &left, int &top, int &w, int &h) {
    LayerInfo &l = psd_.layerList[(size_t)idx];
    if (l.layerType == LAYER_TYPE_FOLDER || l.layerType == LAYER_TYPE_HIDDEN ||
        l.layerType == LAYER_TYPE_ADJUST) return false;
    Canvas surf;
    int sx = 0, sy = 0;
    if (!layerSurface(l, surf, sx, sy)) return false;
    psdfx_layer_effects fx;
    FxStore store;
    int margin = 0;
    if (opt_.effects && layerEffects(l, fx, store)) margin = psdfx_effects_margin(&fx);
    left = sx - margin; top = sy - margin;
    w = surf.width + 2 * margin; h = surf.height + 2 * margin;
    if ((int64_t)w * h > opt_.maxPixels) return false;
    Canvas dst(w, h);
    drawLayer(l, surf, sx, sy, dst, left, top, l.opacity / 255.f, l.fill_opacity / 255.f,
              PSDFX_KEY('n','o','r','m'), nullptr);
    out.swap(dst.px);
    return true;
  }

  // グループの子を下から描く。passThrough なら canvas は下の画像を含んだ面
  // (通過グループ)。ノックアウトの行き先のため、グループの開始時点の面を積む。
  void renderChildren(int parent, Canvas &canvas, bool passThrough = false) {
    std::vector<int> kids = psd_.childIndices(parent);
    std::unique_ptr<Canvas> start;
    if (parent >= 0) {
      if (passThrough) start.reset(new Canvas(canvas));
      scopes_.push_back(start.get());
    }
    struct Pop { std::vector<const Canvas*> &v; bool on; ~Pop() { if (on) v.pop_back(); } } pop{ scopes_, parent >= 0 };
    for (size_t k = 0; k < kids.size(); ) {
      const int idx = kids[k];
      // クリッピング: 下地 (clipping == 0) のすぐ上に続く clipping != 0 の兄弟
      size_t end = k + 1;
      while (end < kids.size() && psd_.layerList[(size_t)kids[end]].clipping != 0) end++;
      std::vector<int> clipped(kids.begin() + (long)k + 1, kids.begin() + (long)end);
      renderWithClipped(idx, clipped, canvas);
      k = end;
    }
  }

private:
  PSDFile &psd_;
  const CompositeOptions &opt_;
  CompositeStats &st_;
  // ノックアウトの行き先: 描いている途中のグループの開始時点の面 (外側から順)。
  // 独立したグループは nullptr (透明から始まる)
  std::vector<const Canvas*> scopes_;
  std::unique_ptr<Canvas> background_;   // 背景レイヤだけを描いた面 (深いノックアウト)

  bool visible(const LayerInfo &l) const { return l.isVisible(); }

  // レイヤマスク (グレー) を文書全体の大きさで返す。無効 / 無しなら空。
  std::vector<uint8_t> documentMask(LayerInfo &l) {
    const LayerMask &m = l.extraData.layerMask;
    std::vector<uint8_t> out;
    if (!m.present || (m.flags & 2) || m.width <= 0 || m.height <= 0) return out;
    std::vector<uint8_t> bgra((size_t)m.width * m.height * 4);
    if (!psd_.getLayerImage(l, bgra.data(), BGRA_LE, m.width * 4, IMAGE_MODE_MASK)) return out;
    const int W = psd_.header.width, H = psd_.header.height;
    out.assign((size_t)W * H, (uint8_t)m.defaultColor);
    for (int y = 0; y < m.height; y++) {
      int dy = m.top + y;
      if (dy < 0 || dy >= H) continue;
      for (int x = 0; x < m.width; x++) {
        int dx = m.left + x;
        if (dx < 0 || dx >= W) continue;
        out[(size_t)dy * W + dx] = bgra[((size_t)y * m.width + x) * 4];
      }
    }
    return out;
  }

  // レイヤの追加情報 key を descriptor として読む (先頭 skip バイトを飛ばす)
  static bool readDescriptor(const LayerInfo &l, int key, int skip, Descriptor &d) {
    for (const auto &a : l.extraData.additionalLayers) {
      if (a.key != key || !a.data) continue;
      IteratorBase *r = a.data->clone();
      r->init();
      bool ok = r->rest() > skip;
      if (ok) { r->advance(skip); d.load(r); }
      delete r;
      return ok;
    }
    return false;
  }

  static double num(Descriptor *d, const char *k, double def = 0.0) {
    DescriptorItem *it = d ? d->item(k).find() : nullptr;
    if (auto *v = dynamic_cast<DescriptorDouble*>(it)) return v->val;
    if (auto *v = dynamic_cast<DescriptorUnitFloat*>(it)) return v->val;
    if (auto *v = dynamic_cast<DescriptorInteger*>(it)) return v->val;
    return def;
  }

  // descriptor の色 ('Clr ' など) を RGB (0..255) へ。RGB / HSB / グレー / CMYK。
  static bool descColor(Descriptor *c, uint8_t rgb[3]) {
    if (!c) return false;
    double r, g, b;
    if (c->classId == "HSBC") {
      const double h = num(c, "H   ") / 360.0, s = num(c, "Strt") / 100.0, v = num(c, "Brgh") / 100.0;
      const double hh = (h - std::floor(h)) * 6.0;
      const int i = (int)hh;
      const double f = hh - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
      switch (i % 6) {
      case 0: r = v; g = t; b = p; break;
      case 1: r = q; g = v; b = p; break;
      case 2: r = p; g = v; b = t; break;
      case 3: r = p; g = q; b = v; break;
      case 4: r = t; g = p; b = v; break;
      default: r = v; g = p; b = q; break;
      }
      r *= 255; g *= 255; b *= 255;
    } else if (c->classId == "Grsc") {
      r = g = b = (1.0 - num(c, "Gry ") / 100.0) * 255.0;
    } else if (c->classId == "CMYC") {
      const double C = num(c, "Cyn ") / 100, M = num(c, "Mgnt") / 100, Y = num(c, "Ylw ") / 100,
                   K = num(c, "Blck") / 100;
      r = (1 - C) * (1 - K) * 255; g = (1 - M) * (1 - K) * 255; b = (1 - Y) * (1 - K) * 255;
    } else {
      r = num(c, "Rd  "); g = num(c, "Grn "); b = num(c, "Bl  ");
      if (c->item("redFloat").find()) {   // 新しいファイルの浮動小数の色
        r = num(c, "redFloat") * 255; g = num(c, "greenFloat") * 255; b = num(c, "blueFloat") * 255;
      }
    }
    const double v[3] = { r, g, b };
    for (int i = 0; i < 3; i++) rgb[i] = (uint8_t)std::min(255.0, std::max(0.0, v[i] + 0.5));
    return true;
  }

  // descriptor のグラデーション ('Grad') を psdfx_gradient へ (分岐点の配列は呼び出し側が持つ)
  static bool descGradient(Descriptor *gr, std::vector<psdfx_color_stop> &cs,
                           std::vector<psdfx_alpha_stop> &as, psdfx_gradient &g) {
    if (!gr) return false;
    cs.clear(); as.clear();
    if (auto *list = dynamic_cast<DescriptorList*>(gr->item("Clrs").find())) {
      for (auto *it : list->items) {
        auto *s = dynamic_cast<Descriptor*>(it);
        if (!s) continue;
        psdfx_color_stop c;
        c.location = num(s, "Lctn") / 4096.0;
        c.midpoint = num(s, "Mdpn", 50) / 100.0;
        uint8_t rgb[3] = {0, 0, 0};
        auto *type = dynamic_cast<DescriptorEnumerated*>(s->item("Type").find());
        if (type && type->enumId == "BckC") { rgb[0] = rgb[1] = rgb[2] = 255; }   // 背景色 (既定の白)
        else descColor(dynamic_cast<Descriptor*>(s->item("Clr ").find()), rgb);
        c.r = rgb[0]; c.g = rgb[1]; c.b = rgb[2];
        cs.push_back(c);
      }
    }
    if (auto *list = dynamic_cast<DescriptorList*>(gr->item("Trns").find())) {
      for (auto *it : list->items) {
        auto *s = dynamic_cast<Descriptor*>(it);
        if (!s) continue;
        as.push_back({ num(s, "Lctn") / 4096.0, num(s, "Mdpn", 50) / 100.0, num(s, "Opct", 100) / 100.0 });
      }
    }
    if (cs.empty()) return false;
    auto byLoc = [](const auto &a, const auto &b) { return a.location < b.location; };
    std::stable_sort(cs.begin(), cs.end(), byLoc);
    std::stable_sort(as.begin(), as.end(), byLoc);
    g.colors = cs.data(); g.color_count = (int)cs.size();
    g.alphas = as.empty() ? nullptr : as.data(); g.alpha_count = (int)as.size();
    g.smoothness = num(gr, "Intr", 4096) / 4096.0;
    g.interpolation = PSDFX_GRADIENT_CLASSIC;
    return true;
  }

  // 補間方法 ('gradientsInterpolationMethod' は塗り / 効果の descriptor 側にある)
  static int gradientInterpolation(Descriptor *d) {
    return enumOf(d, "gradientsInterpolationMethod") == "Lnr " ? PSDFX_GRADIENT_LINEAR_LIGHT
                                                               : PSDFX_GRADIENT_CLASSIC;
  }

  static int gradientStyle(Descriptor *d) {
    auto *t = d ? dynamic_cast<DescriptorEnumerated*>(d->item("Type").find()) : nullptr;
    if (!t) return PSDFX_GRADIENT_LINEAR;
    if (t->enumId == "Rdl ") return PSDFX_GRADIENT_RADIAL;
    if (t->enumId == "Angl") return PSDFX_GRADIENT_ANGLE;
    if (t->enumId == "Rflc") return PSDFX_GRADIENT_REFLECTED;
    if (t->enumId == "Dmnd") return PSDFX_GRADIENT_DIAMOND;
    return PSDFX_GRADIENT_LINEAR;
  }

  // シェイプ (ベクタマスク) の範囲。無ければレイヤ矩形、それも空なら文書。
  void shapeBox(const LayerInfo &l, double box[4]) {
    const double W = psd_.header.width, H = psd_.header.height;
    bool any = false;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (l.vectorMask.present) {
      for (const auto &sp : l.vectorMask.path.subpaths)
        for (const auto &k : sp.knots) {
          const double x = k.anchor.x * W, y = k.anchor.y * H;
          if (!any) { x0 = x1 = x; y0 = y1 = y; any = true; }
          x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
        }
    }
    if (!any && l.width > 0 && l.height > 0) { x0 = l.left; y0 = l.top; x1 = l.right; y1 = l.bottom; any = true; }
    if (!any) { x0 = 0; y0 = 0; x1 = W; y1 = H; }
    box[0] = x0; box[1] = y0; box[2] = x1; box[3] = y1;
  }

  // 塗りの内容 (単色 / グラデーション / パターン) で out を塗る。塗りが無ければ false。
  //   塗りつぶしレイヤ: 'SoCo' / 'GdFl' / 'PtFl' (版 4 バイト + descriptor)
  //   シェイプの塗り:   'vscg' (種類 4 バイト + 版 4 バイト + descriptor)
  bool paintFill(const LayerInfo &l, Canvas &out, int left, int top) {
    int kind = 0;
    Descriptor d;
    for (int k : { 'SoCo', 'GdFl', 'PtFl' })
      if (readDescriptor(l, k, 4, d)) { kind = k; break; }
    if (!kind) {
      for (const auto &a : l.extraData.additionalLayers) {
        if (a.key != 'vscg' || !a.data) continue;
        IteratorBase *r = a.data->clone();
        r->init();
        if (r->rest() > 8) { kind = r->getInt32(); r->advance(4); d.load(r); }
        delete r;
        break;
      }
    }
    if (!kind) return false;
    return paintContent(l, kind, d, out, left, top);
  }

  // 塗りの descriptor (kind は 'SoCo' / 'GdFl' / 'PtFl') で out を塗る
  bool paintContent(const LayerInfo &l, int kind, Descriptor &d, Canvas &out, int left, int top) {
    psdfx_surface s = out.surface();
    if (kind == 'SoCo') {
      uint8_t rgb[3];
      if (!descColor(dynamic_cast<Descriptor*>(d.item("Clr ").find()), rgb)) return false;
      for (size_t i = 0; i < out.px.size(); i += 4) {
        out.px[i] = rgb[2]; out.px[i + 1] = rgb[1]; out.px[i + 2] = rgb[0]; out.px[i + 3] = 255;
      }
      return true;
    }
    if (kind == 'GdFl') {
      std::vector<psdfx_color_stop> cs; std::vector<psdfx_alpha_stop> as;
      psdfx_gradient g;
      if (!descGradient(dynamic_cast<Descriptor*>(d.item("Grad").find()), cs, as, g)) return false;
      g.interpolation = gradientInterpolation(&d);
      double box[4];
      auto *al = dynamic_cast<DescriptorBoolean*>(d.item("Algn").find());
      if (!al || al->val) shapeBox(l, box);
      else { box[0] = 0; box[1] = 0; box[2] = psd_.header.width; box[3] = psd_.header.height; }
      auto *ofs = dynamic_cast<Descriptor*>(d.item("Ofst").find());
      auto *rv = dynamic_cast<DescriptorBoolean*>(d.item("Rvrs").find());
      psdfx_draw_gradient(&s, left, top, &g, gradientStyle(&d), num(&d, "Angl", 90),
                          num(&d, "Scl ", 100) / 100.0, rv && rv->val, box,
                          num(ofs, "Hrzn"), num(ofs, "Vrtc"));
      return true;
    }
    if (kind == 'PtFl') {
      auto *pt = dynamic_cast<Descriptor*>(d.item("Ptrn").find());
      auto *id = pt ? dynamic_cast<DescriptorString*>(pt->item("Idnt").find()) : nullptr;
      if (!id) return false;
      std::string want;
      for (char16_t ch : id->val) if (ch) want.push_back((char)ch);
      for (size_t i = 0; i < psd_.patterns.size(); i++) {
        if (psd_.patterns[i].id != want) continue;
        std::vector<uint8_t> tile; int tw = 0, th = 0;
        if (!psd_.getPatternImage((int)i, tile, tw, th)) return false;
        psdfx_surface ts{ tile.data(), tw, th, tw * 4 };
        // 原点はシェイプの左上 + 位相。ベクタマスクの形が基準のときは横だけ 1 画素右に
        // ずれる (Photoshop の合成画像と照合して決めた。5 種類のタイルで同じ)
        auto *ph = dynamic_cast<Descriptor*>(d.item("phase").find());
        double box[4];
        shapeBox(l, box);
        psdfx_draw_pattern(&s, left, top, &ts, num(&d, "Scl ", 100) / 100.0,
                           std::floor(box[0]) + (l.vectorMask.present ? 1 : 0) + num(ph, "Hrzn"), std::floor(box[1]) + num(ph, "Vrtc"));
        return true;
      }
      return false;
    }
    return false;
  }

  // ベクタマスクを、面 (左上が文書の (left, top)) の大きさでアルファへ掛ける。
  // シェイプの線 ('vstk') があれば、マスクの形は「塗り ∪ 線」(保存画素には
  // パスの外へはみ出す線も描かれている)。
  void applyVectorMask(const LayerInfo &l, Canvas &c, int left, int top) {
    const VectorMask &vm = l.vectorMask;
    if (!vm.present || vm.disabled()) return;
    PathBuf pb;
    toPsdfx(vm.path, psd_.header.width, psd_.header.height, pb);
    std::vector<uint8_t> m((size_t)c.width * c.height);
    psdfx_fill_path(pb.subs.data(), (int)pb.subs.size(), pb.initialFill,
                    m.data(), c.width, c.height, c.width, left, top);
    ShapeInfo si;
    psdfx_stroke_style st;
    std::vector<double> dashes;
    if (strokeStyle(l, si, st, dashes)) {
      std::vector<uint8_t> sm((size_t)c.width * c.height);
      psdfx_stroke_path(pb.subs.data(), (int)pb.subs.size(), pb.initialFill, &st,
                        sm.data(), c.width, c.height, c.width, left, top);
      for (size_t i = 0; i < m.size(); i++) m[i] = std::max(m[i], sm[i]);
    }
    if (vm.inverted()) for (auto &v : m) v = (uint8_t)(255 - v);
    const LayerMask &lm = l.extraData.layerMask;
    if (lm.hasVectorFeather && lm.vectorMaskFeather > 0)
      psdfx_blur_plane(m.data(), c.width, c.height, c.width, lm.vectorMaskFeather);   // σ = ぼかしの値 (Photoshop で測定)
    if (lm.vectorMaskDensity >= 0 && lm.vectorMaskDensity < 255) applyDensity(m, lm.vectorMaskDensity);
    for (size_t i = 0; i < m.size(); i++)
      c.px[i * 4 + 3] = (uint8_t)((c.px[i * 4 + 3] * m[i] + 127) / 255);
  }

  // シェイプの線の描き方。線が無い / 無効なら false。
  bool strokeStyle(const LayerInfo &l, ShapeInfo &si, psdfx_stroke_style &st,
                   std::vector<double> &dashes) {
    if (!decodeShape(l, si) || !si.hasStroke || !si.stroke.strokeEnabled) return false;
    return toStrokeStyle(si.stroke, psd_.header.hres, st, dashes);
  }

  // シェイプの塗り (fillEnabled) と線を、保存画素ではなくパスから描く。
  // 線の無いシェイプなら false (呼び出し側が塗りだけを描く)。
  bool paintShapeWithStroke(const LayerInfo &l, Canvas &out, int left, int top) {
    const VectorMask &vm = l.vectorMask;
    if (!vm.present || vm.disabled()) return false;
    ShapeInfo si;
    psdfx_stroke_style st;
    std::vector<double> dashes;
    if (!strokeStyle(l, si, st, dashes)) return false;
    PathBuf pb;
    toPsdfx(vm.path, psd_.header.width, psd_.header.height, pb);
    const size_t n = (size_t)out.width * out.height;
    std::vector<uint8_t> fm(n), sm(n);
    psdfx_fill_path(pb.subs.data(), (int)pb.subs.size(), pb.initialFill,
                    fm.data(), out.width, out.height, out.width, left, top);
    psdfx_stroke_path(pb.subs.data(), (int)pb.subs.size(), pb.initialFill, &st,
                      sm.data(), out.width, out.height, out.width, left, top);
    Canvas fill(out.width, out.height);
    if (si.stroke.fillEnabled && paintFill(l, fill, left, top)) {
      if (vm.inverted()) for (auto &v : fm) v = (uint8_t)(255 - v);
      for (size_t i = 0; i < n; i++) fill.px[i * 4 + 3] = (uint8_t)((fill.px[i * 4 + 3] * fm[i] + 127) / 255);
    } else {
      std::fill(fill.px.begin(), fill.px.end(), 0);
    }
    Canvas line(out.width, out.height);
    if (si.stroke.content && si.stroke.contentKind &&
        paintContent(l, si.stroke.contentKind, *si.stroke.content, line, left, top)) {
      for (size_t i = 0; i < n; i++) line.px[i * 4 + 3] = (uint8_t)((line.px[i * 4 + 3] * sm[i] + 127) / 255);
      psdfx_surface d = fill.surface(), s = line.surface();
      psdfx_composite(&d, &s, 0, 0, blendFromName(si.stroke.blendMode),
                      (float)si.stroke.opacity, nullptr, 0);
    }
    out.px.swap(fill.px);
    out.shape.resize(n);
    for (size_t i = 0; i < n; i++) out.shape[i] = std::max(si.stroke.fillEnabled ? fm[i] : (uint8_t)0, sm[i]);
    return true;
  }

  // --- 調整レイヤ ---------------------------------------------------------------

  // 調整レイヤを target (文書大の面) へ掛ける。target の色を調整し、レイヤの
  // ブレンドモード・不透明度 x 塗りの不透明度・マスク (と clipMask) で元の色へ
  // 重ねる。アルファは変えない。再現できない調整は数えて飛ばす。
  void applyAdjustment(LayerInfo &l, Canvas &target, const uint8_t *clipMask) {
    AdjustmentInfo a;
    if (!decodeAdjustment(l, a) || !a.valid) { st_.skippedAdjustments++; return; }
    Canvas adj = target;
    psdfx_surface s = adj.surface();
    if (!adjustSurface(a, s)) { st_.skippedAdjustments++; return; }
    const int W = target.width;
    std::vector<uint8_t> mask = adjustmentMask(l, clipMask, target.width, target.height);
    psdfx_surface d = target.surface();
    psdfx_apply_adjusted(&d, &s, (uint32_t)l.blendModeKey, l.opacity / 255.f * (l.fill_opacity / 255.f),
                         mask.data(), W);
  }

  // 調整レイヤのマスク (ユーザーマスク / ベクタマスク、clipMask があれば掛ける) を文書大で
  std::vector<uint8_t> adjustmentMask(LayerInfo &l, const uint8_t *clipMask, int W, int H) {
    Canvas m(W, H);
    for (size_t i = 3; i < m.px.size(); i += 4) m.px[i] = 255;
    LayerInfo maskLayer;
    if (userMaskLayer(l, maskLayer)) applyUserMask(maskLayer, m, 0, 0);
    applyVectorMask(l, m, 0, 0);
    std::vector<uint8_t> mask((size_t)W * H);
    for (size_t i = 0; i < mask.size(); i++) {
      int v = m.px[i * 4 + 3];
      if (clipMask) v = (v * clipMask[i] + 127) / 255;
      mask[i] = (uint8_t)v;
    }
    return mask;
  }

  static double scalarOf(const AdjustmentInfo &a, const char *k, double def = 0.0) {
    for (const auto &v : a.scalars) if (v.first == k) return v.second;
    return def;
  }
  static const std::vector<double> *arrayOf(const AdjustmentInfo &a, const char *k) {
    for (const auto &v : a.arrays) if (v.first == k) return &v.second;
    return nullptr;
  }
  static const std::vector<std::vector<double>> *tableOf(const AdjustmentInfo &a, const char *k) {
    for (const auto &v : a.tables) if (v.first == k) return &v.second;
    return nullptr;
  }

  // Lab (D50、L 0..100) → sRGB
  static void labToRgb(double L, double A, double B, uint8_t out[3]) {
    auto finv = [](double t) { return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); };
    const double fy = (L + 16) / 116, fx = fy + A / 500, fz = fy - B / 200;
    const double X = 0.9642 * finv(fx), Y = finv(fy), Z = 0.8249 * finv(fz);
    const double lin[3] = { 3.1338561 * X - 1.6168667 * Y - 0.4906146 * Z,
                            -0.9787684 * X + 1.9161415 * Y + 0.0334540 * Z,
                            0.0719453 * X - 0.2289914 * Y + 1.4052427 * Z };
    for (int i = 0; i < 3; i++) {
      double v = std::min(1.0, std::max(0.0, lin[i]));
      v = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
      out[i] = (uint8_t)(v * 255 + 0.5);
    }
  }

  // 調整を面に掛ける。対応していない調整なら false。
  bool adjustSurface(const AdjustmentInfo &a, psdfx_surface &s) {
    uint8_t lut[4][256];
    auto identity = [&](uint8_t *t) { for (int i = 0; i < 256; i++) t[i] = (uint8_t)i; };
    auto applyLuts = [&]() { psdfx_apply_lut(&s, lut[1], lut[2], lut[3]); };
    if (a.type == "invert") {
      for (int i = 0; i < 256; i++) lut[1][i] = lut[2][i] = lut[3][i] = (uint8_t)(255 - i);
      applyLuts();
      return true;
    }
    if (a.type == "posterize") {
      psdfx_posterize_lut((int)scalarOf(a, "levels", 4), lut[1]);
      std::memcpy(lut[2], lut[1], 256); std::memcpy(lut[3], lut[1], 256);
      applyLuts();
      return true;
    }
    if (a.type == "threshold") { psdfx_threshold(&s, (int)scalarOf(a, "level", 128)); return true; }
    if (a.type == "exposure") {
      psdfx_exposure_lut(scalarOf(a, "exposure"), scalarOf(a, "offset"), scalarOf(a, "gamma", 1.0), lut[1]);
      std::memcpy(lut[2], lut[1], 256); std::memcpy(lut[3], lut[1], 256);
      applyLuts();
      return true;
    }
    if (a.type == "brightness_contrast") {
      double b = scalarOf(a, "brightness"), c = scalarOf(a, "contrast");
      bool legacy = true;
      if (a.descriptor) {   // 新しいファイルは 'CgEd' に実際の値と方式を持つ
        Descriptor *d = a.descriptor.get();
        b = num(d, "Brgh", b); c = num(d, "Cntr", c);
        legacy = flag(d, "useLegacy", false);
      }
      psdfx_brightness_contrast_lut(b, c, legacy ? 1 : 0, lut[1]);
      std::memcpy(lut[2], lut[1], 256); std::memcpy(lut[3], lut[1], 256);
      applyLuts();
      return true;
    }
    if (a.type == "levels") {
      // 先にチャンネルごと (records[1..3])、次に全体 (records[0])
      const auto *rec = tableOf(a, "records");
      if (!rec || rec->size() < 4) return false;
      uint8_t master[256];
      const auto &m = (*rec)[0];
      psdfx_levels_lut(m[0], m[1], m[2], m[3], m[4], master);
      for (int ch = 1; ch <= 3; ch++) {
        const auto &r = (*rec)[(size_t)ch];
        uint8_t t[256];
        psdfx_levels_lut(r[0], r[1], r[2], r[3], r[4], t);
        for (int i = 0; i < 256; i++) lut[ch][i] = master[t[i]];
      }
      applyLuts();
      return true;
    }
    if (a.type == "curves") {
      // チャンネルごとの曲線を先に、全体 (チャンネル 0) を後に
      const auto *ch = arrayOf(a, "channels");
      const auto *pts = tableOf(a, "points");
      const auto *maps = tableOf(a, "maps");
      const auto *rows = pts ? pts : maps;
      if (!ch || !rows) return false;
      for (int i = 0; i < 4; i++) identity(lut[i]);
      for (size_t k = 0; k < ch->size() && k < rows->size(); k++) {
        const int c = (int)(*ch)[k];
        if (c < 0 || c > 3) continue;
        const auto &row = (*rows)[k];
        if (maps) { for (int i = 0; i < 256 && i < (int)row.size(); i++) lut[c][i] = (uint8_t)row[(size_t)i]; }
        else psdfx_curve_lut(row.data(), (int)row.size() / 2, lut[c]);
      }
      for (int c = 1; c <= 3; c++)
        for (int i = 0; i < 256; i++) lut[c][i] = lut[0][lut[c][i]];
      applyLuts();
      return true;
    }
    if (a.type == "hue_saturation") {
      const auto *mst = arrayOf(a, "master");
      const auto *col = arrayOf(a, "colorization");
      const auto *rg = tableOf(a, "ranges");
      const bool colorize = scalarOf(a, "colorize") != 0;
      const auto *v = colorize ? col : mst;
      if (!v || v->size() < 3) return false;
      std::vector<psdfx_hue_range> ranges;
      if (rg) for (const auto &r : *rg) {
        if (r.size() < 7) continue;
        ranges.push_back({ { r[0], r[1], r[2], r[3] }, r[4], r[5], r[6] });
      }
      psdfx_hue_saturation(&s, (*v)[0], (*v)[1], (*v)[2], colorize ? 1 : 0, ranges.data(), (int)ranges.size());
      return true;
    }
    if (a.type == "vibrance") {
      Descriptor *d = a.descriptor.get();
      if (!d) return false;
      psdfx_vibrance(&s, num(d, "vibrance"), num(d, "Strt"));
      return true;
    }
    if (a.type == "color_balance") {
      const auto *sh = arrayOf(a, "shadows"), *md = arrayOf(a, "midtones"), *hi = arrayOf(a, "highlights");
      if (!sh || !md || !hi) return false;
      psdfx_color_balance(&s, sh->data(), md->data(), hi->data(), scalarOf(a, "preserve_luminosity") != 0);
      return true;
    }
    if (a.type == "selective_color") {
      const auto *rec = tableOf(a, "records");
      if (!rec || rec->size() < 10) return false;
      double adj[9][4];
      for (int r = 0; r < 9; r++)
        for (int k = 0; k < 4; k++) adj[r][k] = (*rec)[(size_t)r + 1][(size_t)k];
      psdfx_selective_color(&s, adj, scalarOf(a, "method") == 0 ? 1 : 0);
      return true;
    }
    if (a.type == "channel_mixer") {
      const auto *rows = tableOf(a, "channels");
      if (!rows || rows->empty()) return false;
      double mtx[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
      for (size_t r = 0; r < 3 && r < rows->size(); r++) {
        const auto &row = (*rows)[r];
        if (row.size() < 5) continue;
        mtx[r][0] = row[0] / 100; mtx[r][1] = row[1] / 100; mtx[r][2] = row[2] / 100; mtx[r][3] = row[4] / 100;
      }
      psdfx_channel_mixer(&s, mtx, scalarOf(a, "monochrome") != 0);
      return true;
    }
    if (a.type == "photo_filter") {
      uint8_t rgb[3];
      const int version = (int)scalarOf(a, "version");
      if (version == 3) {
        const auto *v = arrayOf(a, "xyz");     // 実際は Lab x 100
        if (!v || v->size() < 3) return false;
        labToRgb((*v)[0] / 100, (*v)[1] / 100, (*v)[2] / 100, rgb);
      } else {
        const auto *c = arrayOf(a, "color");
        if (!c || c->size() < 3) return false;
        const int space = (int)scalarOf(a, "color_space");
        if (space == 0) for (int i = 0; i < 3; i++) rgb[i] = (uint8_t)((*c)[(size_t)i] / 257.0 + 0.5);
        else if (space == 7) labToRgb((*c)[0] / 100, (int16_t)(*c)[1] / 100.0, (int16_t)(*c)[2] / 100.0, rgb);
        else return false;
      }
      psdfx_photo_filter(&s, rgb, scalarOf(a, "density") / 100.0, scalarOf(a, "preserve_luminosity") != 0);
      return true;
    }
    if (a.type == "black_white") {
      Descriptor *d = a.descriptor.get();
      if (!d) return false;
      const double w[6] = { num(d, "Rd  ", 40), num(d, "Yllw", 60), num(d, "Grn ", 40),
                            num(d, "Cyn ", 60), num(d, "Bl  ", 20), num(d, "Mgnt", 80) };
      uint8_t tint[3];
      const bool useTint = flag(d, "useTint", false) &&
                           descColor(dynamic_cast<Descriptor*>(d->item("tintColor").find()), tint);
      psdfx_black_white(&s, w, useTint ? tint : nullptr);
      return true;
    }
    if (a.type == "gradient_map") {
      const auto *cs = tableOf(a, "color_stops");
      if (!cs || cs->size() < 2) return false;
      std::vector<psdfx_color_stop> stops;
      for (const auto &r : *cs) {
        if (r.size() < 7 || r[2] != 0) return false;   // RGB の分岐点だけ
        psdfx_color_stop c;
        c.location = r[0] / 4096.0; c.midpoint = r[1] / 100.0;
        c.r = (uint8_t)(r[3] / 257.0 + 0.5); c.g = (uint8_t)(r[4] / 257.0 + 0.5); c.b = (uint8_t)(r[5] / 257.0 + 0.5);
        stops.push_back(c);
      }
      std::sort(stops.begin(), stops.end(),
                [](const psdfx_color_stop &x, const psdfx_color_stop &y) { return x.location < y.location; });
      psdfx_alpha_stop as[2] = { { 0, 0.5, 1 }, { 1, 0.5, 1 } };
      psdfx_gradient g;
      g.interpolation = PSDFX_GRADIENT_CLASSIC;
      g.colors = stops.data(); g.color_count = (int)stops.size();
      g.alphas = as; g.alpha_count = 2;
      g.smoothness = scalarOf(a, "interpolation", 4096) / 4096.0;
      psdfx_gradient_map(&s, &g, scalarOf(a, "reversed") != 0, scalarOf(a, "dithered") != 0, 0, 0);
      return true;
    }
    return false;
  }

  // マスクの濃度: 黒い (隠す) 部分の効き具合を density/255 に弱める
  static void applyDensity(std::vector<uint8_t> &m, int density) {
    for (auto &v : m) v = (uint8_t)(255 - ((255 - v) * density + 127) / 255);
  }

  static bool hasChannel(const LayerInfo &l, int id) {
    for (const auto &c : l.channels) if (c.id == id && c.imageData) return true;
    return false;
  }

  // 合成に使うユーザーマスクを選ぶ。
  //   -3 があれば、それが本物のユーザーマスク (矩形 / 既定色 / フラグは real 側)。
  //      このとき -2 はベクタマスクを焼いたもの。
  //   -2 だけなら、フラグ bit3 (他のデータから描いた) が無いときだけ本物。
  // 使うマスクを 1 枚だけ持つ LayerInfo の写しを作って返す。無ければ false。
  static bool userMaskLayer(const LayerInfo &l, LayerInfo &out) {
    const LayerMask &m = l.extraData.layerMask;
    if (!m.present) return false;
    if (hasChannel(l, -3) && m.hasReal) {
      if (m.realFlags & 2) return false;
      out = l;
      std::vector<ChannelInfo> keep;
      for (const auto &c : out.channels) if (c.id != -2) keep.push_back(c);
      out.channels.swap(keep);
      LayerMask &om = out.extraData.layerMask;
      om.left = m.enclosingLeft; om.top = m.enclosingTop;
      om.right = m.enclosingRight; om.bottom = m.enclosingBottom;
      om.width = om.right - om.left; om.height = om.bottom - om.top;
      om.defaultColor = m.realUserMaskBackground;
      return true;
    }
    if (hasChannel(l, -2) && !(m.flags & (2 | 8))) {
      out = l;
      return true;
    }
    return false;
  }

  // --- レイヤー効果 (lfx2) → psdfx_layer_effects ------------------------------

  // 効果の descriptor のブレンド (列挙名) をレイヤのブレンドキーへ
  static uint32_t blendFromEnum(Descriptor *d, const char *key) {
    auto *e = d ? dynamic_cast<DescriptorEnumerated*>(d->item(key).find()) : nullptr;
    return blendFromEnumId(e ? e->enumId : std::string("Nrml"));
  }

  static uint32_t blendFromEnumId(const std::string &v) {
    static const struct { const char *name; uint32_t key; } kMap[] = {
      { "Nrml", PSDFX_KEY('n','o','r','m') }, { "Dslv", PSDFX_KEY('d','i','s','s') },
      { "Drkn", PSDFX_KEY('d','a','r','k') }, { "Mltp", PSDFX_KEY('m','u','l',' ') },
      { "CBrn", PSDFX_KEY('i','d','i','v') }, { "linearBurn", PSDFX_KEY('l','b','r','n') },
      { "darkerColor", PSDFX_KEY('d','k','C','l') }, { "Lghn", PSDFX_KEY('l','i','t','e') },
      { "Scrn", PSDFX_KEY('s','c','r','n') }, { "CDdg", PSDFX_KEY('d','i','v',' ') },
      { "linearDodge", PSDFX_KEY('l','d','d','g') }, { "lighterColor", PSDFX_KEY('l','g','C','l') },
      { "Ovrl", PSDFX_KEY('o','v','e','r') }, { "SftL", PSDFX_KEY('s','L','i','t') },
      { "HrdL", PSDFX_KEY('h','L','i','t') }, { "vividLight", PSDFX_KEY('v','L','i','t') },
      { "linearLight", PSDFX_KEY('l','L','i','t') }, { "pinLight", PSDFX_KEY('p','L','i','t') },
      { "hardMix", PSDFX_KEY('h','M','i','x') }, { "Dfrn", PSDFX_KEY('d','i','f','f') },
      { "Xclu", PSDFX_KEY('s','m','u','d') }, { "blendSubtraction", PSDFX_KEY('f','s','u','b') },
      { "blendDivide", PSDFX_KEY('f','d','i','v') }, { "H   ", PSDFX_KEY('h','u','e',' ') },
      { "Strt", PSDFX_KEY('s','a','t',' ') }, { "Clr ", PSDFX_KEY('c','o','l','r') },
      { "Lmns", PSDFX_KEY('l','u','m',' ') },
    };
    for (const auto &m : kMap) if (v == m.name) return m.key;
    return PSDFX_KEY('n','o','r','m');
  }

  // 'BlnM' の列挙名 (線の描画モードは "normal" / "multiply" のような長い名前)
  static uint32_t blendFromName(const std::string &v) {
    static const struct { const char *name; uint32_t key; } kMap[] = {
      { "normal", PSDFX_KEY('n','o','r','m') }, { "dissolve", PSDFX_KEY('d','i','s','s') },
      { "darken", PSDFX_KEY('d','a','r','k') }, { "multiply", PSDFX_KEY('m','u','l',' ') },
      { "colorBurn", PSDFX_KEY('i','d','i','v') }, { "linearBurn", PSDFX_KEY('l','b','r','n') },
      { "darkerColor", PSDFX_KEY('d','k','C','l') }, { "lighten", PSDFX_KEY('l','i','t','e') },
      { "screen", PSDFX_KEY('s','c','r','n') }, { "colorDodge", PSDFX_KEY('d','i','v',' ') },
      { "linearDodge", PSDFX_KEY('l','d','d','g') }, { "lighterColor", PSDFX_KEY('l','g','C','l') },
      { "overlay", PSDFX_KEY('o','v','e','r') }, { "softLight", PSDFX_KEY('s','L','i','t') },
      { "hardLight", PSDFX_KEY('h','L','i','t') }, { "vividLight", PSDFX_KEY('v','L','i','t') },
      { "linearLight", PSDFX_KEY('l','L','i','t') }, { "pinLight", PSDFX_KEY('p','L','i','t') },
      { "hardMix", PSDFX_KEY('h','M','i','x') }, { "difference", PSDFX_KEY('d','i','f','f') },
      { "exclusion", PSDFX_KEY('s','m','u','d') }, { "blendSubtraction", PSDFX_KEY('f','s','u','b') },
      { "blendDivide", PSDFX_KEY('f','d','i','v') }, { "hue", PSDFX_KEY('h','u','e',' ') },
      { "saturation", PSDFX_KEY('s','a','t',' ') }, { "color", PSDFX_KEY('c','o','l','r') },
      { "luminosity", PSDFX_KEY('l','u','m',' ') },
    };
    for (const auto &m : kMap) if (v == m.name) return m.key;
    return blendFromEnumId(v);
  }

  static bool flag(Descriptor *d, const char *k, bool def) {
    auto *b = d ? dynamic_cast<DescriptorBoolean*>(d->item(k).find()) : nullptr;
    return b ? b->val : def;
  }

  static std::string enumOf(Descriptor *d, const char *k) {
    auto *e = d ? dynamic_cast<DescriptorEnumerated*>(d->item(k).find()) : nullptr;
    return e ? e->enumId : std::string();
  }

  // 割合 (スプレッド / チョーク)。単位がピクセルと書かれていても値は % なので
  // (Photoshop の合成画像と照合して確認)、常に /100 する。
  static double fraction(Descriptor *d, const char *k, double size) {
    (void)size;
    return std::min(1.0, std::max(0.0, num(d, k) / 100.0));
  }

  // 効果の描画に使うグラデーションの分岐点とパターンのタイルの置き場
  struct FxStore {
    std::vector<std::vector<psdfx_color_stop>> cs;
    std::vector<std::vector<psdfx_alpha_stop>> as;
    std::vector<std::vector<uint8_t>> tiles;
    std::vector<psdfx_surface> tileSurfaces;
    // 同じ種類の 2 つ目以降の効果
    std::vector<psdfx_shadow> moreDrop, moreInner;
    std::vector<psdfx_stroke> moreStroke;
    std::vector<psdfx_overlay> moreColor, moreGrad;
    // 効果の基準点 ('fxrp')。レイヤに整列するパターンの原点
    bool hasRef = false;
    double refX = 0, refY = 0;
    FxStore() { tileSurfaces.reserve(64); }
  };

  bool fillSource(Descriptor *d, const char *paintKey, psdfx_fill_source &f, FxStore &store, double scale) {
    f = psdfx_fill_source();
    f.scale = 1.0;
    // 塗りの種類: 'PntT' (境界線) が無ければ、持っているキーで決める
    std::string pt = paintKey ? enumOf(d, paintKey) : std::string();
    const bool isGrad = pt == "GrFl" || (pt.empty() && d->item("Grad").find());
    const bool isPat = pt == "Ptrn" || (pt.empty() && d->item("Ptrn").find() && !d->item("Clr ").find());
    if (isGrad) {
      store.cs.emplace_back(); store.as.emplace_back();
      if (!descGradient(dynamic_cast<Descriptor*>(d->item("Grad").find()), store.cs.back(),
                        store.as.back(), f.gradient)) {
        st_.unsupportedEffects++;   // ノイズグラデーション (乱数で色が決まる) など
        return false;
      }
      f.kind = PSDFX_FILL_GRADIENT;
      f.gradient.interpolation = gradientInterpolation(d);
      f.gradient_style = gradientStyle(d);
      f.angle = num(d, "Angl", 90);
      f.scale = num(d, "Scl ", 100) / 100.0;
      f.reverse = flag(d, "Rvrs", false);
      f.align_with_layer = flag(d, "Algn", true);
      auto *ofs = dynamic_cast<Descriptor*>(d->item("Ofst").find());
      f.offset_x = num(ofs, "Hrzn"); f.offset_y = num(ofs, "Vrtc");
      return true;
    }
    if (isPat) {
      auto *pd = dynamic_cast<Descriptor*>(d->item("Ptrn").find());
      auto *id = pd ? dynamic_cast<DescriptorString*>(pd->item("Idnt").find()) : nullptr;
      if (!id) return false;
      std::string want;
      for (char16_t ch : id->val) if (ch) want.push_back((char)ch);
      for (size_t i = 0; i < psd_.patterns.size(); i++) {
        if (psd_.patterns[i].id != want) continue;
        store.tiles.emplace_back();
        int tw = 0, th = 0;
        if (!psd_.getPatternImage((int)i, store.tiles.back(), tw, th)) return false;
        if (store.tileSurfaces.size() == store.tileSurfaces.capacity()) return false;
        store.tileSurfaces.push_back({ store.tiles.back().data(), tw, th, tw * 4 });
        f.kind = PSDFX_FILL_PATTERN;
        f.pattern = &store.tileSurfaces.back();
        f.scale = num(d, "Scl ", 100) / 100.0;
        f.align_with_layer = flag(d, "Algn", true);
        auto *ph = dynamic_cast<Descriptor*>(d->item("phase").find());
        f.phase_x = num(ph, "Hrzn"); f.phase_y = num(ph, "Vrtc");
        f.has_reference_point = store.hasRef ? 1 : 0;
        f.reference_x = store.refX; f.reference_y = store.refY;
        return true;
      }
      return false;
    }
    f.kind = PSDFX_FILL_SOLID;
    descColor(dynamic_cast<Descriptor*>(d->item("Clr ").find()), f.color);
    (void)scale;
    return true;
  }

  int globalAngle() {
    std::vector<uint8_t> b;
    for (auto &r : psd_.imageResourceList) {
      if (r.id != 1037 || !r.data) continue;
      IteratorBase *it = r.data->clone(); it->init();
      int v = it->rest() >= 4 ? it->getInt32() : 30;
      delete it;
      return v;
    }
    return 30;
  }
  int globalAltitude() {
    for (auto &r : psd_.imageResourceList) {
      if (r.id != 1049 || !r.data) continue;
      IteratorBase *it = r.data->clone(); it->init();
      int v = it->rest() >= 4 ? it->getInt32() : 30;
      delete it;
      return v;
    }
    return 30;
  }

  // 効果の 1 つ分 (key)。新しい形式の配列 (multiKey) なら有効な最初の要素。
  static Descriptor *effectDesc(Descriptor &fx, const char *key, const char *multiKey) {
    auto *d = dynamic_cast<Descriptor*>(fx.item(key).find());
    if (d && flag(d, "enab", true)) return d;
    if (multiKey) {
      if (auto *list = dynamic_cast<DescriptorList*>(fx.item(multiKey).find())) {
        for (auto *it : list->items) {
          auto *e = dynamic_cast<Descriptor*>(it);
          if (e && flag(e, "enab", true)) return e;
        }
      }
    }
    return nullptr;
  }

  // 有効な効果を一覧の順に (*Multi の一覧があればそれ、無ければ単独のキー)
  static std::vector<Descriptor*> effectList(Descriptor &fx, const char *key, const char *multiKey) {
    std::vector<Descriptor*> out;
    if (multiKey) {
      if (auto *list = dynamic_cast<DescriptorList*>(fx.item(multiKey).find())) {
        for (auto *it : list->items) {
          auto *e = dynamic_cast<Descriptor*>(it);
          if (e && flag(e, "enab", true)) out.push_back(e);
        }
        if (!out.empty()) return out;
      }
    }
    auto *d = dynamic_cast<Descriptor*>(fx.item(key).find());
    if (d && flag(d, "enab", true)) out.push_back(d);
    return out;
  }

  // layer の lfx2 を読む。描く効果が無ければ false。
  // 1 バイトの旗の追加情報 ('clbl' / 'infx' / 'knko' / 'tsly' など)。無ければ def
  static int flagBlock(const LayerInfo &l, int key, int def) {
    for (const auto &a : l.extraData.additionalLayers) {
      if (a.key != key || !a.data) continue;
      IteratorBase *r = a.data->clone(); r->init();
      const int v = r->rest() > 0 ? r->getCh() : def;
      delete r;
      return v;
    }
    return def;
  }

  bool layerEffects(const LayerInfo &l, psdfx_layer_effects &fx, FxStore &store) {
    fx = psdfx_layer_effects();
    Descriptor d;
    for (const auto &a : l.extraData.additionalLayers) {
      if (a.key != 'fxrp' || !a.data) continue;
      IteratorBase *r = a.data->clone(); r->init();
      if (r->rest() >= 16) {
        pun64 x, y;
        x.i = (uint64_t)r->getInt64(true); y.i = (uint64_t)r->getInt64(true);
        if (std::isfinite(x.f) && std::isfinite(y.f)) { store.hasRef = true; store.refX = x.f; store.refY = y.f; }
      }
      delete r;
    }
    // 'lfx2' が普通。同じ形で 'lmfx' / 'lfxs' に持つファイルもある
    if (!readDescriptor(l, 'lfx2', 8, d) && !readDescriptor(l, 'lmfx', 8, d) &&
        !readDescriptor(l, 'lfxs', 8, d)) return false;
    if (!flag(&d, "masterFXSwitch", true)) return false;
    // 'infx' (内部効果を描画モードとしてまとめる): 1 バイト目が 1 なら
    for (const auto &a : l.extraData.additionalLayers) {
      if (a.key != 'infx' || !a.data) continue;
      IteratorBase *r = a.data->clone(); r->init();
      fx.blend_interior_as_group = r->rest() > 0 && r->getCh() != 0;
      delete r;
    }
    // 効果全体の拡大率 'Scl ' は保存されている値に反映済みなので掛けない
    const double sc = 1.0;
    const int gAngle = globalAngle(), gAlt = globalAltitude();
    bool any = false;
    auto angleOf = [&](Descriptor *e) { return flag(e, "uglg", true) ? (double)gAngle : num(e, "lagl", 120); };

    auto shadow = [&](Descriptor *e, psdfx_shadow &s, bool drop) {
      s = psdfx_shadow();
      s.enabled = 1; any = true;
      s.blend = blendFromEnum(e, "Md  "); s.opacity = (float)(num(e, "Opct", 75) / 100.0);
      descColor(dynamic_cast<Descriptor*>(e->item("Clr ").find()), s.color);
      s.angle = angleOf(e); s.distance = num(e, "Dstn", 5) * sc;
      s.size = num(e, "blur", 5) * sc; s.spread = fraction(e, "Ckmt", num(e, "blur", 5));
      if (drop) s.knocks_out = flag(e, "layerConceals", true);
    };
    {
      auto list = effectList(d, "DrSh", "dropShadowMulti");
      for (size_t i = 0; i < list.size(); i++) {
        if (i == 0) shadow(list[i], fx.drop_shadow, true);
        else { store.moreDrop.emplace_back(); shadow(list[i], store.moreDrop.back(), true); }
      }
      auto ilist = effectList(d, "IrSh", "innerShadowMulti");
      for (size_t i = 0; i < ilist.size(); i++) {
        if (i == 0) shadow(ilist[i], fx.inner_shadow, false);
        else { store.moreInner.emplace_back(); shadow(ilist[i], store.moreInner.back(), false); }
      }
    }
    auto glow = [&](const char *key, psdfx_glow &g, bool inner) {
      Descriptor *e = effectDesc(d, key, nullptr);
      if (!e) return;
      g.enabled = 1; any = true;
      g.blend = blendFromEnum(e, "Md  "); g.opacity = (float)(num(e, "Opct", 75) / 100.0);
      fillSource(e, nullptr, g.fill, store, sc);
      g.size = num(e, "blur", 5) * sc; g.spread = fraction(e, "Ckmt", num(e, "blur", 5));
      g.precise = enumOf(e, "GlwT") == "PrBL";
      g.range = num(e, "Inpr", 50) / 100.0;
      g.source_center = inner && enumOf(e, "glwS") == "SrcC";
    };
    glow("OrGl", fx.outer_glow, false);
    glow("IrGl", fx.inner_glow, true);
    auto stroke = [&](Descriptor *e, psdfx_stroke &s) {
      s = psdfx_stroke();
      s.enabled = 1; any = true;
      s.blend = blendFromEnum(e, "Md  "); s.opacity = (float)(num(e, "Opct", 100) / 100.0);
      s.size = num(e, "Sz  ", 3) * sc;
      const std::string pos = enumOf(e, "Styl");
      s.position = pos == "InsF" ? PSDFX_STROKE_INSIDE : pos == "CtrF" ? PSDFX_STROKE_CENTER
                                                                      : PSDFX_STROKE_OUTSIDE;
      fillSource(e, "PntT", s.fill, store, sc);
    };
    {
      auto list = effectList(d, "FrFX", "frameFXMulti");
      for (size_t i = 0; i < list.size(); i++) {
        if (i == 0) stroke(list[i], fx.stroke);
        else { store.moreStroke.emplace_back(); stroke(list[i], store.moreStroke.back()); }
      }
    }
    auto overlayOf = [&](Descriptor *e, psdfx_overlay &o) {
      o = psdfx_overlay();
      o.blend = blendFromEnum(e, "Md  "); o.opacity = (float)(num(e, "Opct", 100) / 100.0);
      if (fillSource(e, nullptr, o.fill, store, sc)) { o.enabled = 1; any = true; }
    };
    auto overlays = [&](const char *key, const char *multi, psdfx_overlay &first,
                        std::vector<psdfx_overlay> &more) {
      auto list = effectList(d, key, multi);
      for (size_t i = 0; i < list.size(); i++) {
        if (i == 0) overlayOf(list[i], first);
        else { more.emplace_back(); overlayOf(list[i], more.back()); }
      }
    };
    overlays("SoFi", "solidFillMulti", fx.color_overlay, store.moreColor);
    overlays("GrFl", "gradientFillMulti", fx.gradient_overlay, store.moreGrad);
    if (Descriptor *e = effectDesc(d, "patternFill", nullptr)) overlayOf(e, fx.pattern_overlay);
    fx.more_drop_shadows = store.moreDrop.data();      fx.more_drop_shadow_count = (int)store.moreDrop.size();
    fx.more_inner_shadows = store.moreInner.data();    fx.more_inner_shadow_count = (int)store.moreInner.size();
    fx.more_strokes = store.moreStroke.data();         fx.more_stroke_count = (int)store.moreStroke.size();
    fx.more_color_overlays = store.moreColor.data();   fx.more_color_overlay_count = (int)store.moreColor.size();
    fx.more_gradient_overlays = store.moreGrad.data(); fx.more_gradient_overlay_count = (int)store.moreGrad.size();
    if (Descriptor *e = effectDesc(d, "ChFX", nullptr)) {
      psdfx_satin &s = fx.satin;
      s.enabled = 1; any = true;
      s.blend = blendFromEnum(e, "Md  "); s.opacity = (float)(num(e, "Opct", 50) / 100.0);
      descColor(dynamic_cast<Descriptor*>(e->item("Clr ").find()), s.color);
      s.angle = num(e, "lagl", 19); s.distance = num(e, "Dstn", 11) * sc;
      s.size = num(e, "blur", 14) * sc; s.invert = flag(e, "Invr", true);
    }
    if (Descriptor *e = effectDesc(d, "ebbl", nullptr)) {
      psdfx_bevel &b = fx.bevel;
      b.enabled = 1; any = true;
      const std::string st = enumOf(e, "bvlS");
      b.style = st == "OtrB" ? PSDFX_BEVEL_OUTER : st == "Embs" ? PSDFX_BEVEL_EMBOSS
              : st == "PlEb" ? PSDFX_BEVEL_PILLOW : PSDFX_BEVEL_INNER;
      b.up = enumOf(e, "bvlD") != "Out ";
      b.depth = num(e, "srgR", 100) / 100.0;
      b.size = num(e, "blur", 5) * sc; b.soften = num(e, "Sftn", 0) * sc;
      b.angle = angleOf(e); b.altitude = flag(e, "uglg", true) ? (double)gAlt : num(e, "Lald", 30);
      b.highlight_blend = blendFromEnum(e, "hglM"); b.shadow_blend = blendFromEnum(e, "sdwM");
      b.highlight_opacity = (float)(num(e, "hglO", 75) / 100.0);
      b.shadow_opacity = (float)(num(e, "sdwO", 75) / 100.0);
      descColor(dynamic_cast<Descriptor*>(e->item("hglC").find()), b.highlight_color);
      descColor(dynamic_cast<Descriptor*>(e->item("sdwC").find()), b.shadow_color);
      const std::string tech = enumOf(e, "bvlT");
      b.technique = tech == "PrBL" ? PSDFX_BEVEL_CHISEL_HARD : tech == "Slmt" ? PSDFX_BEVEL_CHISEL_SOFT
                                                                             : PSDFX_BEVEL_SMOOTH;
    }
    return any;
  }

  // チャンネル制限 ('brst'): 合成から外す色チャンネルの番号 (0 = R, 1 = G, 2 = B) の並び
  static std::vector<int> excludedChannels(const LayerInfo &l) {
    std::vector<int> out;
    for (const auto &a : l.extraData.additionalLayers) {
      if (a.key != 'brst' || !a.data) continue;
      IteratorBase *r = a.data->clone(); r->init();
      while (r->rest() >= 4) out.push_back(r->getInt32());
      delete r;
    }
    return out;
  }

  // 1 枚のレイヤを (効果込みで) dst へ重ねる。clipMask (dst と同じ大きさ) があれば
  // その範囲にだけ (クリッピング)。
  // チャンネル制限があれば、外したチャンネルを重ねる前の値に戻す。
  void drawLayer(LayerInfo &l, Canvas &surface, int sx, int sy, Canvas &dst, int dx, int dy,
                 float opacity, float fill, uint32_t blend, const uint8_t *clipMask) {
    const std::vector<int> excluded = excludedChannels(l);
    if (excluded.empty()) {
      drawLayerImpl(l, surface, sx, sy, dst, dx, dy, opacity, fill, blend, clipMask);
      return;
    }
    const std::vector<uint8_t> before = dst.px;
    drawLayerImpl(l, surface, sx, sy, dst, dx, dy, opacity, fill, blend, clipMask);
    for (int ch : excluded) {
      if (ch < 0 || ch > 2) continue;
      const int off = 2 - ch;   // BGRA の並びでの位置
      for (size_t i = (size_t)off; i < dst.px.size(); i += 4) dst.px[i] = before[i];
    }
  }

  void drawLayerImpl(LayerInfo &l, Canvas &surface, int sx, int sy, Canvas &dst, int dx, int dy,
                     float opacity, float fill, uint32_t blend, const uint8_t *clipMask) {
    psdfx_layer_effects fx;
    FxStore store;
    const bool withFx = opt_.effects && layerEffects(l, fx, store);
    psdfx_surface src = surface.surface();
    const double docBox[4] = { (double)-dx, (double)-dy,
                               (double)(psd_.header.width - dx), (double)(psd_.header.height - dy) };
    if (!clipMask) {
      psdfx_surface d = dst.surface();
      if (!withFx) psdfx_composite_layer(&d, &src, sx - dx, sy - dy, blend, opacity, fill, nullptr, 0);
      else psdfx_composite_with_effects(&d, &src, sx - dx, sy - dy, blend, opacity, fill, &fx, docBox,
                                        surface.shape.empty() ? nullptr : surface.shape.data(), surface.width);
      return;
    }
    // クリップされたレイヤ: 透明な面へ (効果込みで) 描いてから、クリップ範囲のマスク付きで重ねる
    Canvas tmp(dst.width, dst.height);
    psdfx_surface t = tmp.surface();
    psdfx_surface d = dst.surface();
    if (!withFx) {
      psdfx_composite(&t, &src, sx - dx, sy - dy, PSDFX_KEY('n','o','r','m'), 1.f, nullptr, 0);
      psdfx_composite_layer(&d, &t, 0, 0, blend, opacity, fill, clipMask, dst.width);
      return;
    }
    psdfx_composite_with_effects(&t, &src, sx - dx, sy - dy, PSDFX_KEY('n','o','r','m'), 1.f, fill, &fx, docBox,
                                 surface.shape.empty() ? nullptr : surface.shape.data(), surface.width);
    psdfx_composite(&d, &t, 0, 0, blend, opacity, clipMask, dst.width);
  }

  // 画素を持つレイヤ 1 枚を面にする (マスク込み)。left / top は面の左上の位置。空なら false。
  bool layerSurface(LayerInfo &l, Canvas &out, int &left, int &top) {
    LayerInfo maskLayer;
    const bool maskOn = userMaskLayer(l, maskLayer);
    // シェイプ (塗りつぶしレイヤ以外) は保存画素があればそれを使う (Photoshop が
    // 描いた塗り + 線)。塗りつぶしレイヤは保存画素が塗りの一部しか持たないことが
    // あるので、常に塗りから作る。
    if (l.layerType != LAYER_TYPE_FILL && l.width > 0 && l.height > 0) {
      left = l.left; top = l.top;
      out = Canvas(l.width, l.height);
      if (psd_.getLayerImage(l, out.px.data(), BGRA_LE, l.width * 4, IMAGE_MODE_IMAGE)) {
        bool any = false;
        for (size_t i = 3; i < out.px.size() && !any; i += 4) any = out.px[i] != 0;
        if (any) {
          if (maskOn) applyUserMask(maskLayer, out, left, top);
          // シェイプ ('vscg' を持つ) の保存画素はパスの形で描かれ済み。もう一度
          // マスクを掛けると縁のアンチエイリアスが 2 乗になるので、濃度・ぼかしが
          // 無ければ掛けない (照合で確認)
          const LayerMask &lm = l.extraData.layerMask;
          const bool vecParams = (lm.hasVectorFeather && lm.vectorMaskFeather > 0) ||
                                 (lm.vectorMaskDensity >= 0 && lm.vectorMaskDensity < 255);
          bool shape = false;
          for (const auto &a : l.extraData.additionalLayers) if (a.key == 'vscg') shape = true;
          if (!shape || vecParams || l.vectorMask.inverted()) applyVectorMask(l, out, left, top);
          return true;
        }
      }
    }
    // 塗りつぶしレイヤと、保存画素が無い / 空のシェイプ: 塗りそのものから作る。
    // 塗りつぶしレイヤの内容は文書全体に広がる (マスクの濃度で薄く見える所も含めて)
    // ので範囲は文書全体。シェイプはレイヤ矩形 (空なら文書全体)。
    {
      left = l.left; top = l.top;
      int w = l.width, h = l.height;
      if (l.layerType == LAYER_TYPE_FILL || w <= 0 || h <= 0) {
        // 文書全体。形が文書の外へはみ出していれば、その分も含める (形の縁が文書の
        // 外にあるとき、効果の内側の距離が文書の端から測られないように)
        int x0 = 0, y0 = 0, x1 = psd_.header.width, y1 = psd_.header.height;
        if (l.width > 0 && l.height > 0) {
          x0 = std::min(x0, l.left); y0 = std::min(y0, l.top);
          x1 = std::max(x1, l.right); y1 = std::max(y1, l.bottom);
        }
        left = x0; top = y0; w = x1 - x0; h = y1 - y0;
      }
      Canvas fill(w, h);
      if (l.layerType != LAYER_TYPE_FILL && paintShapeWithStroke(l, fill, left, top)) {
        out = std::move(fill);
        if (maskOn) {
          applyUserMask(maskLayer, out, left, top);
          Canvas m(w, h);
          for (size_t i = 0; i < out.shape.size(); i++) m.px[i * 4 + 3] = out.shape[i];
          applyUserMask(maskLayer, m, left, top);
          for (size_t i = 0; i < out.shape.size(); i++) out.shape[i] = m.px[i * 4 + 3];
        }
        return true;
      }
      if (paintFill(l, fill, left, top)) {
        out.width = fill.width; out.height = fill.height; out.px.swap(fill.px);
        if (maskOn) applyUserMask(maskLayer, out, left, top);
        applyVectorMask(l, out, left, top);
        // 効果は塗りの透明度ではなくマスク (シェイプ) の形から作る
        Canvas m(w, h);
        for (size_t i = 3; i < m.px.size(); i += 4) m.px[i] = 255;
        if (maskOn) applyUserMask(maskLayer, m, left, top);
        applyVectorMask(l, m, left, top);
        out.shape.resize((size_t)w * h);
        for (size_t i = 0; i < out.shape.size(); i++) out.shape[i] = m.px[i * 4 + 3];
        return true;
      }
    }
    if (l.width <= 0 || l.height <= 0) return false;
    left = l.left; top = l.top;
    out = Canvas(l.width, l.height);
    if (!psd_.getLayerImage(l, out.px.data(), BGRA_LE, l.width * 4, IMAGE_MODE_IMAGE))
      return false;
    if (maskOn) applyUserMask(maskLayer, out, left, top);
    applyVectorMask(l, out, left, top);
    return true;
  }

  // ユーザーマスクを面のアルファへ掛ける (矩形の外は既定色)
  void applyUserMask(LayerInfo &l, Canvas &c, int left, int top) {
    const LayerMask &m = l.extraData.layerMask;
    std::vector<uint8_t> bgra;
    const bool have = m.width > 0 && m.height > 0;
    if (have) {
      bgra.resize((size_t)m.width * m.height * 4);
      if (!psd_.getLayerImage(l, bgra.data(), BGRA_LE, m.width * 4, IMAGE_MODE_MASK)) bgra.clear();
    }
    std::vector<uint8_t> mk((size_t)c.width * c.height);
    for (int y = 0; y < c.height; y++) {
      for (int x = 0; x < c.width; x++) {
        int mx = left + x - m.left, my = top + y - m.top;
        uint8_t v = (uint8_t)m.defaultColor;
        if (!bgra.empty() && mx >= 0 && my >= 0 && mx < m.width && my < m.height)
          v = bgra[((size_t)my * m.width + mx) * 4];
        mk[(size_t)y * c.width + x] = v;
      }
    }
    if (m.hasUserFeather && m.userMaskFeather > 0)
      psdfx_blur_plane(mk.data(), c.width, c.height, c.width, m.userMaskFeather);   // σ = ぼかしの値 (Photoshop で測定)
    if (m.userMaskDensity >= 0 && m.userMaskDensity < 255) applyDensity(mk, m.userMaskDensity);
    for (size_t i = 0; i < mk.size(); i++)
      c.px[i * 4 + 3] = (uint8_t)((c.px[i * 4 + 3] * mk[i] + 127) / 255);
  }

  // 1 つの兄弟 (レイヤかグループ) と、それにクリップされたレイヤ群を canvas へ
  void renderWithClipped(int idx, const std::vector<int> &clipped, Canvas &canvas) {
    LayerInfo &l = psd_.layerList[(size_t)idx];
    if (!visible(l)) return;   // 下地が非表示ならクリップ側も見えない

    if (l.layerType == LAYER_TYPE_FOLDER) {
      if (clipped.empty()) { renderGroup(idx, canvas); return; }
      // グループを下地にしたクリッピング: グループの中身を独立した面に描き、その
      // 形をクリップの範囲にする
      Canvas group(canvas.width, canvas.height);
      renderGroupContent(idx, group);
      std::vector<uint8_t> clipMask((size_t)canvas.width * canvas.height);
      for (size_t i = 0; i < clipMask.size(); i++) clipMask[i] = group.px[i * 4 + 3];
      const int key = l.sectionBlendKey ? l.sectionBlendKey : l.blendModeKey;
      if (key == 'pass' && l.fill_opacity >= 255) {
        // 通過グループは下の画像と混ざるように描くが、その効き目はグループの中身の
        // 形の中だけ (中が調整レイヤだけなら何も変わらない。照合で確認)。その上へ
        // クリップされたレイヤを範囲内に重ねる
        Canvas after = canvas;
        renderGroup(idx, after);
        std::vector<uint8_t> inside(clipMask.size());
        for (size_t i = 0; i < inside.size(); i++) inside[i] = clipMask[i] ? 255 : 0;
        psdfx_surface dst = canvas.surface(), src = after.surface();
        psdfx_lerp(&dst, &src, 1.f, inside.data(), canvas.width);
        drawClipped(clipped, canvas, clipMask.data());
        return;
      }
      drawClipped(clipped, group, clipMask.data());
      psdfx_surface dst = canvas.surface(), src = group.surface();
      psdfx_composite(&dst, &src, 0, 0, key == 'pass' ? PSDFX_KEY('n','o','r','m') : (uint32_t)key,
                      l.opacity / 255.f * (l.fill_opacity / 255.f), nullptr, 0);
      return;
    }
    if (l.layerType == LAYER_TYPE_ADJUST) {
      if (clipped.empty()) { applyAdjustment(l, canvas, nullptr); return; }
      // 調整レイヤを下地にしたクリッピング: 調整済みの画像を下地の中身、調整の
      // マスクをクリップの範囲とみなす
      AdjustmentInfo a;
      Canvas group = canvas;
      psdfx_surface gs = group.surface();
      if (!decodeAdjustment(l, a) || !a.valid || !adjustSurface(a, gs)) st_.skippedAdjustments++;
      std::vector<uint8_t> mask = adjustmentMask(l, nullptr, canvas.width, canvas.height);
      drawClipped(clipped, group, mask.data());
      psdfx_surface d = canvas.surface();
      psdfx_apply_adjusted(&d, &gs, (uint32_t)l.blendModeKey, l.opacity / 255.f * (l.fill_opacity / 255.f),
                           mask.data(), canvas.width);
      return;
    }

    Canvas base;
    int bx = 0, by = 0;
    if (!layerSurface(l, base, bx, by)) return;
    const float opacity = l.opacity / 255.f;
    const float fill = l.fill_opacity / 255.f;
    if (const int kn = flagBlock(l, 'knko', 0)) {
      std::vector<uint8_t> shape((size_t)canvas.width * canvas.height, 0);
      for (int y = 0; y < base.height; y++) {
        const int dy = by + y;
        if (dy < 0 || dy >= canvas.height) continue;
        for (int x = 0; x < base.width; x++) {
          const int dx = bx + x;
          if (dx < 0 || dx >= canvas.width) continue;
          const size_t si = (size_t)y * base.width + x;
          shape[(size_t)dy * canvas.width + dx] = base.shape.empty() ? base.px[si * 4 + 3] : base.shape[si];
        }
      }
      knockOut(canvas, shape, opacity, kn);
    }

    if (clipped.empty()) {
      drawLayer(l, base, bx, by, canvas, 0, 0, opacity, fill, (uint32_t)l.blendModeKey, nullptr);
      return;
    }
    // 下地 (効果込み) の上にクリップされたレイヤを重ね、まとめて下へ。クリップの範囲は
    // 下地の画素の形 (塗りの不透明度や効果は含まない。照合で確認)。
    Canvas group(canvas.width, canvas.height);
    drawLayer(l, base, bx, by, group, 0, 0, 1.f, fill, PSDFX_KEY('n','o','r','m'), nullptr);
    std::vector<uint8_t> clipMask((size_t)canvas.width * canvas.height, 0);
    for (int y = 0; y < base.height; y++) {
      const int dy = by + y;
      if (dy < 0 || dy >= canvas.height) continue;
      for (int x = 0; x < base.width; x++) {
        const int dx = bx + x;
        if (dx < 0 || dx >= canvas.width) continue;
        clipMask[(size_t)dy * canvas.width + dx] = base.px[((size_t)y * base.width + x) * 4 + 3];
      }
    }
    if (flagBlock(l, 'clbl', 1) == 0) {
      // 「クリップしたレイヤーをグループとして描画」が切: 下地を描いてから、クリップ
      // されたレイヤを下地の形の範囲で直接下の画像へ重ねる
      drawLayer(l, base, bx, by, canvas, 0, 0, opacity, fill, (uint32_t)l.blendModeKey, nullptr);
      drawClipped(clipped, canvas, clipMask.data());
      return;
    }
    {
      psdfx_layer_effects fx;
      FxStore store;
      if (opt_.effects && layerEffects(l, fx, store)) {
        // 下地に効果があるときは、下地の画素にクリップされたレイヤを重ねてから
        // 効果を掛ける (オーバーレイなどはクリップされたレイヤの上にも乗る。照合で
        // 確認)。効果の形は下地の形
        Canvas content(canvas.width, canvas.height);
        psdfx_surface cd = content.surface(), bs = base.surface();
        psdfx_composite(&cd, &bs, bx, by, PSDFX_KEY('n','o','r','m'), fill, nullptr, 0);
        drawClipped(clipped, content, clipMask.data());
        content.shape.assign(clipMask.size(), 0);
        for (int y = 0; y < base.height; y++) {
          const int dy = by + y;
          if (dy < 0 || dy >= canvas.height) continue;
          for (int x = 0; x < base.width; x++) {
            const int dx = bx + x;
            if (dx < 0 || dx >= canvas.width) continue;
            const size_t si = (size_t)y * base.width + x;
            content.shape[(size_t)dy * canvas.width + dx] = base.shape.empty() ? base.px[si * 4 + 3] : base.shape[si];
          }
        }
        drawLayer(l, content, 0, 0, canvas, 0, 0, opacity, 1.f, (uint32_t)l.blendModeKey, nullptr);
        return;
      }
    }
    drawClipped(clipped, group, clipMask.data());
    psdfx_surface dst = canvas.surface(), src = group.surface();
    psdfx_composite(&dst, &src, 0, 0, (uint32_t)l.blendModeKey, opacity, nullptr, 0);
  }

  // クリップされたレイヤを、下地の面 group (文書大) へ clipMask の範囲で重ねる
  void drawClipped(const std::vector<int> &clipped, Canvas &group, const uint8_t *clipMask) {
    for (int ci : clipped) {
      LayerInfo &c = psd_.layerList[(size_t)ci];
      if (!visible(c)) continue;
      if (c.layerType == LAYER_TYPE_ADJUST) { applyAdjustment(c, group, clipMask); continue; }
      if (c.layerType == LAYER_TYPE_FOLDER) {
        // クリップされたグループ: 中身を独立した面に描いてから範囲内に重ねる
        Canvas buf(group.width, group.height);
        renderGroupContent(ci, buf);
        const int key = c.sectionBlendKey ? c.sectionBlendKey : c.blendModeKey;
        psdfx_surface d = group.surface(), s = buf.surface();
        psdfx_composite(&d, &s, 0, 0, key == 'pass' ? PSDFX_KEY('n','o','r','m') : (uint32_t)key,
                        c.opacity / 255.f * (c.fill_opacity / 255.f), clipMask, group.width);
        continue;
      }
      Canvas cs;
      int cx = 0, cy = 0;
      if (!layerSurface(c, cs, cx, cy)) continue;
      drawLayer(c, cs, cx, cy, group, 0, 0, c.opacity / 255.f, c.fill_opacity / 255.f,
                (uint32_t)c.blendModeKey, clipMask);
    }
  }

  // グループの中身を透明な面 buf へ描き、グループのマスクを掛ける (不透明度と
  // ブレンドは呼び出し側)
  void renderGroupContent(int idx, Canvas &buf) {
    LayerInfo &g = psd_.layerList[(size_t)idx];
    renderChildren(idx, buf);
    std::vector<uint8_t> mask = groupMask(g);
    if (!mask.empty())
      for (size_t i = 0; i < mask.size(); i++)
        buf.px[i * 4 + 3] = (uint8_t)((buf.px[i * 4 + 3] * mask[i] + 127) / 255);
  }

  void renderGroup(int idx, Canvas &canvas) {
    LayerInfo &g = psd_.layerList[(size_t)idx];
    const float opacity = g.opacity / 255.f;
    const int key = g.sectionBlendKey ? g.sectionBlendKey : g.blendModeKey;
    std::vector<uint8_t> mask = groupMask(g);
    if (g.artboard.present) {
      renderArtboard(idx, canvas);
      return;
    }
    // 効果の付いたグループ: 中身を独立した面に描き、レイヤと同じく効果込みで重ねる
    // (通過グループも独立して描く)
    {
      psdfx_layer_effects fx;
      FxStore store;
      if (opt_.effects && layerEffects(g, fx, store)) {
        Canvas buf(canvas.width, canvas.height);
        renderGroupContent(idx, buf);
        if (flagBlock(g, 'knko', 0)) {
          std::vector<uint8_t> shape(buf.px.size() / 4);
          for (size_t i = 0; i < shape.size(); i++) shape[i] = buf.px[i * 4 + 3];
          knockOut(canvas, shape, opacity, flagBlock(g, 'knko', 0));
        }
        drawLayer(g, buf, 0, 0, canvas, 0, 0, opacity, g.fill_opacity / 255.f,
                  key == 'pass' ? PSDFX_KEY('n','o','r','m') : (uint32_t)key, nullptr);
        return;
      }
    }
    // 通過グループでも塗りの不透明度が 100% 未満なら、独立した面に描いて通常で
    // 重ねる (中の調整レイヤは下の画像に届かない。照合で確認)
    if (key == 'pass' && g.fill_opacity >= 255) {
      if (opacity >= 1.f && mask.empty() && !flagBlock(g, 'knko', 0)) {
        renderChildren(idx, canvas, true);
        return;
      }
      if (flagBlock(g, 'knko', 0)) { renderKnockoutGroup(idx, canvas); return; }
      Canvas after = canvas;
      renderChildren(idx, after, true);
      psdfx_surface dst = canvas.surface(), src = after.surface();
      psdfx_lerp(&dst, &src, opacity, mask.empty() ? nullptr : mask.data(), canvas.width);
      return;
    }
    if (flagBlock(g, 'knko', 0)) { renderKnockoutGroup(idx, canvas); return; }
    Canvas buf(canvas.width, canvas.height);
    renderChildren(idx, buf);
    psdfx_surface dst = canvas.surface(), src = buf.surface();
    psdfx_composite(&dst, &src, 0, 0, key == 'pass' ? PSDFX_KEY('n','o','r','m') : (uint32_t)key,
                    opacity * g.fill_opacity / 255.f,
                    mask.empty() ? nullptr : mask.data(), canvas.width);
  }

  // --- ノックアウト ---------------------------------------------------------------
  //
  // ノックアウトの付いたレイヤ / グループは、自分の形の中の下の画像を「行き先」に
  // 置き換えてから、自分を塗りの不透明度で重ねる (照合で確認)。
  //   浅い (1): いちばん内側のグループの開始時点の画像 (独立したグループなら透明)
  //   深い (2): 背景レイヤ (無ければ透明)。途中に独立したグループがあればそこで止まる
  // 外側にグループが無いときは浅い場合も背景レイヤまで。

  const Canvas *backgroundCanvas() {
    if (!background_) {
      background_.reset(new Canvas(psd_.header.width, psd_.header.height));
      // 合成画像に透明度の無い文書の一番下の通常レイヤを背景レイヤとみなす
      if (!psd_.mergedHasTransparency() && !psd_.layerList.empty()) {
        LayerInfo &b = psd_.layerList[0];
        Canvas surf; int x = 0, y = 0;
        if (b.layerType == LAYER_TYPE_NORMAL && b.parentIndex < 0 && visible(b) && layerSurface(b, surf, x, y)) {
          psdfx_surface d = background_->surface(), s2 = surf.surface();
          psdfx_composite(&d, &s2, x, y, PSDFX_KEY('n','o','r','m'), 1.f, nullptr, 0);
        }
      }
    }
    return background_.get();
  }

  // 行き先 (nullptr = 透明)
  const Canvas *knockoutTarget(int mode) {
    if (mode == 1 && !scopes_.empty()) return scopes_.back();
    for (size_t i = scopes_.size(); i-- > 0;) if (!scopes_[i]) return nullptr;
    return backgroundCanvas();
  }

  // canvas のうち形 shape (文書大、0..255) x opacity の分を行き先に置き換える
  void knockOut(Canvas &canvas, const std::vector<uint8_t> &shape, float opacity, int mode) {
    const Canvas *target = knockoutTarget(mode);
    Canvas transparent;
    if (!target) { transparent = Canvas(canvas.width, canvas.height); target = &transparent; }
    std::vector<uint8_t> k(shape.size());
    for (size_t i = 0; i < k.size(); i++) k[i] = (uint8_t)(shape[i] * opacity + 0.5f);
    psdfx_surface d = canvas.surface(), s2 = const_cast<Canvas*>(target)->surface();
    psdfx_lerp(&d, &s2, 1.f, k.data(), canvas.width);
  }

  // ノックアウトの付いたグループ: 中身を独立した面に描き、その形で下を抜いてから
  // 塗りの不透明度で重ねる
  void renderKnockoutGroup(int idx, Canvas &canvas) {
    LayerInfo &g = psd_.layerList[(size_t)idx];
    const int key = g.sectionBlendKey ? g.sectionBlendKey : g.blendModeKey;
    Canvas buf(canvas.width, canvas.height);
    renderGroupContent(idx, buf);
    std::vector<uint8_t> shape(buf.px.size() / 4);
    for (size_t i = 0; i < shape.size(); i++) shape[i] = buf.px[i * 4 + 3];
    knockOut(canvas, shape, g.opacity / 255.f, flagBlock(g, 'knko', 0));
    psdfx_surface dst = canvas.surface(), src = buf.surface();
    psdfx_composite(&dst, &src, 0, 0, key == 'pass' ? PSDFX_KEY('n','o','r','m') : (uint32_t)key,
                    g.opacity / 255.f * (g.fill_opacity / 255.f), nullptr, 0);
  }

  // グループのマスク (ユーザーマスク / ベクタマスク、濃度・ぼかし込み) を文書大で。
  // どちらも無ければ空。
  std::vector<uint8_t> groupMask(LayerInfo &g) {
    LayerInfo maskLayer;
    const bool user = userMaskLayer(g, maskLayer);
    const bool vec = g.vectorMask.present && !g.vectorMask.disabled();
    std::vector<uint8_t> out;
    if (!user && !vec) return out;
    const int W = psd_.header.width, H = psd_.header.height;
    Canvas m(W, H);
    for (size_t i = 3; i < m.px.size(); i += 4) m.px[i] = 255;
    if (user) applyUserMask(maskLayer, m, 0, 0);
    applyVectorMask(g, m, 0, 0);
    out.resize((size_t)W * H);
    for (size_t i = 0; i < out.size(); i++) out[i] = m.px[i * 4 + 3];
    return out;
  }

  // アートボード: 枠を背景色で塗った独立した面へ中身を描き、枠の外を落として重ねる。
  // 背景: 1 白 (既定) / 2 黒 / 3 透明 / 4 指定色
  void renderArtboard(int idx, Canvas &canvas) {
    LayerInfo &g = psd_.layerList[(size_t)idx];
    const ArtboardInfo &ab = g.artboard;
    const int x0 = std::max(0, (int)std::floor(ab.left)), y0 = std::max(0, (int)std::floor(ab.top));
    const int x1 = std::min(canvas.width, (int)std::ceil(ab.right));
    const int y1 = std::min(canvas.height, (int)std::ceil(ab.bottom));
    Canvas buf(canvas.width, canvas.height);
    uint8_t bg[4] = { 255, 255, 255, 255 };   // B, G, R, A
    switch (ab.backgroundType) {
    case 2: bg[0] = bg[1] = bg[2] = 0; break;
    case 3: bg[3] = 0; break;
    case 4:
      if (ab.hasColor) {
        bg[0] = (uint8_t)std::min(255.0, std::max(0.0, ab.color[2]));
        bg[1] = (uint8_t)std::min(255.0, std::max(0.0, ab.color[1]));
        bg[2] = (uint8_t)std::min(255.0, std::max(0.0, ab.color[0]));
      }
      break;
    default: break;
    }
    if (bg[3])
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) std::memcpy(&buf.px[((size_t)y * buf.width + x) * 4], bg, 4);
    renderChildren(idx, buf);
    for (int y = 0; y < buf.height; y++)
      for (int x = 0; x < buf.width; x++)
        if (x < x0 || x >= x1 || y < y0 || y >= y1) buf.px[((size_t)y * buf.width + x) * 4 + 3] = 0;
    psdfx_surface dst = canvas.surface(), src = buf.surface();
    psdfx_composite(&dst, &src, 0, 0, PSDFX_KEY('n','o','r','m'), g.opacity / 255.f, nullptr, 0);
  }
};

}  // anonymous namespace

bool PSDFile::shapeMask(int index, ShapePart part, std::vector<uint8_t> &mask, int &left, int &top,
                        int &width, int &height, int64_t maxPixels) {
  mask.clear();
  left = top = width = height = 0;
  if (index < 0 || index >= (int)layerList.size()) return false;
  const LayerInfo &l = layerList[(size_t)index];
  const VectorMask &vm = l.vectorMask;
  if (!vm.present) return false;
  PathBuf pb;
  toPsdfx(vm.path, header.width, header.height, pb);
  ShapeInfo si;
  psdfx_stroke_style st;
  std::vector<double> dashes;
  const bool wantFill = part != SHAPE_PART_STROKE, wantStroke = part != SHAPE_PART_FILL;
  const bool hasStroke = wantStroke && decodeShape(l, si) && si.hasStroke && si.stroke.strokeEnabled &&
                         toStrokeStyle(si.stroke, header.hres, st, dashes);
  if (!wantFill && !hasStroke) return false;
  // 範囲: 制御点を含むパスの外接矩形を、線がはみ出す分だけ広げる。
  // 反転したマスクの塗りは文書全体。
  bool any = false;
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  for (const auto &ks : pb.knots)
    for (const auto &k : ks)
      for (int i = 0; i < 3; i++) {
        const double x = i == 0 ? k.in_x : i == 1 ? k.x : k.out_x;
        const double y = i == 0 ? k.in_y : i == 1 ? k.y : k.out_y;
        if (!any) { x0 = x1 = x; y0 = y1 = y; any = true; }
        x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
      }
  if (!any && !(wantFill && (vm.inverted() || pb.initialFill))) return false;
  double reach = 1.0;
  if (hasStroke) {
    double r = st.width;
    if (st.join == PSDFX_JOIN_MITER) r *= std::max(1.0, std::min(st.miter_limit, 10.0));
    if (st.cap == PSDFX_CAP_SQUARE) r = std::max(r, st.width * 1.5);
    reach += r;
  }
  int l0 = (int)std::floor(x0 - reach), t0 = (int)std::floor(y0 - reach);
  int r0 = (int)std::ceil(x1 + reach), b0 = (int)std::ceil(y1 + reach);
  if (wantFill && (vm.inverted() || pb.initialFill)) {
    l0 = any ? std::min(l0, 0) : 0; t0 = any ? std::min(t0, 0) : 0;
    r0 = any ? std::max(r0, header.width) : header.width;
    b0 = any ? std::max(b0, header.height) : header.height;
  }
  const int64_t w = (int64_t)r0 - l0, h = (int64_t)b0 - t0;
  if (w <= 0 || h <= 0 || w * h > maxPixels) return false;
  left = l0; top = t0; width = (int)w; height = (int)h;
  mask.assign((size_t)(w * h), 0);
  if (wantFill) {
    psdfx_fill_path(pb.subs.data(), (int)pb.subs.size(), pb.initialFill, mask.data(),
                    width, height, width, left, top);
    if (vm.inverted()) for (auto &v : mask) v = (uint8_t)(255 - v);
  }
  if (hasStroke) {
    std::vector<uint8_t> sm(mask.size());
    psdfx_stroke_path(pb.subs.data(), (int)pb.subs.size(), pb.initialFill, &st, sm.data(),
                      width, height, width, left, top);
    for (size_t i = 0; i < mask.size(); i++) mask[i] = std::max(mask[i], sm[i]);
  }
  return true;
}

bool PSDFile::renderLayer(int index, std::vector<uint8_t> &bgra, int &left, int &top,
                          int &width, int &height, const CompositeOptions &opt,
                          CompositeStats *stats) {
  CompositeStats local;
  CompositeStats &st = stats ? *stats : local;
  st = CompositeStats();
  if (index < 0 || index >= (int)layerList.size()) return false;
  Compositor c(*this, opt, st);
  return c.renderSingle(index, bgra, left, top, width, height);
}

bool PSDFile::compositeImage(std::vector<uint8_t> &bgra, const CompositeOptions &opt,
                             CompositeStats *stats) {
  CompositeStats local;
  CompositeStats &st = stats ? *stats : local;
  st = CompositeStats();
  const int W = header.width, H = header.height;
  if (W <= 0 || H <= 0 || W > 300000 || H > 300000) return false;
  if ((int64_t)W * H > (int64_t)opt.maxPixels) return false;
  Canvas canvas(W, H);
  if (opt.background) {
    for (size_t i = 0; i < canvas.px.size(); i += 4) {
      canvas.px[i] = opt.backgroundColor[2]; canvas.px[i + 1] = opt.backgroundColor[1];
      canvas.px[i + 2] = opt.backgroundColor[0]; canvas.px[i + 3] = 255;
    }
  }
  Compositor c(*this, opt, st);
  c.renderChildren(-1, canvas);
  bgra.swap(canvas.px);
  return true;
}

}  // namespace psd
