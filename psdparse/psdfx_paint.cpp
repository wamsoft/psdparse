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

inline double srgbToLinear(double x) { return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4); }
inline double linearToSrgb(double v) { v = clamp01(v); return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055; }

// sRGB (0..255) → Oklab
void toOklab(uint8_t r8, uint8_t g8, uint8_t b8, double o[3]) {
  const double r = srgbToLinear(r8 / 255.0), g = srgbToLinear(g8 / 255.0), b = srgbToLinear(b8 / 255.0);
  const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  o[0] = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
  o[1] = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
  o[2] = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
}

// Oklab → sRGB (0..1、範囲外は切る)
void fromOklab(const double o[3], double &r, double &g, double &b) {
  const double l = o[0] + 0.3963377774 * o[1] + 0.2158037573 * o[2];
  const double m = o[0] - 0.1055613458 * o[1] - 0.0638541728 * o[2];
  const double s = o[0] - 0.0894841775 * o[1] - 1.2914855480 * o[2];
  const double l3 = l * l * l, m3 = m * m * m, s3 = s * s * s;
  r = linearToSrgb(4.0767416621 * l3 - 3.3077115913 * m3 + 0.2309699292 * s3);
  g = linearToSrgb(-1.2684380046 * l3 + 2.6097574011 * m3 - 0.3413193965 * s3);
  b = linearToSrgb(-0.0041960863 * l3 - 0.7034186147 * m3 + 1.7076147010 * s3);
}

// 知覚的 / 滑らか用の分岐点 (Oklab の値とアルファ)
struct Knot { double location, midpoint; double v[3]; };

// 滑らか (Smoo): 中間点も「隣の 2 点の平均を通る点」として加え、点を 3 次の
// エルミート曲線でつなぐ。点での傾きは両隣の区間の傾き (値 / 位置) の平均、両端は
// 区間の傾きの 0.7 倍 (滑らかさの値は使わない。Photoshop で測定)
double sampleSmooth(const std::vector<Knot> &k, double t, int ch) {
  const int n = (int)k.size();
  if (n == 0) return 0;
  if (t <= k[0].location || n == 1) return k[0].v[ch];
  if (t >= k[n - 1].location) return k[n - 1].v[ch];
  int i = 0;
  while (i + 1 < n && k[i + 1].location < t) i++;
  auto chord = [&](int j) {
    const double d = k[j + 1].location - k[j].location;
    return d > 1e-9 ? (k[j + 1].v[ch] - k[j].v[ch]) / d : 0.0;
  };
  auto slope = [&](int j) {
    if (j == 0) return chord(0) * 0.7;
    if (j == n - 1) return chord(n - 2) * 0.7;
    return (chord(j - 1) + chord(j)) * 0.5;
  };
  const double span = k[i + 1].location - k[i].location;
  if (span <= 1e-9) return k[i + 1].v[ch];
  const double u = (t - k[i].location) / span, u2 = u * u, u3 = u2 * u;
  return (2 * u3 - 3 * u2 + 1) * k[i].v[ch] + (u3 - 2 * u2 + u) * slope(i) * span +
         (-2 * u3 + 3 * u2) * k[i + 1].v[ch] + (u3 - u2) * slope(i + 1) * span;
}

std::vector<Knot> withMidKnots(const std::vector<Knot> &k) {
  std::vector<Knot> out;
  for (size_t i = 0; i < k.size(); i++) {
    if (i > 0) {
      Knot m;
      m.location = k[i - 1].location + (k[i].location - k[i - 1].location) * k[i].midpoint;
      m.midpoint = 0.5;
      for (int c = 0; c < 3; c++) m.v[c] = (k[i - 1].v[c] + k[i].v[c]) * 0.5;
      out.push_back(m);
    }
    Knot a = k[i];
    a.midpoint = 0.5;
    out.push_back(a);
  }
  return out;
}

}  // anonymous namespace

extern "C" void psdfx_gradient_color(const psdfx_gradient *g, double t, uint8_t rgba[4]) {
  t = clamp01(t);
  const double s = clamp01(g->smoothness);
  double r, gg, b;
  const bool smoo = g->interpolation == PSDFX_GRADIENT_SMOOTH;
  if ((g->interpolation == PSDFX_GRADIENT_PERCEPTUAL || smoo) && g->color_count > 0) {
    std::vector<Knot> k((size_t)g->color_count);
    for (int i = 0; i < g->color_count; i++) {
      const psdfx_color_stop &c = g->colors[i];
      k[i].location = c.location; k[i].midpoint = c.midpoint;
      toOklab(c.r, c.g, c.b, k[i].v);
    }
    if (smoo) k = withMidKnots(k);
    double o[3];
    for (int ch = 0; ch < 3; ch++)
      o[ch] = smoo ? sampleSmooth(k, t, ch)
                   : sampleStops(k.data(), (int)k.size(), t, s, [ch](const Knot &q) { return q.v[ch]; });
    fromOklab(o, r, gg, b);
  } else if (g->interpolation == PSDFX_GRADIENT_LINEAR_LIGHT) {
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
  double a = 1.0;
  if (g->alpha_count > 0 && smoo) {
    std::vector<Knot> k((size_t)g->alpha_count);
    for (int i = 0; i < g->alpha_count; i++) {
      k[i].location = g->alphas[i].location; k[i].midpoint = g->alphas[i].midpoint;
      k[i].v[0] = g->alphas[i].opacity; k[i].v[1] = k[i].v[2] = 0;
    }
    k = withMidKnots(k);
    a = sampleSmooth(k, t, 0);
  } else if (g->alpha_count > 0) {
    a = sampleStops(g->alphas, g->alpha_count, t, s, [](const psdfx_alpha_stop &c) { return c.opacity; });
  }
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
