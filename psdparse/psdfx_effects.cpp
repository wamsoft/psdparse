// psdfx — レイヤー効果。API は psdfx.h。
//
// 描画の順 (下から):
//   ドロップシャドウ → 光彩 (外側)          … 下地へ、それぞれのブレンドで
//   [レイヤ (塗りの不透明度) → パターン / グラデーション / カラーオーバーレイ →
//    サテン → 光彩 (内側) → シャドウ (内側) → ベベル] → 境界線
//                                           … まとめてレイヤのブレンドで下地へ
// 内側の効果は「形の中での被覆率」で重ね、最後にレイヤのアルファを掛ける
// (縁の半透明を二重に数えないため)。
//
// 影や光彩のぼかしの幅と Photoshop の「サイズ」の対応は、Photoshop の保存した
// 合成画像と照合して決めた近似 (kSigma*)。
#include "psdfx.h"
#include "psdfx_parallel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <vector>

namespace {

const double kPi = 3.14159265358979323846;
// 効果の「サイズ」をガウスの sigma へ換算する係数 (Photoshop の合成画像と照合して決めた)
const double kSigmaSatin = 0.42;   // Photoshop で測定
const double kSigmaBevel = 0.4;     // 形のぼかし (Photoshop で測定)

inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
inline uint8_t to8(float v) { return (uint8_t)(clamp01(v) * 255.f + 0.5f); }

struct Plane {
  int w = 0, h = 0;
  std::vector<float> v;
  Plane() = default;
  Plane(int w_, int h_, float init = 0.f) : w(w_), h(h_), v((size_t)w_ * h_, init) {}
  float &at(int x, int y) { return v[(size_t)y * w + x]; }
  float get(int x, int y) const {
    if (x < 0 || y < 0 || x >= w || y >= h) return 0.f;
    return v[(size_t)y * w + x];
  }
};

// 面の各行 / 各列に fn(線の先頭, 長さ, 間隔, 作業用) を並列に掛ける
template <class F>
void eachRow(Plane &p, F fn) {
  psdfx_internal::parallelFor(0, p.h, (long long)p.w * p.h, [&](int a, int b) {
    std::vector<float> tmp;
    for (int y = a; y < b; y++) fn(&p.v[(size_t)y * p.w], p.w, 1, tmp);
  });
}
template <class F>
void eachCol(Plane &p, F fn) {
  psdfx_internal::parallelFor(0, p.w, (long long)p.w * p.h, [&](int a, int b) {
    std::vector<float> tmp;
    for (int x = a; x < b; x++) fn(&p.v[(size_t)x], p.h, p.w, tmp);
  });
}

// 要素ごとに独立な処理 f(i) (i = 0..n-1) を並列に
template <class F>
void pfor(size_t n, F f) {
  const int blocks = (int)((n + 4095) / 4096);
  psdfx_internal::parallelFor(0, blocks, (long long)n, [&](int a, int b) {
    const size_t e = std::min(n, (size_t)b * 4096);
    for (size_t i = (size_t)a * 4096; i < e; i++) f(i);
  });
}

// --- ぼかし (箱ぼかし 3 回) ----------------------------------------------------
// 端の外は outside の値とみなす (形のアルファなら 0、形の反転なら 1)
void boxPass(float *v, int n, int step, int r, std::vector<float> &tmp, float outside) {
  if (r <= 0 || n <= 1) return;
  tmp.resize((size_t)n);
  const float inv = 1.f / (2 * r + 1);
  float acc = outside * r;
  for (int i = 0; i <= r; i++) acc += i < n ? v[(size_t)i * step] : outside;
  for (int i = 0; i < n; i++) {
    tmp[(size_t)i] = acc * inv;
    const int add = i + r + 1, sub = i - r;
    acc += add < n ? v[(size_t)add * step] : outside;
    acc -= sub >= 0 ? v[(size_t)sub * step] : outside;
  }
  for (int i = 0; i < n; i++) v[(size_t)i * step] = tmp[(size_t)i];
}

void blur(Plane &p, double sigma, float outside = 0.f) {
  if (!(sigma > 0.25)) return;
  const int n = 3;
  double wIdeal = std::sqrt(12.0 * sigma * sigma / n + 1.0);
  int wl = (int)std::floor(wIdeal);
  if (wl % 2 == 0) wl--;
  const int wu = wl + 2;
  const double mIdeal = (12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4.0);
  const int m = (int)std::round(mIdeal);
  for (int pass = 0; pass < n; pass++) {
    const int r = ((pass < m ? wl : wu) - 1) / 2;
    auto f = [&](float *v, int len, int step, std::vector<float> &tmp) { boxPass(v, len, step, r, tmp, outside); };
    eachRow(p, f);
    eachCol(p, f);
  }
}

// 輪郭: 被覆率を 256 段の表で写す (段の間は直線でつなぐ)。表が無ければそのまま
void applyContour(Plane &p, const uint8_t *lut) {
  if (!lut) return;
  pfor(p.v.size(), [&](size_t k) {
    float &v = p.v[k];
    const float x = std::min(1.f, std::max(0.f, v)) * 255.f;
    const int i = std::min(254, (int)x);
    const float f = x - i;
    v = (lut[i] + (lut[i + 1] - lut[i]) * f) / 255.f;
  });
}

// --- 距離変換 (Felzenszwalb の 2 乗ユークリッド距離) ---------------------------
void edt1d(const float *f, int n, float *d, std::vector<int> &v, std::vector<float> &z) {
  v.resize((size_t)n); z.resize((size_t)n + 1);
  int k = 0;
  v[0] = 0; z[0] = -std::numeric_limits<float>::infinity(); z[1] = std::numeric_limits<float>::infinity();
  for (int q = 1; q < n; q++) {
    float s;
    while (true) {
      s = ((f[q] + (float)q * q) - (f[v[(size_t)k]] + (float)v[(size_t)k] * v[(size_t)k])) /
          (2.f * q - 2.f * v[(size_t)k]);
      if (s <= z[(size_t)k] && k > 0) { k--; continue; }
      break;
    }
    k++;
    v[(size_t)k] = q; z[(size_t)k] = s; z[(size_t)k + 1] = std::numeric_limits<float>::infinity();
  }
  k = 0;
  for (int q = 0; q < n; q++) {
    while (z[(size_t)k + 1] < q) k++;
    const float dq = (float)(q - v[(size_t)k]);
    d[q] = dq * dq + f[v[(size_t)k]];
  }
}

// inside(x, y) が真の画素までの距離 (真の画素は 0)
Plane distanceTo(const Plane &a, bool wantInside, float threshold = 0.5f) {
  const float INF = 1e20f;
  Plane d(a.w, a.h);
  pfor(d.v.size(), [&](size_t i) {
    const bool in = a.v[i] >= threshold;
    d.v[i] = (in == wantInside) ? 0.f : INF;
  });
  const long long work = (long long)a.w * a.h * 4;
  psdfx_internal::parallelFor(0, a.w, work, [&](int xa, int xb) {
    std::vector<float> f((size_t)a.h), out((size_t)a.h);
    std::vector<int> vv; std::vector<float> zz;
    for (int x = xa; x < xb; x++) {
      for (int y = 0; y < a.h; y++) f[(size_t)y] = d.v[(size_t)y * a.w + x];
      edt1d(f.data(), a.h, out.data(), vv, zz);
      for (int y = 0; y < a.h; y++) d.v[(size_t)y * a.w + x] = out[(size_t)y];
    }
  });
  psdfx_internal::parallelFor(0, a.h, work, [&](int ya, int yb) {
    std::vector<float> out((size_t)a.w);
    std::vector<int> vv; std::vector<float> zz;
    for (int y = ya; y < yb; y++) {
      edt1d(&d.v[(size_t)y * a.w], a.w, out.data(), vv, zz);
      for (int x = 0; x < a.w; x++) d.v[(size_t)y * a.w + x] = std::sqrt(out[(size_t)x]);
    }
  });
  return d;
}

// 形を r ピクセル広げる (アンチエイリアス付き)
Plane dilate(const Plane &a, double r) {
  if (r <= 0.0) return a;
  Plane d = distanceTo(a, true);
  Plane o(a.w, a.h);
  pfor(o.v.size(), [&](size_t i) { o.v[i] = std::max(a.v[i], clamp01((float)(r + 1.0 - d.v[i]))); });
  return o;
}

Plane shifted(const Plane &a, double dx, double dy, float outside = 0.f) {
  Plane o(a.w, a.h);
  const int ix = (int)std::lround(dx), iy = (int)std::lround(dy);
  psdfx_internal::parallelFor(0, a.h, (long long)a.w * a.h, [&](int ya, int yb) {
    for (int y = ya; y < yb; y++)
      for (int x = 0; x < a.w; x++) {
        const int sx = x - ix, sy = y - iy;
        o.at(x, y) = (sx < 0 || sy < 0 || sx >= a.w || sy >= a.h) ? outside : a.get(sx, sy);
      }
  });
  return o;
}

// 半径 r (小数可) の箱で 1 列をならす。端数 f = r - floor(r) は外側の 1 画素ずつに重み f で掛ける
void boxPassFrac(float *v, int n, int step, double r, std::vector<float> &tmp, float outside) {
  if (!(r > 0.0) || n <= 1) return;
  const int k = (int)std::floor(r);
  const float f = (float)(r - k);
  tmp.resize((size_t)n);
  auto at = [&](int i) { return (i >= 0 && i < n) ? v[(size_t)i * step] : outside; };
  const float inv = 1.f / (2 * k + 1 + 2 * f);
  float acc = 0.f;
  for (int i = -k; i <= k; i++) acc += at(i);
  for (int i = 0; i < n; i++) {
    tmp[(size_t)i] = (acc + f * (at(i - k - 1) + at(i + k + 1))) * inv;
    acc += at(i + k + 1) - at(i - k);
  }
  for (int i = 0; i < n; i++) v[(size_t)i * step] = tmp[(size_t)i];
}

// 影 / 光彩の形: 広げ (spread) てから箱ぼかし 3 回。spread 0..1、size は全体の幅。
// 箱の半径は 0.423 x ぼかす幅 - 0.72 (小数の半径。大きさ 3〜30 で Photoshop と照合)
Plane spreadBox(const Plane &a, double spread, double size, float outside = 0.f) {
  spread = std::min(1.0, std::max(0.0, spread));
  Plane p = dilate(a, size * spread);
  const double r = std::max(0.0, size * (1.0 - spread) * 0.423 - 0.72);
  auto f = [&](float *v, int len, int step, std::vector<float> &tmp) { boxPassFrac(v, len, step, r, tmp, outside); };
  for (int pass = 0; pass < 3; pass++) {
    eachRow(p, f);
    eachCol(p, f);
  }
  return p;
}

// 塗りの元から色の面を作る (W x H、左上が文書の (ox, oy))
std::vector<uint8_t> paintSource(const psdfx_fill_source &src, int W, int H, int ox, int oy,
                                 const double layerBox[4], const double docBox[4]) {
  std::vector<uint8_t> px((size_t)W * H * 4, 255);
  psdfx_surface s{ px.data(), W, H, W * 4 };
  if (src.kind == PSDFX_FILL_GRADIENT && src.gradient.color_count > 0) {
    psdfx_draw_gradient(&s, ox, oy, &src.gradient, src.gradient_style, src.angle,
                        src.scale > 0 ? src.scale : 1.0, src.reverse,
                        src.align_with_layer ? layerBox : docBox, src.offset_x, src.offset_y);
  } else if (src.kind == PSDFX_FILL_PATTERN && src.pattern) {
    const double sc = src.scale > 0 ? src.scale : 1.0;
    // レイヤに整列するときの原点は効果の基準点 (あれば)、無ければレイヤの左上
    double oxp = (src.align_with_layer ? layerBox[0] : docBox[0]) + src.phase_x;
    double oyp = (src.align_with_layer ? layerBox[1] : docBox[1]) + src.phase_y;
    if (src.align_with_layer && src.has_reference_point) {
      oxp = docBox[0] + src.reference_x + src.phase_x;
      oyp = docBox[1] + src.reference_y + src.phase_y;
    }
    psdfx_draw_pattern(&s, ox, oy, src.pattern, sc, oxp, oyp);
  } else {
    pfor(px.size() / 4, [&](size_t k) {
      uint8_t *q = &px[k * 4];
      q[0] = src.color[2]; q[1] = src.color[1]; q[2] = src.color[0]; q[3] = 255;
    });
  }
  return px;
}

// 色の面 px に被覆率 cov を掛けた面を dst へ重ねる
void compositeCoverage(psdfx_surface *dst, std::vector<uint8_t> &px, const Plane &cov,
                       int dx, int dy, uint32_t blend, float opacity) {
  // 塗りが特別に効くモード (リニアバーンなど) では、効果の被覆率 x 不透明度で色を中立色へ
  // 寄せてから全面で合成する (アルファでは混ぜない。Photoshop で確認)
  float neutral = -1.f;
  switch (blend) {
  case PSDFX_KEY('l','b','r','n'): case PSDFX_KEY('i','d','i','v'): neutral = 1.f; break;
  case PSDFX_KEY('l','d','d','g'): case PSDFX_KEY('d','i','v',' '): case PSDFX_KEY('d','i','f','f'): neutral = 0.f; break;
  case PSDFX_KEY('v','L','i','t'): case PSDFX_KEY('l','L','i','t'): neutral = 0.5f; break;
  default: break;
  }
  if (neutral >= 0.f) {
    pfor(cov.v.size(), [&](size_t i) {
      const float k = px[i * 4 + 3] / 255.f * cov.v[i] * clamp01(opacity);
      for (int c = 0; c < 3; c++) px[i * 4 + c] = to8(neutral + (px[i * 4 + c] / 255.f - neutral) * k);
      px[i * 4 + 3] = k > 0.f ? 255 : 0;
    });
    psdfx_surface src{ px.data(), cov.w, cov.h, cov.w * 4 };
    psdfx_composite(dst, &src, dx, dy, blend, 1.f, nullptr, 0);
    return;
  }
  pfor(cov.v.size(), [&](size_t i) { px[i * 4 + 3] = to8(px[i * 4 + 3] / 255.f * cov.v[i]); });
  psdfx_surface src{ px.data(), cov.w, cov.h, cov.w * 4 };
  psdfx_composite(dst, &src, dx, dy, blend, opacity, nullptr, 0);
}

std::vector<uint8_t> solid(int W, int H, const uint8_t rgb[3]) {
  std::vector<uint8_t> px((size_t)W * H * 4);
  pfor((size_t)W * H, [&](size_t k) {
    uint8_t *q = &px[k * 4];
    q[0] = rgb[2]; q[1] = rgb[1]; q[2] = rgb[0]; q[3] = 255;
  });
  return px;
}

// 光彩の範囲: 被覆率 / 範囲 で立ち上げる (範囲 50% なら被覆率 0.5 で最大)。
// 候補の式を Photoshop の合成画像と照合して選んだ。
void applyRange(Plane &p, double range) {
  if (!(range > 0.0) || range >= 1.0) return;
  for (auto &v : p.v) v = clamp01((float)(v / range));
}

// 光彩の色: グラデーションなら被覆率 (形からの距離) を位置として引く
std::vector<uint8_t> glowColor(const psdfx_glow &g, const Plane &m, bool inner) {
  if (g.fill.kind != PSDFX_FILL_GRADIENT || g.fill.gradient.color_count == 0)
    return solid(m.w, m.h, g.fill.color);
  std::vector<uint8_t> px((size_t)m.w * m.h * 4);
  pfor(m.v.size(), [&](size_t i) {
    uint8_t c[4];
    // 外側: 形の際 (被覆 1) が位置 0。内側 (エッジから): 際が位置 1 側
    const double t = inner ? m.v[i] : 1.0 - m.v[i];
    psdfx_gradient_color(&g.fill.gradient, g.fill.reverse ? 1.0 - t : t, c);
    px[i * 4] = c[2]; px[i * 4 + 1] = c[1]; px[i * 4 + 2] = c[0]; px[i * 4 + 3] = c[3];
  });
  return px;
}

}  // anonymous namespace

