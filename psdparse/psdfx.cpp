// psdfx — Photoshop 互換の合成とレイヤー効果 (実装)。API は psdfx.h。
//
// ブレンドは W3C "Compositing and Blending" の source-over を土台にする:
//   αo = αs + αb (1 - αs)
//   Co = (αs (1-αb) Cs + αs αb B(Cb, Cs) + (1-αs) αb Cb) / αo
// B の式は Photoshop に合わせたもの (ソフトライト、比較 (暗) / (明) のカラーなど)。
#include "psdfx.h"
#include "psdfx_parallel.h"
#include "psdfx_simd.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

inline float dodge(float b, float s) {
  if (s >= 1.f) return 1.f;
  return std::min(1.f, b / (1.f - s));
}
inline float burn(float b, float s) {
  if (s <= 0.f) return 0.f;
  return 1.f - std::min(1.f, (1.f - b) / s);
}

// --- 分離可能なブレンド (チャンネルごと) -------------------------------------
inline float blendChannel(uint32_t key, float b, float s) {
  switch (key) {
  case PSDFX_KEY('m','u','l',' '): return b * s;
  case PSDFX_KEY('s','c','r','n'): return b + s - b * s;
  case PSDFX_KEY('d','a','r','k'): return std::min(b, s);
  case PSDFX_KEY('l','i','t','e'): return std::max(b, s);
  case PSDFX_KEY('o','v','e','r'):   // オーバーレイ = 基本色と合成色を入れ替えたハードライト
    return b <= 0.5f ? 2.f * s * b : 1.f - 2.f * (1.f - s) * (1.f - b);
  case PSDFX_KEY('h','L','i','t'):
    return s <= 0.5f ? 2.f * s * b : 1.f - 2.f * (1.f - s) * (1.f - b);
  case PSDFX_KEY('s','L','i','t'):   // Photoshop のソフトライト
    return s <= 0.5f ? 2.f * b * s + b * b * (1.f - 2.f * s)
                     : 2.f * b * (1.f - s) + std::sqrt(b) * (2.f * s - 1.f);
  // 覆い焼き / 焼き込みカラー (W3C と同じく下地の判定が先)
  case PSDFX_KEY('d','i','v',' '): return b <= 0.f ? 0.f : dodge(b, s);
  case PSDFX_KEY('i','d','i','v'): return b >= 1.f ? 1.f : burn(b, s);
  case PSDFX_KEY('l','b','r','n'): return std::max(0.f, b + s - 1.f);
  case PSDFX_KEY('l','d','d','g'): return std::min(1.f, b + s);
  // ビビッドライト: 中の焼き込み / 覆い焼きは合成色の判定が先 (Photoshop の挙動。
  // 合成色が純白 / 純黒なら下地によらず白 / 黒)
  case PSDFX_KEY('v','L','i','t'):
    return s <= 0.5f ? burn(b, 2.f * s) : dodge(b, 2.f * (s - 0.5f));
  case PSDFX_KEY('l','L','i','t'): return clamp01(b + 2.f * s - 1.f);
  case PSDFX_KEY('p','L','i','t'):
    return s <= 0.5f ? std::min(b, 2.f * s) : std::max(b, 2.f * s - 1.f);
  case PSDFX_KEY('h','M','i','x'): return (b + s >= 1.f) ? 1.f : 0.f;
  case PSDFX_KEY('d','i','f','f'): return std::fabs(b - s);
  case PSDFX_KEY('s','m','u','d'): return b + s - 2.f * b * s;   // 除外
  case PSDFX_KEY('f','s','u','b'): return std::max(0.f, b - s);  // 減算
  case PSDFX_KEY('f','d','i','v'):                              // 除算
    if (s <= 0.f) return b <= 0.f ? 0.f : 1.f;
    return std::min(1.f, b / s);
  default: return s;   // 通常 / ディザ合成 / 未対応
  }
}

// --- 分離できないブレンド (W3C の色相 / 彩度 / カラー / 輝度) ------------------
inline float lum(const float c[3]) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }

