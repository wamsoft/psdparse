// psdfx — 調整 (レベル補正 / トーンカーブ / 色相・彩度 など) の画素処理。
//
// 式は公開仕様の各パラメータの意味に沿い、細部 (ガンマの立ち上がり、明るさ・
// コントラストの新方式の曲線、露光量の伝達特性など) は Photoshop が保存した
// 合成画像と照合して合わせたもの。透明な画素 (アルファ 0) は触らない。
#include "psdfx.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

inline float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
inline uint8_t to8(float v) { return (uint8_t)(clamp01(v) * 255.f + 0.5f); }
inline float luma(float r, float g, float b) { return 0.299f * r + 0.587f * g + 0.114f * b; }

// 画素ごとに RGB (0..1) を変換する
template <class F>
void mapRGB(psdfx_surface *s, F f) {
  if (!s || !s->pixels) return;
  for (int y = 0; y < s->height; y++) {
    uint8_t *p = s->pixels + (size_t)y * s->stride;
    for (int x = 0; x < s->width; x++, p += 4) {
      if (!p[3]) continue;
      float c[3] = { p[2] / 255.f, p[1] / 255.f, p[0] / 255.f };
      f(c, x, y);
      p[2] = to8(c[0]); p[1] = to8(c[1]); p[0] = to8(c[2]);
    }
  }
}

void rgbToHsl(const float c[3], float &h, float &s, float &l) {
  const float mx = std::max(c[0], std::max(c[1], c[2])), mn = std::min(c[0], std::min(c[1], c[2]));
  l = (mx + mn) * 0.5f;
  const float d = mx - mn;
  if (d < 1e-7f) { h = 0; s = 0; return; }
  s = l > 0.5f ? d / (2.f - mx - mn) : d / (mx + mn);
  if (mx == c[0]) h = (c[1] - c[2]) / d + (c[1] < c[2] ? 6.f : 0.f);
  else if (mx == c[1]) h = (c[2] - c[0]) / d + 2.f;
  else h = (c[0] - c[1]) / d + 4.f;
  h /= 6.f;
}

float hue2rgb(float p, float q, float t) {
  if (t < 0) t += 1;
  if (t > 1) t -= 1;
  if (t < 1.f / 6) return p + (q - p) * 6 * t;
  if (t < 0.5f) return q;
  if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
  return p;
}

void hslToRgb(float h, float s, float l, float c[3]) {
  if (s <= 0) { c[0] = c[1] = c[2] = l; return; }
  const float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
  c[0] = hue2rgb(p, q, h + 1.f / 3); c[1] = hue2rgb(p, q, h); c[2] = hue2rgb(p, q, h - 1.f / 3);
}

// 4x4 の組織的ディザ (-0.5..0.5)
float bayer4(int x, int y) {
  static const float M[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
  return (M[(y & 3) * 4 + (x & 3)] + 0.5f) / 16.f - 0.5f;
}

}  // namespace

extern "C" void psdfx_apply_lut(psdfx_surface *s, const uint8_t lut_r[256], const uint8_t lut_g[256],
                                const uint8_t lut_b[256]) {
  if (!s || !s->pixels) return;
  for (int y = 0; y < s->height; y++) {
    uint8_t *p = s->pixels + (size_t)y * s->stride;
    for (int x = 0; x < s->width; x++, p += 4) {
      if (!p[3]) continue;
      p[2] = lut_r[p[2]]; p[1] = lut_g[p[1]]; p[0] = lut_b[p[0]];
    }
  }
}

extern "C" void psdfx_levels_lut(double in_black, double in_white, double out_black, double out_white,
                                 double gamma, uint8_t lut[256]) {
  // 入力範囲で引き伸ばした値を整数の段へ丸めてからガンマを掛ける (8bit の
  // Photoshop は段単位で計算する)。ガンマ > 1 のときは黒付近の傾きが有限に
  // なるよう、べき乗と傾き 0.93 * 2^gamma の直線のなめらかな最小を取る。
  const double range = std::max(1e-6, in_white - in_black);
  const double g = std::max(0.01, gamma);
  for (int i = 0; i < 256; i++) {
    double u = std::floor(std::min(1.0, std::max(0.0, (i - in_black) / range)) * 255.0 + 0.5 + 1e-3) / 255.0;
    double t;
    if (g > 1.0) {
      const double pw = std::pow(u, 1.0 / g), line = 0.93 * std::pow(2.0, g) * u;
      t = (pw <= 1e-6 || line <= 1e-6) ? std::min(pw, line)
                                         : std::pow(std::pow(pw, -10.0) + std::pow(line, -10.0), -0.1);
    } else {
      t = std::pow(u, 1.0 / g);
    }
    const double v = out_black + t * (out_white - out_black);
    lut[i] = (uint8_t)std::min(255.0, std::max(0.0, std::floor(v + 0.5)));
  }
}

