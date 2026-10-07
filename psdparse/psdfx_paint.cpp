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
  u = bend(u, a.midpoint);
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
  const double r = sampleStops(g->colors, g->color_count, t, s, [](const psdfx_color_stop &c) { return c.r / 255.0; });
  const double gg = sampleStops(g->colors, g->color_count, t, s, [](const psdfx_color_stop &c) { return c.g / 255.0; });
  const double b = sampleStops(g->colors, g->color_count, t, s, [](const psdfx_color_stop &c) { return c.b / 255.0; });
  const double a = g->alpha_count > 0
      ? sampleStops(g->alphas, g->alpha_count, t, s, [](const psdfx_alpha_stop &c) { return c.opacity; })
      : 1.0;
  rgba[0] = to8(r); rgba[1] = to8(gg); rgba[2] = to8(b); rgba[3] = to8(a);
}

extern "C" void psdfx_draw_gradient(psdfx_surface *dst, int dst_left, int dst_top,
                                    const psdfx_gradient *g, int style, double angle, double scale,
                                    int reverse, const double box[4], double offset_x, double offset_y) {
  if (!dst || !dst->pixels || !g) return;
  const double bw = box[2] - box[0], bh = box[3] - box[1];
  const double th = angle * kPi / 180.0;
  const double dx = std::cos(th), dy = -std::sin(th);
  double L = (std::fabs(bw * dx) + std::fabs(bh * dy)) * (scale > 0 ? scale : 1.0);
  if (L < 1e-6) L = 1e-6;
  const double cx = (box[0] + box[2]) * 0.5 + bw * offset_x / 100.0;
  const double cy = (box[1] + box[3]) * 0.5 + bh * offset_y / 100.0;
  // 色は 1024 段の表を作って引く
  const int N = 1024;
  std::vector<uint8_t> lut((size_t)N * 4);
  for (int i = 0; i < N; i++) psdfx_gradient_color(g, (double)i / (N - 1), &lut[(size_t)i * 4]);
  for (int y = 0; y < dst->height; y++) {
    uint8_t *row = dst->pixels + (size_t)y * dst->stride;
    const double py = dst_top + y + 0.5 - cy;
    for (int x = 0; x < dst->width; x++) {
      const double px = dst_left + x + 0.5 - cx;
      const double along = px * dx + py * dy;       // 方向成分
      const double across = -px * dy + py * dx;     // 直交成分
      double t;
      switch (style) {
      case PSDFX_GRADIENT_RADIAL:    t = std::hypot(px, py) / (L * 0.5); break;
      case PSDFX_GRADIENT_ANGLE: {
        double a = std::atan2(-py, px) - th;        // 反時計回り
        a = std::fmod(a, 2 * kPi);
        if (a < 0) a += 2 * kPi;
        t = 1.0 - a / (2 * kPi);
        break;
      }
      case PSDFX_GRADIENT_REFLECTED: t = std::fabs(along) / (L * 0.5); break;
      case PSDFX_GRADIENT_DIAMOND:   t = (std::fabs(along) + std::fabs(across)) / (L * 0.5); break;
      default:                       t = along / L + 0.5; break;
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