void clipColor(float c[3]) {
  float l = lum(c);
  float n = std::min(c[0], std::min(c[1], c[2]));
  float x = std::max(c[0], std::max(c[1], c[2]));
  for (int i = 0; i < 3; i++) {
    if (n < 0.f && l - n > 1e-6f) c[i] = l + (c[i] - l) * l / (l - n);
    if (x > 1.f && x - l > 1e-6f) c[i] = l + (c[i] - l) * (1.f - l) / (x - l);
  }
}

void setLum(const float c[3], float l, float out[3]) {
  float d = l - lum(c);
  for (int i = 0; i < 3; i++) out[i] = c[i] + d;
  clipColor(out);
}

inline float sat(const float c[3]) {
  return std::max(c[0], std::max(c[1], c[2])) - std::min(c[0], std::min(c[1], c[2]));
}

void setSat(const float c[3], float s, float out[3]) {
  int mx = 0, mn = 0;
  for (int i = 1; i < 3; i++) { if (c[i] > c[mx]) mx = i; if (c[i] < c[mn]) mn = i; }
  if (mx == mn) { out[0] = out[1] = out[2] = 0.f; return; }
  int md = 3 - mx - mn;
  float range = c[mx] - c[mn];
  out[md] = (c[md] - c[mn]) * s / range;
  out[mx] = s;
  out[mn] = 0.f;
}

bool nonSeparable(uint32_t key, const float b[3], const float s[3], float out[3]) {
  float t[3];
  switch (key) {
  case PSDFX_KEY('h','u','e',' '): setSat(s, sat(b), t); setLum(t, lum(b), out); return true;
  case PSDFX_KEY('s','a','t',' '): setSat(b, sat(s), t); setLum(t, lum(b), out); return true;
  case PSDFX_KEY('c','o','l','r'): setLum(s, lum(b), out); return true;
  case PSDFX_KEY('l','u','m',' '): setLum(b, lum(s), out); return true;
  case PSDFX_KEY('d','k','C','l'):   // カラー比較 (暗)
    for (int i = 0; i < 3; i++) out[i] = (lum(s) < lum(b)) ? s[i] : b[i];
    return true;
  case PSDFX_KEY('l','g','C','l'):   // カラー比較 (明)
    for (int i = 0; i < 3; i++) out[i] = (lum(s) > lum(b)) ? s[i] : b[i];
    return true;
  default:
    return false;
  }
}

// 1 画素を重ねる。b / s は 0..1 の RGB、ab / as はアルファ。結果を b, ab へ。
inline void blendPixel(uint32_t key, float b[3], float &ab, const float s[3], float as) {
  if (as <= 0.f) return;
  float B[3];
  if (!nonSeparable(key, b, s, B))
    for (int i = 0; i < 3; i++) B[i] = blendChannel(key, b[i], s[i]);
  const float ao = as + ab * (1.f - as);
  for (int i = 0; i < 3; i++) {
    float co = as * (1.f - ab) * s[i] + as * ab * B[i] + (1.f - as) * ab * b[i];
    b[i] = ao > 0.f ? co / ao : 0.f;
  }
  ab = ao;
}

inline uint8_t to8(float v) { return (uint8_t)(clamp01(v) * 255.f + 0.5f); }

}  // anonymous namespace

