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
#include <vector>

namespace psd {

namespace {

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

  void renderChildren(int parent, Canvas &canvas) {
    std::vector<int> kids = psd_.childIndices(parent);
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
    return true;
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
        // 原点はシェイプの左上 + 位相 (Photoshop の合成画像と照合して決めた)
        auto *ph = dynamic_cast<Descriptor*>(d.item("phase").find());
        double box[4];
        shapeBox(l, box);
        psdfx_draw_pattern(&s, left, top, &ts, num(&d, "Scl ", 100) / 100.0,
                           std::floor(box[0]) + num(ph, "Hrzn"), std::floor(box[1]) + num(ph, "Vrtc"));
        return true;
      }
      return false;
    }
    return false;
  }

  // ベクタマスクを、面 (左上が文書の (left, top)) の大きさでアルファへ掛ける
  void applyVectorMask(const LayerInfo &l, Canvas &c, int left, int top) {
    const VectorMask &vm = l.vectorMask;
    if (!vm.present || vm.disabled()) return;
    const double W = psd_.header.width, H = psd_.header.height;
    std::vector<std::vector<psdfx_knot>> knots;
    std::vector<psdfx_subpath> subs;
    for (const auto &sp : vm.path.subpaths) {
      std::vector<psdfx_knot> ks;
      for (const auto &k : sp.knots)
        ks.push_back({ k.preceding.x * W, k.preceding.y * H, k.anchor.x * W, k.anchor.y * H,
                       k.leaving.x * W, k.leaving.y * H });
      knots.push_back(ks);
    }
    for (size_t i = 0; i < knots.size(); i++) {
      const auto &sp = vm.path.subpaths[i];
      subs.push_back({ knots[i].data(), (int)knots[i].size(), sp.closed ? 1 : 0, sp.operation });
    }
    std::vector<uint8_t> m((size_t)c.width * c.height);
    psdfx_fill_path(subs.data(), (int)subs.size(), vm.path.initialFill == 1 ? 1 : 0,
                    m.data(), c.width, c.height, c.width, left, top);
    if (vm.inverted()) for (auto &v : m) v = (uint8_t)(255 - v);
    const LayerMask &lm = l.extraData.layerMask;
    if (lm.hasVectorFeather && lm.vectorMaskFeather > 0)
      psdfx_blur_plane(m.data(), c.width, c.height, c.width, lm.vectorMaskFeather / 2.0);
    if (lm.vectorMaskDensity >= 0 && lm.vectorMaskDensity < 255) applyDensity(m, lm.vectorMaskDensity);
    for (size_t i = 0; i < m.size(); i++)
      c.px[i * 4 + 3] = (uint8_t)((c.px[i * 4 + 3] * m[i] + 127) / 255);
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
    const std::string v = e ? e->enumId : std::string("Nrml");
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
    FxStore() { tileSurfaces.reserve(16); }
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

  // layer の lfx2 を読む。描く効果が無ければ false。
  bool layerEffects(const LayerInfo &l, psdfx_layer_effects &fx, FxStore &store) {
    fx = psdfx_layer_effects();
    Descriptor d;
    if (!readDescriptor(l, 'lfx2', 8, d)) return false;
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

    if (Descriptor *e = effectDesc(d, "DrSh", "dropShadowMulti")) {
      psdfx_shadow &s = fx.drop_shadow;
      s.enabled = 1; any = true;
      s.blend = blendFromEnum(e, "Md  "); s.opacity = (float)(num(e, "Opct", 75) / 100.0);
      descColor(dynamic_cast<Descriptor*>(e->item("Clr ").find()), s.color);
      s.angle = angleOf(e); s.distance = num(e, "Dstn", 5) * sc;
      s.size = num(e, "blur", 5) * sc; s.spread = fraction(e, "Ckmt", num(e, "blur", 5));
      s.knocks_out = flag(e, "layerConceals", true);
    }
    if (Descriptor *e = effectDesc(d, "IrSh", "innerShadowMulti")) {
      psdfx_shadow &s = fx.inner_shadow;
      s.enabled = 1; any = true;
      s.blend = blendFromEnum(e, "Md  "); s.opacity = (float)(num(e, "Opct", 75) / 100.0);
      descColor(dynamic_cast<Descriptor*>(e->item("Clr ").find()), s.color);
      s.angle = angleOf(e); s.distance = num(e, "Dstn", 5) * sc;
      s.size = num(e, "blur", 5) * sc; s.spread = fraction(e, "Ckmt", num(e, "blur", 5));
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
    if (Descriptor *e = effectDesc(d, "FrFX", "frameFXMulti")) {
      psdfx_stroke &s = fx.stroke;
      s.enabled = 1; any = true;
      s.blend = blendFromEnum(e, "Md  "); s.opacity = (float)(num(e, "Opct", 100) / 100.0);
      s.size = num(e, "Sz  ", 3) * sc;
      const std::string pos = enumOf(e, "Styl");
      s.position = pos == "InsF" ? PSDFX_STROKE_INSIDE : pos == "CtrF" ? PSDFX_STROKE_CENTER
                                                                      : PSDFX_STROKE_OUTSIDE;
      fillSource(e, "PntT", s.fill, store, sc);
    }
    auto overlay = [&](const char *key, const char *multi, psdfx_overlay &o) {
      Descriptor *e = effectDesc(d, key, multi);
      if (!e) return;
      o.blend = blendFromEnum(e, "Md  "); o.opacity = (float)(num(e, "Opct", 100) / 100.0);
      if (fillSource(e, nullptr, o.fill, store, sc)) { o.enabled = 1; any = true; }
    };
    overlay("SoFi", "solidFillMulti", fx.color_overlay);
    overlay("GrFl", "gradientFillMulti", fx.gradient_overlay);
    overlay("patternFill", nullptr, fx.pattern_overlay);
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
      if (!withFx) psdfx_composite(&d, &src, sx - dx, sy - dy, blend, opacity * fill, nullptr, 0);
      else psdfx_composite_with_effects(&d, &src, sx - dx, sy - dy, blend, opacity, fill, &fx, docBox,
                                        surface.shape.empty() ? nullptr : surface.shape.data(), surface.width);
      return;
    }
    // クリップされたレイヤ: 透明な面へ (効果込みで) 描いてから、クリップ範囲のマスク付きで重ねる
    Canvas tmp(dst.width, dst.height);
    psdfx_surface t = tmp.surface();
    if (!withFx) psdfx_composite(&t, &src, sx - dx, sy - dy, PSDFX_KEY('n','o','r','m'), fill, nullptr, 0);
    else psdfx_composite_with_effects(&t, &src, sx - dx, sy - dy, PSDFX_KEY('n','o','r','m'), 1.f, fill, &fx, docBox,
                                      surface.shape.empty() ? nullptr : surface.shape.data(), surface.width);
    psdfx_surface d = dst.surface();
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
          applyVectorMask(l, out, left, top);
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
        left = 0; top = 0; w = psd_.header.width; h = psd_.header.height;
      }
      Canvas fill(w, h);
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
      psdfx_blur_plane(mk.data(), c.width, c.height, c.width, m.userMaskFeather / 2.0);
    if (m.userMaskDensity >= 0 && m.userMaskDensity < 255) applyDensity(mk, m.userMaskDensity);
    for (size_t i = 0; i < mk.size(); i++)
      c.px[i * 4 + 3] = (uint8_t)((c.px[i * 4 + 3] * mk[i] + 127) / 255);
  }

  // 1 つの兄弟 (レイヤかグループ) と、それにクリップされたレイヤ群を canvas へ
  void renderWithClipped(int idx, const std::vector<int> &clipped, Canvas &canvas) {
    LayerInfo &l = psd_.layerList[(size_t)idx];
    if (!visible(l)) return;   // 下地が非表示ならクリップ側も見えない

    if (l.layerType == LAYER_TYPE_FOLDER) {
      renderGroup(idx, canvas);   // グループを下地にしたクリッピングはまだ扱わない
      if (!clipped.empty()) st_.unsupportedClipBase++;
      return;
    }
    if (l.layerType == LAYER_TYPE_ADJUST) {
      st_.skippedAdjustments++;
      return;
    }

    Canvas base;
    int bx = 0, by = 0;
    if (!layerSurface(l, base, bx, by)) return;
    const float opacity = l.opacity / 255.f;
    const float fill = l.fill_opacity / 255.f;

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
    for (int ci : clipped) {
      LayerInfo &c = psd_.layerList[(size_t)ci];
      if (!visible(c)) continue;
      if (c.layerType == LAYER_TYPE_ADJUST) { st_.skippedAdjustments++; continue; }
      if (c.layerType == LAYER_TYPE_FOLDER) { st_.unsupportedClipBase++; continue; }
      Canvas cs;
      int cx = 0, cy = 0;
      if (!layerSurface(c, cs, cx, cy)) continue;
      drawLayer(c, cs, cx, cy, group, 0, 0, c.opacity / 255.f, c.fill_opacity / 255.f,
                (uint32_t)c.blendModeKey, clipMask.data());
    }
    psdfx_surface dst = canvas.surface(), src = group.surface();
    psdfx_composite(&dst, &src, 0, 0, (uint32_t)l.blendModeKey, opacity, nullptr, 0);
  }

  void renderGroup(int idx, Canvas &canvas) {
    LayerInfo &g = psd_.layerList[(size_t)idx];
    const float opacity = g.opacity / 255.f;
    const int key = g.sectionBlendKey ? g.sectionBlendKey : g.blendModeKey;
    std::vector<uint8_t> mask = documentMask(g);
    if (g.artboard.present) {
      renderArtboard(idx, canvas);
      return;
    }
    if (key == 'pass') {
      if (opacity >= 1.f && mask.empty()) {
        renderChildren(idx, canvas);
        return;
      }
      Canvas after = canvas;
      renderChildren(idx, after);
      psdfx_surface dst = canvas.surface(), src = after.surface();
      psdfx_lerp(&dst, &src, opacity, mask.empty() ? nullptr : mask.data(), canvas.width);
      return;
    }
    Canvas buf(canvas.width, canvas.height);
    renderChildren(idx, buf);
    psdfx_surface dst = canvas.surface(), src = buf.surface();
    psdfx_composite(&dst, &src, 0, 0, (uint32_t)key,
                    opacity * g.fill_opacity / 255.f,
                    mask.empty() ? nullptr : mask.data(), canvas.width);
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