extern "C" void psdfx_curve_lut(const double *points, int count, uint8_t lut[256]) {
  // 点を通る自然 3 次スプライン (点の間で行き過ぎることもある)。最初 / 最後の
  // 点の外は一定。
  std::vector<std::pair<double, double>> pts;
  for (int i = 0; i < count; i++) pts.push_back({ points[i * 2], points[i * 2 + 1] });
  std::sort(pts.begin(), pts.end());
  pts.erase(std::unique(pts.begin(), pts.end(),
                        [](const std::pair<double, double> &a, const std::pair<double, double> &b) {
                          return std::fabs(a.first - b.first) < 1e-6;
                        }), pts.end());
  if (pts.size() < 2) { for (int i = 0; i < 256; i++) lut[i] = (uint8_t)i; return; }
  const size_t n = pts.size();
  std::vector<double> m2(n, 0.0), c(n, 0.0), d(n, 0.0);
  for (size_t i = 1; i + 1 < n; i++) {
    const double h0 = pts[i].first - pts[i - 1].first, h1 = pts[i + 1].first - pts[i].first;
    const double a = h0 / 6.0, b = (h0 + h1) / 3.0, cc = h1 / 6.0;
    const double r = (pts[i + 1].second - pts[i].second) / h1 - (pts[i].second - pts[i - 1].second) / h0;
    const double den = b - a * c[i - 1];
    c[i] = cc / den;
    d[i] = (r - a * d[i - 1]) / den;
  }
  for (size_t i = n - 1; i-- > 1;) m2[i] = d[i] - c[i] * m2[i + 1];
  for (int k = 0; k < 256; k++) {
    const double x = k;
    double y;
    if (x <= pts[0].first) y = pts[0].second;
    else if (x >= pts[n - 1].first) y = pts[n - 1].second;
    else {
      size_t i = 0;
      while (i + 2 < n && x > pts[i + 1].first) i++;
      const double x0 = pts[i].first, x1 = pts[i + 1].first, h = x1 - x0;
      const double A = (x1 - x) / h, B = (x - x0) / h;
      y = A * pts[i].second + B * pts[i + 1].second +
          ((A * A * A - A) * m2[i] + (B * B * B - B) * m2[i + 1]) * h * h / 6.0;
    }
    lut[k] = (uint8_t)std::min(255.0, std::max(0.0, std::floor(y + 0.5)));
  }
}

extern "C" void psdfx_brightness_contrast_lut(double brightness, double contrast, int legacy, uint8_t lut[256]) {
  for (int i = 0; i < 256; i++) {
    double v = i / 255.0;
    if (legacy) {
      // 旧方式: 中間の灰色を軸にコントラストを掛け、明るさを足す
      const double cc = std::min(99.0, std::max(-100.0, contrast));
      const double k = cc >= 0 ? 1.0 / (1.0 - cc / 100.0) : 1.0 + cc / 100.0;
      v = (v - 0.5) * k + 0.5 + brightness / 255.0;
    } else {
      // 新方式: 明るさは原点からの傾き 1.375^(b/50) の直線を白で 1 に収める曲線、
      // コントラストは 0.5 を軸にした 3 次エルミートの S 字 (端の傾き 1 - c/128、
      // 中央の傾き 1 + c/128)。明るさ → コントラストの順。
      if (brightness != 0) {
        const double s = std::pow(1.375, brightness / 50.0);
        const double p = brightness >= 0 ? std::max(2.0, 4.5 - 0.013 * brightness) : 5.0 - 0.072 * brightness;
        v = s * v + (1.0 - s) * std::pow(v, p);
      }
      if (contrast != 0) {
        const double k = contrast / 128.0, e = 1.0 - k, m = 1.0 + k;
        auto half = [&](double t) {
          return e * 0.5 * (t * t * t - 2 * t * t + t) + (-2 * t * t * t + 3 * t * t) * 0.5 + m * 0.5 * (t * t * t - t * t);
        };
        v = std::min(1.0, std::max(0.0, v));
        v = v <= 0.5 ? half(v / 0.5) : 1.0 - half((1.0 - v) / 0.5);
      }
    }
    lut[i] = to8((float)v);
  }
}