extern "C" {

int psdfx_blend_supported(uint32_t key) {
  switch (key) {
  case PSDFX_KEY('n','o','r','m'): case PSDFX_KEY('p','a','s','s'): case PSDFX_KEY('d','i','s','s'):
  case PSDFX_KEY('m','u','l',' '): case PSDFX_KEY('s','c','r','n'): case PSDFX_KEY('d','a','r','k'):
  case PSDFX_KEY('l','i','t','e'): case PSDFX_KEY('o','v','e','r'): case PSDFX_KEY('h','L','i','t'):
  case PSDFX_KEY('s','L','i','t'): case PSDFX_KEY('d','i','v',' '): case PSDFX_KEY('i','d','i','v'):
  case PSDFX_KEY('l','b','r','n'): case PSDFX_KEY('l','d','d','g'): case PSDFX_KEY('v','L','i','t'):
  case PSDFX_KEY('l','L','i','t'): case PSDFX_KEY('p','L','i','t'): case PSDFX_KEY('h','M','i','x'):
  case PSDFX_KEY('d','i','f','f'): case PSDFX_KEY('s','m','u','d'): case PSDFX_KEY('f','s','u','b'):
  case PSDFX_KEY('f','d','i','v'): case PSDFX_KEY('h','u','e',' '): case PSDFX_KEY('s','a','t',' '):
  case PSDFX_KEY('c','o','l','r'): case PSDFX_KEY('l','u','m',' '): case PSDFX_KEY('d','k','C','l'):
  case PSDFX_KEY('l','g','C','l'):
    return 1;
  default:
    return 0;
  }
}

// 塗りの不透明度が「特別」に効く描画モード (Photoshop の special eight)。塗りは
// アルファを下げず、上の色をそのモードで効かない色 (中立色) へ寄せてから合成する。
// ハードミックスは (下 - 塗り x (1 - 上)) / (1 - 塗り) を 0..1 に。Photoshop で確認
static bool specialFill(uint32_t key, float &neutral) {
  switch (key) {
  case PSDFX_KEY('l','b','r','n'): case PSDFX_KEY('i','d','i','v'): neutral = 1.f; return true;
  case PSDFX_KEY('l','d','d','g'): case PSDFX_KEY('d','i','v',' '):
  case PSDFX_KEY('d','i','f','f'): neutral = 0.f; return true;
  case PSDFX_KEY('v','L','i','t'): case PSDFX_KEY('l','L','i','t'): neutral = 0.5f; return true;
  case PSDFX_KEY('h','M','i','x'): neutral = -1.f; return true;
  default: return false;
  }
}

static void compositeImpl(psdfx_surface *dst, const psdfx_surface *src, int dx, int dy,
                          uint32_t key, float opacity, const uint8_t *mask, int mstride,
                          bool atop, float fill = 1.f) {
  if (!dst || !src || !dst->pixels || !src->pixels || opacity <= 0.f) return;
  float neutral = 0.f;
  const bool special = fill < 1.f && specialFill(key, neutral);
  if (!special) { opacity *= clamp01(fill); if (opacity <= 0.f) return; }
  if (key == PSDFX_KEY('p','a','s','s') || key == PSDFX_KEY('d','i','s','s'))
    key = PSDFX_KEY('n','o','r','m');
  const int x0 = std::max(0, dx), y0 = std::max(0, dy);
  const int x1 = std::min(dst->width, dx + src->width), y1 = std::min(dst->height, dy + src->height);
  const float op = clamp01(opacity);
  const bool normal = key == PSDFX_KEY('n','o','r','m');
  const uint32_t key0 = key;
  // SIMD 版が使えるとき (塗りの特別な扱いと、透明な下地へのハードミックスは除く)。
  // 行の先頭から 8 画素単位で SIMD、残りをこの下のスカラー処理で (結果は同じ)
  const bool simd = !special && key != PSDFX_KEY('h','M','i','x') && psdfx_internal::simdAvailable();
  psdfx_internal::parallelFor(y0, y1, (long long)(y1 - y0) * (x1 - x0), [&](int ya, int yb) {
  uint32_t key = key0;   // ハードミックス + 塗りでは途中で通常に切り替える (スレッドごと)
  for (int y = ya; y < yb; y++) {
    uint8_t *d = dst->pixels + (size_t)y * dst->stride + (size_t)x0 * 4;
    const uint8_t *s = src->pixels + (size_t)(y - dy) * src->stride + (size_t)(x0 - dx) * 4;
    const uint8_t *m = mask ? mask + (size_t)(y - dy) * mstride + (x0 - dx) : nullptr;
    int xs = x0;
    if (simd) {
      const int done = psdfx_internal::compositeRowSimd(d, s, m, x1 - x0, key, op, atop);
      xs += done; d += (size_t)done * 4; s += (size_t)done * 4;
      if (m) m += done;
    }
    for (int x = xs; x < x1; x++, d += 4, s += 4) {
      float as = s[3] / 255.f * op;
      if (m) as *= (*m++) / 255.f;
      if (as <= 0.f) continue;
      float ab = d[3] / 255.f;
      if (atop && ab <= 0.f) continue;
      float b[3] = { d[2] / 255.f, d[1] / 255.f, d[0] / 255.f };
      float sc[3] = { s[2] / 255.f, s[1] / 255.f, s[0] / 255.f };
      if (special) {
        if (neutral < 0.f) {
          // ハードミックス + 塗り: 下の色ごとに決まる値を、塗り 100% の通常の合成で重ねる
          for (int i = 0; i < 3; i++)
            sc[i] = fill <= 0.f ? b[i] : clamp01((b[i] - fill * (1.f - sc[i])) / std::max(1e-6f, 1.f - fill));
          key = PSDFX_KEY('n','o','r','m');
        } else {
          for (int i = 0; i < 3; i++) sc[i] = neutral + (sc[i] - neutral) * fill;
        }
      }
      if (atop) {
        // source-atop: αo = αb、Co = αs B(Cb, Cs) + (1 - αs) Cb
        float B[3];
        if (!nonSeparable(key, b, sc, B))
          for (int i = 0; i < 3; i++) B[i] = blendChannel(key, b[i], sc[i]);
        for (int i = 0; i < 3; i++) b[i] = as * B[i] + (1.f - as) * b[i];
      } else if (normal && ab <= 0.f) {
        b[0] = sc[0]; b[1] = sc[1]; b[2] = sc[2]; ab = as;
      } else if (key == PSDFX_KEY('h','M','i','x') && ab < 0.999f) {
        // ハードミックスを透明な下地に重ねるとき: しきい値の判定には不透明度を掛けた上の
        // 色を使う (cb + cs x 不透明度 >= 1。不透明な下地では通常どおり。照合で確認)
        const float ao = as + ab - as * ab;
        for (int i = 0; i < 3; i++) {
          const float B = b[i] + sc[i] * op >= 1.f - 1e-6f ? 1.f : 0.f;
          const float co = (1.f - as) * ab * b[i] + (1.f - ab) * as * sc[i] + as * ab * B;
          b[i] = ao > 0.f ? co / ao : 0.f;
        }
        ab = ao;
      } else {
        blendPixel(key, b, ab, sc, as);
      }
      d[2] = to8(b[0]); d[1] = to8(b[1]); d[0] = to8(b[2]); d[3] = to8(ab);
    }
  }
  });
}

void psdfx_composite(psdfx_surface *dst, const psdfx_surface *src, int dx, int dy,
                     uint32_t blend_key, float opacity, const uint8_t *mask, int mask_stride) {
  compositeImpl(dst, src, dx, dy, blend_key, opacity, mask, mask_stride, false);
}

void psdfx_composite_layer(psdfx_surface *dst, const psdfx_surface *src, int dx, int dy,
                           uint32_t blend_key, float opacity, float fill,
                           const uint8_t *mask, int mask_stride) {
  compositeImpl(dst, src, dx, dy, blend_key, opacity, mask, mask_stride, false, clamp01(fill));
}

void psdfx_composite_layer_atop(psdfx_surface *dst, const psdfx_surface *src, int dx, int dy,
                                uint32_t blend_key, float opacity, float fill,
                                const uint8_t *mask, int mask_stride) {
  compositeImpl(dst, src, dx, dy, blend_key, opacity, mask, mask_stride, true, clamp01(fill));
}

void psdfx_composite_atop(psdfx_surface *dst, const psdfx_surface *src, int dx, int dy,
                          uint32_t blend_key, float opacity) {
  compositeImpl(dst, src, dx, dy, blend_key, opacity, nullptr, 0, true);
}

void psdfx_lerp(psdfx_surface *dst, const psdfx_surface *src, float t,
                const uint8_t *mask, int mask_stride) {
  if (!dst || !src || dst->width != src->width || dst->height != src->height) return;
  t = clamp01(t);
  psdfx_internal::parallelFor(0, dst->height, (long long)dst->width * dst->height, [&](int ya, int yb) {
  for (int y = ya; y < yb; y++) {
    uint8_t *d = dst->pixels + (size_t)y * dst->stride;
    const uint8_t *s = src->pixels + (size_t)y * src->stride;
    const uint8_t *m = mask ? mask + (size_t)y * mask_stride : nullptr;
    for (int x = 0; x < dst->width; x++, d += 4, s += 4) {
      float k = t * (m ? m[x] / 255.f : 1.f);
      // アルファ込みで補間 (乗算済みで混ぜてから戻す)
      float ad = d[3] / 255.f, as = s[3] / 255.f;
      float ao = ad + (as - ad) * k;
      for (int c = 0; c < 3; c++) {
        float pd = d[c] / 255.f * ad, ps = s[c] / 255.f * as;
        float po = pd + (ps - pd) * k;
        d[c] = to8(ao > 0.f ? po / ao : 0.f);
      }
      d[3] = to8(ao);
    }
  }
  });
}

}  // extern "C"