extern "C" int psdfx_effects_margin(const psdfx_layer_effects *fx) {
  if (!fx) return 0;
  double m = 0;
  if (fx->drop_shadow.enabled)
    m = std::max(m, fx->drop_shadow.distance + fx->drop_shadow.size * 1.5);
  for (int i = 0; i < fx->more_drop_shadow_count && fx->more_drop_shadows; i++)
    if (fx->more_drop_shadows[i].enabled)
      m = std::max(m, fx->more_drop_shadows[i].distance + fx->more_drop_shadows[i].size * 1.5);
  for (int i = 0; i < fx->more_stroke_count && fx->more_strokes; i++)
    if (fx->more_strokes[i].enabled && fx->more_strokes[i].position != PSDFX_STROKE_INSIDE)
      m = std::max(m, fx->more_strokes[i].size);
  if (fx->outer_glow.enabled) m = std::max(m, fx->outer_glow.size * 1.5);
  if (fx->stroke.enabled && fx->stroke.position != PSDFX_STROKE_INSIDE) m = std::max(m, fx->stroke.size);
  if (fx->bevel.enabled) m = std::max(m, fx->bevel.size * 1.5);
  // 内側の効果も、ぼかしが形の外 (作業面の外は「形の外」とみなす) に届く分の余白が要る
  if (fx->inner_glow.enabled) m = std::max(m, fx->inner_glow.size * 1.5);
  if (fx->inner_shadow.enabled)
    m = std::max(m, fx->inner_shadow.distance + fx->inner_shadow.size * 1.5);
  for (int i = 0; i < fx->more_inner_shadow_count && fx->more_inner_shadows; i++)
    if (fx->more_inner_shadows[i].enabled)
      m = std::max(m, fx->more_inner_shadows[i].distance + fx->more_inner_shadows[i].size * 1.5);
  if (fx->satin.enabled) m = std::max(m, fx->satin.distance + fx->satin.size * 1.5);
  return (int)std::ceil(m) + 2;
}

