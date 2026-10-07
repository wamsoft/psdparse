// psdfx — 塗り (グラデーション / パターン)。
//
// グラデーションの幾何: 角度 θ の方向 d = (cos θ, -sin θ) (画像は y 下向き) で、
// 基準の矩形を d へ投影した長さ L を 100% とする (比率 scale を掛ける)。中心 c は
// 矩形の中心を (offset_x, offset_y) % ずらした点。
//   線形:   t = ((p - c)・d) / L + 0.5
//   円形:   t = |p - c| / (L / 2)
//   円錐形: t = (θ 方向からの角度) / 360°
//   反射形: t = |(p - c)・d| / (L / 2)
//   菱形:   t = (|(p - c)・d| + |(p - c)・d⊥|) / (L / 2)
// 色: 分岐点の間を、中間点で区間を曲げてから補間する。滑らかさ s は線形補間と
// 分岐点の色を通る Catmull-Rom 曲線を s で混ぜて近似する。
#include "psdfx.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

const double kPi = 3.14159265358979323846;

inline double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// 中間点 m で区間内の位置 u を曲げる (m = 0.5 で直線)
inline double bend(double u, double m) {
  m = std::min(0.999, std::max(0.001, m));
  return u <= m ? 0.5 * u / m : 0.5 + 0.5 * (u - m) / (1.0 - m);
}

template <class Stop, class Get>
double sampleStops(const Stop *s, int n, double t, double smooth, Get get) {
  if (n <= 0) return 0;
  if (t <= s[0].location || n == 1) return get(s[0]);
  if (t >= s[n - 1].location) return get(s[n - 1]);
  int i = 0;
  while (i + 1 < n && s[i + 1].location < t) i++;
  const Stop &a = s[i], &b = s[i + 1];
  const double span = b.location - a.location;
  double u = span > 1e-9 ? (t - a.location) / span : 0;
  u = bend(u, b.midpoint);   // 中間点は区間の終わりの分岐点が持つ (Photoshop の保存の形)
  const double va = get(a), vb = get(b);
  const double lin = va + (vb - va) * u;
  if (smooth <= 0) return lin;
  // Catmull-Rom (端の接線は弦の傾きの半分)
  const double vp = i > 0 ? get(s[i - 1]) : va - (vb - va);
  const double vn = i + 2 < n ? get(s[i + 2]) : vb + (vb - va);
  const double m0 = (i > 0) ? (vb - vp) * 0.5 : (vb - va) * 0.5;
  const double m1 = (i + 2 < n) ? (vn - va) * 0.5 : (vb - va) * 0.5;
  const double u2 = u * u, u3 = u2 * u;
  const double cr = (2 * u3 - 3 * u2 + 1) * va + (u3 - 2 * u2 + u) * m0 +
                    (-2 * u3 + 3 * u2) * vb + (u3 - u2) * m1;
  return lin + (cr - lin) * smooth;
}

inline uint8_t to8(double v) { return (uint8_t)(clamp01(v) * 255.0 + 0.5); }

}  // anonymous namespace

extern "C" void psdfx_gradient_color(const psdfx_gradient *g, double t, uint8_t rgba[4]) {
  t = clamp01(t);
  const double s = clamp01(g->smoothness);
  double r, gg, b;
  if (g->interpolation == PSDFX_GRADIENT_LINEAR_LIGHT) {
    auto lin = [](uint8_t v) { const double x = v / 255.0; return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4); };
    auto enc = [](double v) { v = clamp01(v); return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055; };
    r = enc(sampleStops(g->colors, g->color_count, t, s, [&](const psdfx_color_stop &c) { return lin(c.r); }));
    gg = enc(sampleStops(g->colors, g->color_count, t, s, [&](const psdfx_color_stop &c) { return lin(c.g); }));
    b = enc(sampleStops(g->colors, g->color_count, t, s, [&](const psdfx_color_stop &c) { return lin(c.b); }));
  } else {
    r = sampleStops(g->colors, g->color_count, t, s, [](const psdfx_color_stop &c) { return c.r / 255.0; });
    gg = sampleStops(g->colors, g->color_count, t, s, [](const psdfx_color_stop &c) { return c.g / 255.0; });
    b = sampleStops(g->colors, g->color_count, t, s, [](const psdfx_color_stop &c) { return c.b / 255.0; });
  }
  const double a = g->alpha_count > 0
      ? sampleStops(g->alphas, g->alpha_count, t, s, [](const psdfx_alpha_stop &c) { return c.opacity; })
      : 1.0;
  rgba[0] = to8(r); rgba[1] = to8(gg); rgba[2] = to8(b); rgba[3] = to8(a);
}