// --- ぼかし ------------------------------------------------------------------
namespace {

// 半径 r の箱ぼかしを 1 行 (または 1 列) に掛ける (端は端の値で延長)
void boxPass(float *v, int n, int step, int r, std::vector<float> &tmp) {
  if (r <= 0 || n <= 1) return;
  tmp.resize((size_t)n);
  const float inv = 1.f / (2 * r + 1);
  float acc = v[0] * (r + 1);
  for (int i = 1; i <= r; i++) acc += v[(size_t)std::min(i, n - 1) * step];
  for (int i = 0; i < n; i++) {
    tmp[(size_t)i] = acc * inv;
    const int add = std::min(i + r + 1, n - 1), sub = std::max(i - r, 0);
    acc += v[(size_t)add * step] - v[(size_t)sub * step];
  }
  for (int i = 0; i < n; i++) v[(size_t)i * step] = tmp[(size_t)i];
}

// ガウス sigma を箱ぼかし 3 回で近似するための半径 (W. Jarosz / Kovesi の式)
void boxRadii(double sigma, int r[3]) {
  const int n = 3;
  double wIdeal = std::sqrt(12.0 * sigma * sigma / n + 1.0);
  int wl = (int)std::floor(wIdeal);
  if (wl % 2 == 0) wl--;
  const int wu = wl + 2;
  const double mIdeal = (12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4.0);
  const int m = (int)std::round(mIdeal);
  for (int i = 0; i < n; i++) r[i] = ((i < m ? wl : wu) - 1) / 2;
}

}  // anonymous namespace

extern "C" void psdfx_blur_plane(uint8_t *plane, int width, int height, int stride, double sigma) {
  if (!plane || width <= 0 || height <= 0 || !(sigma > 0.0)) return;
  std::vector<float> f((size_t)width * height);
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++) f[(size_t)y * width + x] = plane[(size_t)y * stride + x];
  int r[3];
  boxRadii(sigma, r);
  const long long work = (long long)width * height;
  for (int pass = 0; pass < 3; pass++) {
    psdfx_internal::parallelFor(0, height, work, [&](int a, int b) {
      std::vector<float> tmp;
      for (int y = a; y < b; y++) boxPass(&f[(size_t)y * width], width, 1, r[pass], tmp);
    });
    psdfx_internal::parallelFor(0, width, work, [&](int a, int b) {
      std::vector<float> tmp;
      for (int x = a; x < b; x++) boxPass(&f[(size_t)x], height, width, r[pass], tmp);
    });
  }
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++)
      plane[(size_t)y * stride + x] = to8(f[(size_t)y * width + x] / 255.f);
}