extern "C" void psdfx_exposure_lut(double exposure, double offset, double gamma, uint8_t lut[256]) {
  // 線形光で (v * 2^露光量 + オフセット)^(1/ガンマ)。RGB 文書の線形化は sRGB の
  // 曲線ではなく純粋な 2.2 乗 (照合で確認)。
  const double m = std::pow(2.0, exposure), g = std::max(0.01, gamma);
  for (int i = 0; i < 256; i++) {
    const double lin = std::pow(std::max(0.0, std::pow(i / 255.0, 2.2) * m + offset), 1.0 / g);
    lut[i] = to8((float)std::pow(std::max(0.0, lin), 1.0 / 2.2));
  }
}

extern "C" void psdfx_posterize_lut(int levels, uint8_t lut[256]) {
  const int n = std::min(255, std::max(2, levels));
  for (int i = 0; i < 256; i++) {
    const int bin = std::min(n - 1, i * n / 256);
    lut[i] = (uint8_t)(bin * 255 / (n - 1));
  }
}

extern "C" void psdfx_threshold(psdfx_surface *s, int level) {
  mapRGB(s, [&](float c[3], int, int) {
    const float v = std::floor(luma(c[0], c[1], c[2]) * 255.f + 0.5f) >= level ? 1.f : 0.f;
    c[0] = c[1] = c[2] = v;
  });
}

extern "C" void psdfx_hue_saturation(psdfx_surface *s, double hue, double saturation, double lightness,
                                     int colorize, const psdfx_hue_range *ranges, int range_count) {
  // 範囲ごとの調整は色相の表 (360 段) にまとめる。範囲の重みは 4 つの境界で
  // 作る台形 (始まりから測るので赤系の 0 度またぎも扱える)。
  std::vector<float> th(361, 0.f), ts(361, 0.f), tl(361, 0.f);
  bool anyRange = false;
  for (int r = 0; r < range_count && ranges && !colorize; r++) {
    const psdfx_hue_range &g = ranges[r];
    if (g.hue == 0 && g.saturation == 0 && g.lightness == 0) continue;
    anyRange = true;
    auto rel = [&](double x) { double v = std::fmod(x - g.bounds[0], 360.0); return v < 0 ? v + 360.0 : v; };
    const double b = rel(g.bounds[1]), c = std::max(rel(g.bounds[2]), b), d = std::max(rel(g.bounds[3]), c);
    for (int k = 0; k <= 360; k++) {
      const double h = rel(k);
      double w;
      if (h <= b) w = b <= 0 ? 1.0 : h / b;
      else if (h <= c) w = 1.0;
      else if (h <= d) w = d <= c ? 1.0 : (d - h) / (d - c);
      else w = 0.0;
      th[(size_t)k] += (float)(w * g.hue);
      ts[(size_t)k] += (float)(w * g.saturation / 100.0);
      tl[(size_t)k] += (float)(w * g.lightness / 100.0);
    }
  }
  // Photoshop で単体の調整を掛けた画像と照合して決めた式:
  //   彩度: 上げる側は s / (1 - 量)、下げる側は s x (1 + 量)
  //   範囲ごとの明度: + は各チャンネルを最大のチャンネルへ、- は最小へ寄せる
  //   全体の明度: + は白へ、- は黒へ寄せる
  //   色彩の統一: 明度を先に掛けた明るさで、指定の色相・彩度の色にする
  mapRGB(s, [&](float c[3], int, int) {
    float dh = 0, ds = 0, dl = 0;
    float h, sa, l;
    rgbToHsl(c, h, sa, l);
    if (anyRange) {
      const float x = h * 360.f;
      const int i0 = std::min(359, (int)x);
      const float f = x - i0;
      dh = th[(size_t)i0] * (1 - f) + th[(size_t)i0 + 1] * f;
      ds = ts[(size_t)i0] * (1 - f) + ts[(size_t)i0 + 1] * f;
      dl = tl[(size_t)i0] * (1 - f) + tl[(size_t)i0 + 1] * f;
    }
    const float lv = std::min(1.f, std::max(-1.f, (float)(lightness / 100.0)));
    if (colorize) {
      float hh = std::fmod((float)hue, 360.f); if (hh < 0) hh += 360.f;
      float L = l;
      if (lv > 0) L = L + (1 - L) * lv; else if (lv < 0) L = L * (1 + lv);
      hslToRgb(hh / 360.f, clamp01((float)(saturation / 100.0)), L, c);
      return;
    }
    const float hu = (float)hue + dh;
    const float sv = std::min(1.f, std::max(-1.f, (float)(saturation / 100.0) + ds));
    if (hu != 0 || sv != 0) {
      h = std::fmod(h + hu / 360.f, 1.f); if (h < 0) h += 1.f;
      if (sv > 0) sa = sv >= 1.f ? (sa > 0 ? 1.f : 0.f) : clamp01(sa / (1.f - sv));
      else sa = clamp01(sa * (1.f + sv));
      hslToRgb(h, sa, l, c);
    }
    if (dl != 0) {
      const float d = std::min(1.f, std::max(-1.f, dl));
      const float mx = std::max(c[0], std::max(c[1], c[2])), mn = std::min(c[0], std::min(c[1], c[2]));
      for (int i = 0; i < 3; i++) c[i] = d > 0 ? c[i] + (mx - c[i]) * d : c[i] + (c[i] - mn) * d;
    }
    for (int i = 0; i < 3; i++) {
      if (lv > 0) c[i] = c[i] + (1 - c[i]) * lv;
      else if (lv < 0) c[i] = c[i] * (1 + lv);
    }
  });
}