extern "C" void psdfx_draw_gradient(psdfx_surface *dst, int dst_left, int dst_top,
                                    const psdfx_gradient *g, int style, double angle, double scale,
                                    int reverse, const double box[4], double offset_x, double offset_y) {
  if (!dst || !dst->pixels || !g) return;
  // 長さ (Photoshop の合成画像と照合して決めた):
  //   線形 / 反射: 基準の矩形の中心を通り、角度の向きに矩形を横切る弦の長さ
  //   円形 / 菱形 / 角度: 矩形の幅・高さを角度で混ぜた楕円ノルム
  // 線形 / 反射の端点は整数の画素位置へ切り捨てる (小さな矩形では角度が変わる。
  // 4x4 で 30 度の反射は (4, 0) で終わり、45 度として描かれる)。
  const double bw = std::max(1.0, box[2] - box[0]), bh = std::max(1.0, box[3] - box[1]);
  const double sc = scale > 0 ? scale : 1.0;
  double th = angle * kPi / 180.0;
  const double cx = box[0] + bw * 0.5 + bw * offset_x / 100.0;
  const double cy = box[1] + bh * 0.5 + bh * offset_y / 100.0;
  auto chordOf = [&](double a) {
    return std::max(1.0, std::min(bw / std::max(std::fabs(std::cos(a)), 1e-6),
                                  bh / std::max(std::fabs(std::sin(a)), 1e-6)));
  };
  double L = std::max(1.0, std::hypot(std::cos(th) * bw, std::sin(th) * bh)) * sc;
  double ox = cx, oy = cy;   // 線形 / 反射の t = 0.5 / 0 の位置
  if (style == PSDFX_GRADIENT_LINEAR || style == PSDFX_GRADIENT_REFLECTED) {
    const double half = chordOf(th) * sc * 0.5;
    const double hx = std::cos(th) * half, hy = -std::sin(th) * half;
    const double ex = std::floor(cx + hx + 1e-4), ey = std::floor(cy + hy + 1e-4);
    double sx = cx, sy = cy;
    if (style == PSDFX_GRADIENT_LINEAR) { sx = std::floor(cx - hx + 1e-4); sy = std::floor(cy - hy + 1e-4); }
    const double vx = ex - sx, vy = ey - sy, len = std::hypot(vx, vy);
    if (len >= 0.5) {
      th = std::atan2(-vy, vx);
      L = style == PSDFX_GRADIENT_REFLECTED ? 2.0 * len : len;
      if (style == PSDFX_GRADIENT_LINEAR) { ox = (sx + ex) * 0.5; oy = (sy + ey) * 0.5; }
    } else {
      L = chordOf(th) * sc;
    }
  }
  const double dx = std::cos(th), dy = -std::sin(th);
  // 円形 / 菱形の長さも線形と同じ弦 (矩形の中心を通り角度の向きに横切る長さ。Photoshop で確認)
  const double Lc = chordOf(angle * kPi / 180.0) * sc;
  // 線形 / 反射は画素の左上の角で値を取る (中心ではない)
  const double corner = 0.5 * (std::cos(th) - std::sin(th));
  // 色は 1024 段の表を作って引く
  const int N = 1024;
  std::vector<uint8_t> lut((size_t)N * 4);
  for (int i = 0; i < N; i++) psdfx_gradient_color(g, (double)i / (N - 1), &lut[(size_t)i * 4]);
  for (int y = 0; y < dst->height; y++) {
    uint8_t *row = dst->pixels + (size_t)y * dst->stride;
    // 円形 / 菱形 / 角度は画素の左上の角で値を取る (中心の画素がちょうど 0。Photoshop で確認)
    const double py = dst_top + y - cy;
    for (int x = 0; x < dst->width; x++) {
      const double px = dst_left + x - cx;
      const double along = px * dx + py * dy;       // 方向成分
      const double across = -px * dy + py * dx;     // 直交成分
      // 線形 / 反射は端点をそろえた基準点からの方向成分
      const double alongL = (dst_left + x + 0.5 - ox) * dx + (dst_top + y + 0.5 - oy) * dy - corner;
      double t;
      switch (style) {
      case PSDFX_GRADIENT_RADIAL:    t = std::hypot(px, py) / (Lc * 0.5); break;
      case PSDFX_GRADIENT_ANGLE: {
        double a = std::atan2(-py, px) - th;        // 反時計回り
        a = std::fmod(a, 2 * kPi);
        if (a < 0) a += 2 * kPi;
        t = 1.0 - a / (2 * kPi);
        break;
      }
      case PSDFX_GRADIENT_REFLECTED: t = std::fabs(alongL) / (L * 0.5); break;
      case PSDFX_GRADIENT_DIAMOND:   t = (std::fabs(along) + std::fabs(across)) / (Lc * 0.5); break;
      default:                       t = alongL / L + 0.5; break;
      }
      t = clamp01(t);
      if (reverse) t = 1.0 - t;
      const uint8_t *c = &lut[(size_t)(t * (N - 1) + 0.5) * 4];
      row[x * 4 + 0] = c[2]; row[x * 4 + 1] = c[1]; row[x * 4 + 2] = c[0]; row[x * 4 + 3] = c[3];
    }
  }
}