extern "C" void psdfx_composite_with_effects(psdfx_surface *dst, const psdfx_surface *layer,
                                             int left, int top, uint32_t blend, float opacity,
                                             float fill_opacity, const psdfx_layer_effects *fx,
                                             const double doc_box[4],
                                             const uint8_t *shape, int shape_stride) {
  if (!dst || !layer || !layer->pixels) return;
  if (!fx) { psdfx_composite(dst, layer, left, top, blend, opacity * fill_opacity, nullptr, 0); return; }
  const int m = psdfx_effects_margin(fx);
  const int W = layer->width + 2 * m, H = layer->height + 2 * m;
  const int ox = left - m, oy = top - m;   // 作業面の左上 (dst 座標)
  if ((int64_t)W * H > (1LL << 26)) {       // 大きすぎるときは効果を省く
    psdfx_composite(dst, layer, left, top, blend, opacity * fill_opacity, nullptr, 0);
    return;
  }

  // 効果の形 A (shape があればそれ、無ければレイヤのアルファ) と、レイヤの色。
  // C はレイヤ自身のアルファ (形の中での透明度に使う)
  Plane A(W, H), C(W, H);
  std::vector<uint8_t> S((size_t)W * H * 4, 0);   // 内側の合成面 (形の中での被覆率で持つ)
  double lb[4] = { 1e30, 1e30, -1e30, -1e30 };      // 不透明な所の範囲 (dst 座標)
  std::mutex lbMutex;
  psdfx_internal::parallelFor(0, layer->height, (long long)layer->width * layer->height, [&](int ya, int yb) {
    double b[4] = { 1e30, 1e30, -1e30, -1e30 };
    for (int y = ya; y < yb; y++) {
      const uint8_t *row = layer->pixels + (size_t)y * layer->stride;
      for (int x = 0; x < layer->width; x++) {
        const float c = row[x * 4 + 3] / 255.f;
        const float a = shape ? shape[(size_t)y * shape_stride + x] / 255.f : c;
        A.at(x + m, y + m) = a;
        C.at(x + m, y + m) = c;
        uint8_t *s = &S[((size_t)(y + m) * W + x + m) * 4];
        s[0] = row[x * 4]; s[1] = row[x * 4 + 1]; s[2] = row[x * 4 + 2];
        if (a > 0.f) {
          b[0] = std::min(b[0], (double)(left + x)); b[1] = std::min(b[1], (double)(top + y));
          b[2] = std::max(b[2], (double)(left + x + 1)); b[3] = std::max(b[3], (double)(top + y + 1));
        }
      }
    }
    std::lock_guard<std::mutex> lk(lbMutex);
    lb[0] = std::min(lb[0], b[0]); lb[1] = std::min(lb[1], b[1]);
    lb[2] = std::max(lb[2], b[2]); lb[3] = std::max(lb[3], b[3]);
  });
  if (lb[0] > lb[2]) { lb[0] = left; lb[1] = top; lb[2] = left + layer->width; lb[3] = top + layer->height; }
  // 形の中: レイヤの色を (形の中での透明度 x 塗りの不透明度) で。形の外は空。
  // 内部効果をまとめるときは、塗りの不透明度は内側の効果のあとでまとめて掛ける。
  const float fillOnContent = fx->blend_interior_as_group ? 1.f : clamp01(fill_opacity);
  pfor(A.v.size(), [&](size_t i) {
    S[i * 4 + 3] = A.v[i] > 0.f ? to8(clamp01(C.v[i] / A.v[i]) * fillOnContent) : 0;
  });

  // --- 外側の効果 (下地へ) ---
  // 同じ種類の 2 つ目以降は一覧の下のものから描き、1 つ目を最後に (いちばん上に)
  auto eachBottomUp = [](const auto &first, const auto *more, int count, auto draw) {
    for (int i = count - 1; i >= 0 && more; i--) draw(more[i]);
    draw(first);
  };
  eachBottomUp(fx->drop_shadow, fx->more_drop_shadows, fx->more_drop_shadow_count, [&](const psdfx_shadow &ds) {
  if (ds.enabled && ds.opacity > 0) {
    const double th = ds.angle * kPi / 180.0;
    Plane sh = spreadBox(shifted(A, -std::cos(th) * ds.distance, std::sin(th) * ds.distance),
                         ds.spread, ds.size);
    applyContour(sh, ds.contour);
    if (ds.knocks_out)
      pfor(sh.v.size(), [&](size_t i) { sh.v[i] *= 1.f - A.v[i]; });
    std::vector<uint8_t> px = solid(W, H, ds.color);
    compositeCoverage(dst, px, sh, ox, oy, ds.blend, ds.opacity * opacity);
  }
  });
  const psdfx_glow &og = fx->outer_glow;
  if (og.enabled && og.opacity > 0) {
    Plane gl, raw;
    if (og.precise) {
      // 精細: 形からの距離 d で (大きさ + 1 - d) / (大きさ + 1) の直線 (スプレッドの分は先に広げる)
      const double sp = std::min(1.0, std::max(0.0, og.spread)) * og.size, rest = og.size - sp;
      Plane din = distanceTo(A, true);
      gl = Plane(W, H);
      for (size_t i = 0; i < gl.v.size(); i++) {
        const double d = std::max(0.0, din.v[i] - sp);
        gl.v[i] = std::max(A.v[i], clamp01((float)((rest + 1.0 - d) / (rest + 1.0))));
      }
      raw = gl;
      // 範囲は 0.5 / 範囲 倍 (50% で直線そのまま)
      const double k = og.range > 0 ? 0.5 / og.range : 1.0;
      for (auto &v : gl.v) v = clamp01((float)(v * k));
    } else {
      gl = spreadBox(A, og.spread, og.size);
      raw = gl;
      applyRange(gl, og.range);
    }
    applyContour(gl, og.contour);
    std::vector<uint8_t> px = glowColor(og, gl, false);
    if (og.fill.kind == PSDFX_FILL_GRADIENT && og.fill.gradient.color_count > 0) {
      // グラデーションの光彩: 色は (範囲・輪郭を掛けた) 値を位置として引き、不透明度は
      // ぼかしただけの被覆率の 8.33 倍で頭打ち (範囲・大きさによらない。Photoshop で測定)
      pfor(gl.v.size(), [&](size_t i) { gl.v[i] = std::min(1.f, raw.v[i] * 8.33f); });
    }
    // 光彩 (外側) はレイヤの形の下には出ない (半透明のレイヤや比較 (暗) などでも透けない。
    // Photoshop で確認)
    pfor(gl.v.size(), [&](size_t i) { gl.v[i] *= 1.f - A.v[i]; });
    compositeCoverage(dst, px, gl, ox, oy, og.blend, og.opacity * opacity);
  }

  // --- 内側の効果 (形の中で S へ) ---
  psdfx_surface Ss{ S.data(), W, H, W * 4 };
  auto overlay = [&](const psdfx_overlay &o) {
    if (!o.enabled || o.opacity <= 0) return;
    std::vector<uint8_t> px = paintSource(o.fill, W, H, ox, oy, lb, doc_box);
    Plane in(W, H);
    for (size_t i = 0; i < in.v.size(); i++) in.v[i] = A.v[i] > 0.f ? 1.f : 0.f;
    compositeCoverage(&Ss, px, in, 0, 0, o.blend, o.opacity);
  };
  overlay(fx->pattern_overlay);
  eachBottomUp(fx->gradient_overlay, fx->more_gradient_overlays, fx->more_gradient_overlay_count, overlay);
  eachBottomUp(fx->color_overlay, fx->more_color_overlays, fx->more_color_overlay_count, overlay);

  const psdfx_satin &sa = fx->satin;
  if (sa.enabled && sa.opacity > 0) {
    const double th = sa.angle * kPi / 180.0;
    // 形を角度の向きに距離だけ前後へずらした 2 枚 (ずれは整数の画素に丸める。Photoshop で測定)
    const double dx = std::round(std::cos(th) * sa.distance), dy = std::round(-std::sin(th) * sa.distance);
    Plane a1 = shifted(A, dx, dy), a2 = shifted(A, -dx, -dy);
    blur(a1, sa.size * kSigmaSatin); blur(a2, sa.size * kSigmaSatin);
    // 輪郭はぼかした 2 枚それぞれに掛けてから差を取る (Photoshop で測定)
    applyContour(a1, sa.contour); applyContour(a2, sa.contour);
    Plane cov(W, H);
    for (size_t i = 0; i < cov.v.size(); i++) {
      float v = std::fabs(a1.v[i] - a2.v[i]);
      cov.v[i] = sa.invert ? 1.f - v : v;
    }
    std::vector<uint8_t> px = solid(W, H, sa.color);
    compositeCoverage(&Ss, px, cov, 0, 0, sa.blend, sa.opacity);
  }

  const psdfx_glow &ig = fx->inner_glow;
  if (ig.enabled && ig.opacity > 0) {
    Plane inv(W, H);
    pfor(inv.v.size(), [&](size_t i) { inv.v[i] = 1.f - A.v[i]; });
    // 文書の外 (作業面の外) も「形の外」として扱うため、縁は 1 のまま広げる
    Plane gl;
    if (ig.precise) {
      // 精細: 形の外からの距離 d で (大きさ + 1 - d) / (大きさ + 1)、範囲は 0.5 / 範囲 倍
      const double sp = std::min(1.0, std::max(0.0, ig.spread)) * ig.size, rest = ig.size - sp;
      Plane dout = distanceTo(A, false);
      gl = Plane(W, H);
      const double k = ig.range > 0 ? 0.5 / ig.range : 1.0;
      for (size_t i = 0; i < gl.v.size(); i++) {
        const double d = std::max(0.0, dout.v[i] - sp);
        gl.v[i] = clamp01((float)(std::max((double)inv.v[i], (rest + 1.0 - d) / (rest + 1.0)) * k));
      }
    } else {
      gl = spreadBox(inv, ig.spread, ig.size, 1.f);
      applyRange(gl, ig.range);
    }
    applyContour(gl, ig.contour);
    if (ig.source_center) for (auto &v : gl.v) v = 1.f - v;
    std::vector<uint8_t> px = glowColor(ig, gl, true);
    compositeCoverage(&Ss, px, gl, 0, 0, ig.blend, ig.opacity);
  }

  eachBottomUp(fx->inner_shadow, fx->more_inner_shadows, fx->more_inner_shadow_count, [&](const psdfx_shadow &is) {
  if (is.enabled && is.opacity > 0) {
    Plane inv(W, H);
    pfor(inv.v.size(), [&](size_t i) { inv.v[i] = 1.f - A.v[i]; });
    const double th = is.angle * kPi / 180.0;
    Plane sh = spreadBox(shifted(inv, -std::cos(th) * is.distance, std::sin(th) * is.distance, 1.f),
                         is.spread, is.size, 1.f);
    applyContour(sh, is.contour);
    std::vector<uint8_t> px = solid(W, H, is.color);
    compositeCoverage(&Ss, px, sh, 0, 0, is.blend, is.opacity);
  }
  });

  // 中身の上に描き直す面 (シェイプの線): 内側の効果の上、ベベル / 境界線の下
  if (const psdfx_surface *top = fx->content_top) {
    if (top->pixels && top->width == layer->width && top->height == layer->height) {
      for (int y = 0; y < layer->height; y++) {
        const uint8_t *row = top->pixels + (size_t)y * top->stride;
        for (int x = 0; x < layer->width; x++) {
          const size_t i = (size_t)(y + m) * W + x + m;
          const float a = A.v[i];
          if (a <= 0.f || row[x * 4 + 3] == 0) continue;
          const float c = std::min(1.f, row[x * 4 + 3] / 255.f / a) * fillOnContent;
          uint8_t *q = &S[i * 4];
          const float sa = q[3] / 255.f, ra = c + sa * (1.f - c);
          for (int k = 0; k < 3; k++)
            q[k] = ra > 0.f ? to8((row[x * 4 + k] / 255.f * c + q[k] / 255.f * sa * (1.f - c)) / ra) : 0;
          q[3] = to8(ra);
        }
      }
    }
  }

  const psdfx_bevel &bv = fx->bevel;
  Plane bevelOuterHi, bevelOuterSh;
  if (bv.enabled && bv.size > 0) {
    // 浮き彫り / ピロー浮き彫りは大きさの半分ずつを形の内と外に (Photoshop で測定)
    const bool both = bv.style == PSDFX_BEVEL_EMBOSS || bv.style == PSDFX_BEVEL_PILLOW;
    const double w = both ? bv.size * 0.5 : bv.size;
    // 高さ: 滑らかは形をぼかしたもの、ジゼルは形の縁からの距離で直線に
    Plane hgt;
    if (bv.technique == PSDFX_BEVEL_SMOOTH) {
      hgt = A;
      blur(hgt, w * kSigmaBevel);
    } else {
      Plane dout = distanceTo(A, false), din = distanceTo(A, true);
      hgt = Plane(W, H);
      for (size_t i = 0; i < hgt.v.size(); i++) {
        const double sd = A.v[i] >= 0.5f ? dout.v[i] - 0.5 : 0.5 - din.v[i];   // 内側で正
        double h;
        if (bv.style == PSDFX_BEVEL_OUTER) h = 1.0 + sd / w;
        else if (both) h = 0.5 + sd / (2.0 * w);
        else h = sd / w;
        hgt.v[i] = clamp01((float)h);
      }
      if (bv.technique == PSDFX_BEVEL_CHISEL_SOFT) blur(hgt, 1.0);
    }
    const double az = bv.angle * kPi / 180.0, al = bv.altitude * kPi / 180.0;
    const double lx = std::cos(al) * std::cos(az), ly = -std::cos(al) * std::sin(az), lz = std::sin(al);
    const double k = (bv.up ? 1.0 : -1.0) * bv.depth * (bv.technique == PSDFX_BEVEL_SMOOTH ? w : 1.0);
    Plane hi(W, H), sh(W, H);
    psdfx_internal::parallelFor(0, H, (long long)W * H * 4, [&](int ya, int yb) {
    for (int y = ya; y < yb; y++)
      for (int x = 0; x < W; x++) {
        double gx = (hgt.get(x + 1, y) - hgt.get(x - 1, y)) * 0.5 * k;
        double gy = (hgt.get(x, y + 1) - hgt.get(x, y - 1)) * 0.5 * k;
        // ジゼルの斜面の傾きは 深さ x 0.46 (大きさにほぼよらない。Photoshop で測定)
        if (bv.technique != PSDFX_BEVEL_SMOOTH) { gx *= w * 0.46; gy *= w * 0.46; }
        // ピローは形の外側を逆向きに照らす
        if (bv.style == PSDFX_BEVEL_PILLOW && A.get(x, y) < 0.5f) { gx = -gx; gy = -gy; }
        const double nl = std::sqrt(gx * gx + gy * gy + 1.0);
        const double shade = (-gx * lx - gy * ly + lz) / nl;
        const double d = shade - lz;
        if (d > 0) hi.at(x, y) = (float)std::min(1.0, d / (1.0 - lz + 1e-6));
        else sh.at(x, y) = (float)std::min(1.0, -d / (lz + 1e-6));
      }
    });
    if (bv.soften > 0) { blur(hi, bv.soften * kSigmaBevel); blur(sh, bv.soften * kSigmaBevel); }
    if (bv.style != PSDFX_BEVEL_OUTER) {
      std::vector<uint8_t> ph = solid(W, H, bv.highlight_color), ps = solid(W, H, bv.shadow_color);
      Plane hiIn = hi, shIn = sh;
      pfor(A.v.size(), [&](size_t i) { if (A.v[i] <= 0.f) { hiIn.v[i] = 0; shIn.v[i] = 0; } });
      compositeCoverage(&Ss, ps, shIn, 0, 0, bv.shadow_blend, bv.shadow_opacity);
      compositeCoverage(&Ss, ph, hiIn, 0, 0, bv.highlight_blend, bv.highlight_opacity);
    }
    if (bv.style == PSDFX_BEVEL_OUTER || both) {
      bevelOuterHi = hi; bevelOuterSh = sh;
      for (size_t i = 0; i < A.v.size(); i++) {
        bevelOuterHi.v[i] *= 1.f - A.v[i]; bevelOuterSh.v[i] *= 1.f - A.v[i];
      }
    }
  }

  // 形の中の被覆率 → 実際のアルファ (内部効果をまとめるなら塗りの不透明度もここで)
  const float fillAfter = fx->blend_interior_as_group ? clamp01(fill_opacity) : 1.f;
  pfor(A.v.size(), [&](size_t i) { S[i * 4 + 3] = to8(S[i * 4 + 3] / 255.f * A.v[i] * fillAfter); });

  // --- 境界線 (形の上、外側は形の外へ) ---
  eachBottomUp(fx->stroke, fx->more_strokes, fx->more_stroke_count, [&](const psdfx_stroke &st) {
  if (st.enabled && st.opacity > 0 && st.size > 0) {
    Plane cov(W, H);
    const double sz = st.position == PSDFX_STROKE_CENTER ? st.size * 0.5 : st.size;
    // 形の各画素 q を「半径 線幅 + A(q)」の円とみなした和 (外側)、内側は 1 - A(q) で
    // 同じことをしたもの。被覆率 = clamp(線幅 - D)、D = min_q (|p - q| - A(q))。
    // アルファのしきい値ごとの距離変換の最小で求める。硬い縁では従来の
    // 「縁からの距離」と同じで、ぼかした縁ではアルファの付いた画素すべてが形に入る
    // (Photoshop で測定)
    auto softDistance = [&](bool outside) {
      Plane D(W, H, 1e9f);
      static const float kLevels[] = { 1.f / 255, 1.f / 16, 2.f / 16, 3.f / 16, 4.f / 16, 5.f / 16, 6.f / 16,
                                       7.f / 16, 8.f / 16, 9.f / 16, 10.f / 16, 11.f / 16, 12.f / 16,
                                       13.f / 16, 14.f / 16, 15.f / 16, 254.5f / 255 };
      // しきい値で分けた形が前のしきい値と同じなら距離変換もし直さない (硬い縁の形では
      // ほとんどのしきい値で同じになる。結果は変わらない)
      Plane d;
      float prevThr = -1.f;
      for (float t : kLevels) {
        const float thr = outside ? t : 1.f - t + 1e-6f;
        bool same = prevThr >= 0.f;
        if (same) {
          const float lo = std::min(thr, prevThr), hi = std::max(thr, prevThr);
          for (float v : A.v)
            if (v >= lo && v < hi) { same = false; break; }   // 2 つのしきい値の間の値があると形が変わる
        }
        if (!same) d = distanceTo(A, outside, thr);
        prevThr = thr;
        pfor(D.v.size(), [&](size_t i) { D.v[i] = std::min(D.v[i], d.v[i] - t); });
      }
      return D;
    };
    // 外側の分は、通常モードならレイヤの下に重ねる (形の中の半透明な所で中身の色が残る)
    Plane under;
    const bool normalStroke = st.blend == PSDFX_KEY('n','o','r','m');
    if (st.position != PSDFX_STROKE_INSIDE) {
      Plane D = softDistance(true);
      under = Plane(W, H);
      pfor(cov.v.size(), [&](size_t i) { under.v[i] = clamp01((float)(sz - D.v[i])); });
    }
    // 内側の分は中身の色を置き換え、アルファは中身のまま (source-atop。Photoshop で確認)
    Plane inner;
    if (st.position != PSDFX_STROKE_OUTSIDE) {
      Plane D = softDistance(false);
      inner = Plane(W, H);
      pfor(inner.v.size(), [&](size_t i) { if (A.v[i] > 0.f) inner.v[i] = clamp01((float)(sz - D.v[i])); });
    }
    // グラデーションの線は、形の範囲を外側の線幅 - 1 だけ広げた枠に描く
    // (Photoshop の合成画像と照合して確認)
    double sb[4] = { lb[0], lb[1], lb[2], lb[3] };
    const double outW = st.position == PSDFX_STROKE_OUTSIDE ? st.size
                      : st.position == PSDFX_STROKE_CENTER ? st.size * 0.5 : 0.0;
    const double grow = std::max(0.0, std::ceil(outW) - 1.0);
    sb[0] -= grow; sb[1] -= grow; sb[2] += grow; sb[3] += grow;
    std::vector<uint8_t> px = paintSource(st.fill, W, H, ox, oy, sb, doc_box);
    if (normalStroke) {
      // 通常モード: 線と中身を形 A で分け合う (Photoshop で確認)
      //   外側: 形の外の分 c x (1 - A) を足す (形の中は中身のまま。塗り 0% でも形の中に入らない)
      //   内側: 形の中の分 c で中身を置き換える (中身が透明でも形の中には線が出る)
      const float op = clamp01(st.opacity);
      pfor(A.v.size(), [&](size_t i) {
        uint8_t *q = &S[i * 4];
        float sa = q[3] / 255.f;
        float pr[3] = { q[0] / 255.f * sa, q[1] / 255.f * sa, q[2] / 255.f * sa };
        const float a = A.v[i];
        const float pa = px[i * 4 + 3] / 255.f;
        const float st3[3] = { px[i * 4] / 255.f, px[i * 4 + 1] / 255.f, px[i * 4 + 2] / 255.f };
        if (!inner.v.empty() && inner.v[i] > 0.f && a > 0.f) {
          const float c = inner.v[i] * op * pa;
          for (int k = 0; k < 3; k++) pr[k] = pr[k] * (1.f - c) + st3[k] * c * a;
          sa = sa * (1.f - c) + c * a;
        }
        if (!under.v.empty() && under.v[i] > 0.f) {
          const float c = under.v[i] * op * pa * (1.f - a);
          for (int k = 0; k < 3; k++) pr[k] += st3[k] * c;
          sa = std::min(1.f, sa + c);
        }
        q[3] = to8(sa);
        for (int k = 0; k < 3; k++) q[k] = sa > 0.f ? to8(pr[k] / sa) : 0;
      });
    } else {
      if (!under.v.empty())
        for (size_t i = 0; i < cov.v.size(); i++) cov.v[i] = A.v[i] > 0.f ? under.v[i] * (1.f - A.v[i]) : under.v[i];
      if (!inner.v.empty())
        for (size_t i = 0; i < cov.v.size(); i++) cov.v[i] = std::max(cov.v[i], A.v[i] * inner.v[i]);
      compositeCoverage(&Ss, px, cov, 0, 0, st.blend, st.opacity);
    }
  }
  });
  psdfx_composite(dst, &Ss, ox, oy, blend, opacity, nullptr, 0);

  if (!bevelOuterHi.v.empty()) {
    // 形の外のベベルは、レイヤ (外側の境界線を含む) を重ねたあとの画像へ直接重ねる
    // (白地に覆い焼きカラーなら何も変わらず、外側の境界線には陰が付く。Photoshop で確認)
    std::vector<uint8_t> ph = solid(W, H, bv.highlight_color), ps = solid(W, H, bv.shadow_color);
    compositeCoverage(dst, ps, bevelOuterSh, ox, oy, bv.shadow_blend, bv.shadow_opacity * opacity);
    compositeCoverage(dst, ph, bevelOuterHi, ox, oy, bv.highlight_blend, bv.highlight_opacity * opacity);
  }
}