extern "C" void psdfx_vibrance(psdfx_surface *s, double vibrance, double saturation) {
  const float v = (float)(vibrance / 100.0), sa = (float)(saturation / 100.0);
  mapRGB(s, [&](float c[3], int, int) {
    float h, ss, l;
    rgbToHsl(c, h, ss, l);
    const float boost = v * (1 - ss);   // 鮮やかでない色ほど大きく動く
    hslToRgb(h, clamp01(ss * (1 + sa) + boost * std::max(ss, 0.1f)), l, c);
  });
}

extern "C" void psdfx_color_balance(psdfx_surface *s, const double shadows[3], const double midtones[3],
                                    const double highlights[3], int preserve_luminosity) {
  mapRGB(s, [&](float c[3], int, int) {
    const float l = luma(c[0], c[1], c[2]);
    const float ws = clamp01(1 - l * 2), wh = clamp01(l * 2 - 1), wm = 1 - ws - wh;
    float o[3];
    for (int i = 0; i < 3; i++)
      o[i] = clamp01(c[i] + (float)(shadows[i] * ws + midtones[i] * wm + highlights[i] * wh) / 100.f * 0.5f);
    if (preserve_luminosity) {
      const float l1 = std::max(1e-6f, luma(o[0], o[1], o[2]));
      for (int i = 0; i < 3; i++) o[i] = clamp01(o[i] * l / l1);
    }
    c[0] = o[0]; c[1] = o[1]; c[2] = o[2];
  });
}

extern "C" void psdfx_selective_color(psdfx_surface *s, const double adjustments[9][4], int relative) {
  mapRGB(s, [&](float c[3], int, int) {
    const float mx = std::max(c[0], std::max(c[1], c[2])), mn = std::min(c[0], std::min(c[1], c[2]));
    const float md = c[0] + c[1] + c[2] - mx - mn;
    auto top = [&](int i) { return c[i] >= mx ? mx - md : 0.f; };
    auto bottom = [&](int i) { return c[i] <= mn ? md - mn : 0.f; };
    // レッド系 / イエロー系 / グリーン系 / シアン系 / ブルー系 / マゼンタ系 / ホワイト系 /
    // ニュートラル系 / ブラック系への属しかた
    const float w[9] = { top(0), bottom(2), top(1), bottom(0), top(2), bottom(1),
                         std::max(0.f, (mn - 0.5f) * 2), clamp01(1 - std::fabs(mx - 0.5f) - std::fabs(mn - 0.5f)),
                         std::max(0.f, (0.5f - mx) * 2) };
    float delta[3] = { 0, 0, 0 };
    for (int r = 0; r < 9; r++) {
      if (w[r] <= 0) continue;
      for (int i = 0; i < 3; i++) {
        const float a = (float)(adjustments[r][i] + adjustments[r][3]) / 100.f;
        delta[i] += (relative ? a * (1 - c[i]) : a) * w[r];
      }
    }
    for (int i = 0; i < 3; i++) c[i] = clamp01(c[i] - delta[i]);
  });
}