extern "C" void psdfx_draw_pattern(psdfx_surface *dst, int dst_left, int dst_top,
                                   const psdfx_surface *tile, double scale,
                                   double origin_x, double origin_y) {
  if (!dst || !dst->pixels || !tile || !tile->pixels || tile->width <= 0 || tile->height <= 0) return;
  if (!(scale > 0)) scale = 1.0;
  const double tw = tile->width * scale, th = tile->height * scale;
  if (std::fabs(scale - 1.0) > 1e-9) {
    // 拡大縮小したパターンは画素の左上の角の位置で、タイルの画素の間を直線補間する
    // (繰り返しの継ぎ目も含めて。Photoshop の合成画像と照合して確認)
    const int TW = tile->width, TH = tile->height;
    auto wrap = [](int v, int n) { v %= n; return v < 0 ? v + n : v; };
    for (int y = 0; y < dst->height; y++) {
      uint8_t *row = dst->pixels + (size_t)y * dst->stride;
      const double v = (dst_top + y - origin_y) / scale;
      const double vf = std::floor(v);
      const float ty = (float)(v - vf);
      const int y0 = wrap((int)vf, TH), y1 = wrap((int)vf + 1, TH);
      const uint8_t *r0 = tile->pixels + (size_t)y0 * tile->stride, *r1 = tile->pixels + (size_t)y1 * tile->stride;
      for (int x = 0; x < dst->width; x++) {
        const double u = (dst_left + x - origin_x) / scale;
        const double uf = std::floor(u);
        const float tx = (float)(u - uf);
        const int x0 = wrap((int)uf, TW), x1 = wrap((int)uf + 1, TW);
        for (int c = 0; c < 4; c++) {
          const float a = r0[x0 * 4 + c] * (1 - tx) + r0[x1 * 4 + c] * tx;
          const float b = r1[x0 * 4 + c] * (1 - tx) + r1[x1 * 4 + c] * tx;
          row[x * 4 + c] = (uint8_t)std::min(255.f, a * (1 - ty) + b * ty + 0.5f);
        }
      }
    }
    return;
  }
  for (int y = 0; y < dst->height; y++) {
    uint8_t *row = dst->pixels + (size_t)y * dst->stride;
    double fy = std::fmod((dst_top + y + 0.5 - origin_y), th);
    if (fy < 0) fy += th;
    const int sy = std::min(tile->height - 1, (int)(fy / scale));
    const uint8_t *trow = tile->pixels + (size_t)sy * tile->stride;
    for (int x = 0; x < dst->width; x++) {
      double fx = std::fmod((dst_left + x + 0.5 - origin_x), tw);
      if (fx < 0) fx += tw;
      const int sx = std::min(tile->width - 1, (int)(fx / scale));
      const uint8_t *c = trow + (size_t)sx * 4;
      row[x * 4 + 0] = c[0]; row[x * 4 + 1] = c[1]; row[x * 4 + 2] = c[2]; row[x * 4 + 3] = c[3];
    }
  }
}