extern "C" void psdfx_channel_mixer(psdfx_surface *s, const double matrix[3][4], int monochrome) {
  mapRGB(s, [&](float c[3], int, int) {
    float o[3];
    for (int r = 0; r < 3; r++)
      o[r] = clamp01((float)(matrix[r][0] * c[0] + matrix[r][1] * c[1] + matrix[r][2] * c[2] + matrix[r][3]));
    if (monochrome) o[1] = o[2] = o[0];
    c[0] = o[0]; c[1] = o[1]; c[2] = o[2];
  });
}

extern "C" void psdfx_photo_filter(psdfx_surface *s, const uint8_t color[3], double density,
                                   int preserve_luminosity) {
  const float k[3] = { color[0] / 255.f, color[1] / 255.f, color[2] / 255.f };
  const float d = (float)std::min(1.0, std::max(0.0, density));
  mapRGB(s, [&](float c[3], int, int) {
    float o[3];
    for (int i = 0; i < 3; i++) o[i] = c[i] * (1 - d) + c[i] * k[i] * d;
    if (preserve_luminosity) {
      const float l0 = luma(c[0], c[1], c[2]), l1 = std::max(1e-6f, luma(o[0], o[1], o[2]));
      for (int i = 0; i < 3; i++) o[i] = clamp01(o[i] * l0 / l1);
    }
    c[0] = o[0]; c[1] = o[1]; c[2] = o[2];
  });
}

extern "C" void psdfx_black_white(psdfx_surface *s, const double weights[6], const uint8_t *tint) {
  // 灰色 = 最小 + (中間 - 最小) x 二次色の重み + (最大 - 中間) x 原色の重み。
  // 重みはレッド系 / イエロー系 / グリーン系 / シアン系 / ブルー系 / マゼンタ系 (%)。
  mapRGB(s, [&](float c[3], int, int) {
    const float r = c[0], g = c[1], b = c[2];
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), md = r + g + b - mx - mn;
    const int primary = (r >= g && r >= b) ? 0 : (g >= b ? 2 : 4);
    const int secondary = (b <= r && b <= g) ? 1 : (r <= g ? 3 : 5);
    const float v = clamp01(mn + (md - mn) * (float)weights[secondary] / 100.f + (mx - md) * (float)weights[primary] / 100.f);
    for (int i = 0; i < 3; i++)
      c[i] = tint ? clamp01(v * (tint[i] / 255.f) * 2.f) * 0.5f + v * 0.5f : v;
  });
}

extern "C" void psdfx_gradient_map(psdfx_surface *s, const psdfx_gradient *g, int reverse, int dither,
                                   int origin_x, int origin_y) {
  if (!g) return;
  std::vector<uint8_t> lut(1024 * 4);
  for (int i = 0; i < 1024; i++) psdfx_gradient_color(g, i / 1023.0, &lut[(size_t)i * 4]);
  mapRGB(s, [&](float c[3], int x, int y) {
    float t = luma(c[0], c[1], c[2]);
    if (reverse) t = 1 - t;
    const uint8_t *o = &lut[(size_t)(clamp01(t) * 1023.f + 0.5f) * 4];
    const float dd = dither ? bayer4(origin_x + x, origin_y + y) / 255.f : 0.f;
    for (int i = 0; i < 3; i++) c[i] = o[i] / 255.f + dd;
  });
}

extern "C" void psdfx_apply_adjusted(psdfx_surface *dst, const psdfx_surface *adjusted, uint32_t blend_key,
                                     float opacity, const uint8_t *mask, int mask_stride) {
  // 調整済みの色を、元の色の上にブレンドモード・不透明度・マスクで重ねる。
  // アルファは元のまま (調整レイヤは下地の透明度を変えない)。
  if (!dst || !adjusted || !dst->pixels || !adjusted->pixels) return;
  const int w = std::min(dst->width, adjusted->width), h = std::min(dst->height, adjusted->height);
  std::vector<uint8_t> src((size_t)w * h * 4);
  for (int y = 0; y < h; y++) {
    const uint8_t *a = adjusted->pixels + (size_t)y * adjusted->stride;
    for (int x = 0; x < w; x++) {
      uint8_t *p = &src[((size_t)y * w + x) * 4];
      p[0] = a[x * 4]; p[1] = a[x * 4 + 1]; p[2] = a[x * 4 + 2];
      p[3] = mask ? mask[(size_t)y * mask_stride + x] : 255;
    }
  }
  psdfx_surface s = { src.data(), w, h, w * 4 };
  psdfx_composite_atop(dst, &s, 0, 0, blend_key, opacity);
}
