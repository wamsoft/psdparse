// psdfx 内部: 合成の行処理の AVX2 版。このファイルだけ AVX2 を有効にしてビルドし
// (FMA は使わない: スカラー版と演算順・丸めを同じにするため)、実行時に CPU が
// AVX2 に対応しているときだけ呼ぶ。式は psdfx.cpp の blendChannel / blendPixel と同じ。
#include "psdfx.h"
#include "psdfx_simd.h"

#include <cstdlib>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define PSDFX_HAVE_AVX2_TU 1
#include <immintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif
#endif

namespace psdfx_internal {

#if PSDFX_HAVE_AVX2_TU

#if defined(__GNUC__) || defined(__clang__)
#define PSDFX_AVX2 __attribute__((target("avx2")))
#else
#define PSDFX_AVX2
#endif

namespace {

enum Mode { NORM, MUL, SCRN, DARK, LITE, OVER, HLIT, SLIT, LDDG, LBRN, DIFF, SMUD, FSUB, UNSUPPORTED };

Mode modeOf(uint32_t key) {
  switch (key) {
  case PSDFX_KEY('n','o','r','m'): case PSDFX_KEY('p','a','s','s'): case PSDFX_KEY('d','i','s','s'): return NORM;
  case PSDFX_KEY('m','u','l',' '): return MUL;
  case PSDFX_KEY('s','c','r','n'): return SCRN;
  case PSDFX_KEY('d','a','r','k'): return DARK;
  case PSDFX_KEY('l','i','t','e'): return LITE;
  case PSDFX_KEY('o','v','e','r'): return OVER;
  case PSDFX_KEY('h','L','i','t'): return HLIT;
  case PSDFX_KEY('s','L','i','t'): return SLIT;
  case PSDFX_KEY('l','d','d','g'): return LDDG;
  case PSDFX_KEY('l','b','r','n'): return LBRN;
  case PSDFX_KEY('d','i','f','f'): return DIFF;
  case PSDFX_KEY('s','m','u','d'): return SMUD;
  case PSDFX_KEY('f','s','u','b'): return FSUB;
  default: return UNSUPPORTED;
  }
}

// スカラー版の std::min(a, b) / std::max(a, b) と同じ選び方 (等しいときは a)
PSDFX_AVX2 inline __m256 smin(__m256 a, __m256 b) { return _mm256_blendv_ps(a, b, _mm256_cmp_ps(b, a, _CMP_LT_OQ)); }
PSDFX_AVX2 inline __m256 smax(__m256 a, __m256 b) { return _mm256_blendv_ps(a, b, _mm256_cmp_ps(a, b, _CMP_LT_OQ)); }
PSDFX_AVX2 inline __m256 sel(__m256 cond, __m256 t, __m256 f) { return _mm256_blendv_ps(f, t, cond); }

template <Mode M>
PSDFX_AVX2 inline __m256 blendCh(__m256 b, __m256 s) {
  const __m256 one = _mm256_set1_ps(1.f), two = _mm256_set1_ps(2.f), half = _mm256_set1_ps(0.5f),
               zero = _mm256_setzero_ps();
  switch (M) {
  case MUL: return _mm256_mul_ps(b, s);
  case SCRN: return _mm256_sub_ps(_mm256_add_ps(b, s), _mm256_mul_ps(b, s));
  case DARK: return smin(b, s);
  case LITE: return smax(b, s);
  case OVER: {
    const __m256 lo = _mm256_mul_ps(_mm256_mul_ps(two, s), b);
    const __m256 hi = _mm256_sub_ps(one, _mm256_mul_ps(_mm256_mul_ps(two, _mm256_sub_ps(one, s)), _mm256_sub_ps(one, b)));
    return sel(_mm256_cmp_ps(b, half, _CMP_LE_OQ), lo, hi);
  }
  case HLIT: {
    const __m256 lo = _mm256_mul_ps(_mm256_mul_ps(two, s), b);
    const __m256 hi = _mm256_sub_ps(one, _mm256_mul_ps(_mm256_mul_ps(two, _mm256_sub_ps(one, s)), _mm256_sub_ps(one, b)));
    return sel(_mm256_cmp_ps(s, half, _CMP_LE_OQ), lo, hi);
  }
  case SLIT: {
    // s <= 0.5: 2 b s + b b (1 - 2 s)、それ以外: 2 b (1 - s) + sqrt(b) (2 s - 1)
    const __m256 twoB = _mm256_mul_ps(two, b), twoS = _mm256_mul_ps(two, s);
    const __m256 lo = _mm256_add_ps(_mm256_mul_ps(twoB, s), _mm256_mul_ps(_mm256_mul_ps(b, b), _mm256_sub_ps(one, twoS)));
    const __m256 hi = _mm256_add_ps(_mm256_mul_ps(twoB, _mm256_sub_ps(one, s)),
                                    _mm256_mul_ps(_mm256_sqrt_ps(b), _mm256_sub_ps(twoS, one)));
    return sel(_mm256_cmp_ps(s, half, _CMP_LE_OQ), lo, hi);
  }
  case LDDG: return smin(one, _mm256_add_ps(b, s));
  case LBRN: return smax(zero, _mm256_sub_ps(_mm256_add_ps(b, s), one));
  case DIFF: return _mm256_andnot_ps(_mm256_set1_ps(-0.f), _mm256_sub_ps(b, s));
  case SMUD: return _mm256_sub_ps(_mm256_add_ps(b, s), _mm256_mul_ps(_mm256_mul_ps(two, b), s));
  case FSUB: return smax(zero, _mm256_sub_ps(b, s));
  default: return s;
  }
}

// to8: (uint8)(clamp01(v) * 255 + 0.5)
PSDFX_AVX2 inline __m256i to8(__m256 v) {
  // clamp01 は v < 0 ? 0 : (v > 1 ? 1 : v)
  const __m256 c = sel(_mm256_cmp_ps(v, _mm256_setzero_ps(), _CMP_LT_OQ), _mm256_setzero_ps(),
                       sel(_mm256_cmp_ps(v, _mm256_set1_ps(1.f), _CMP_GT_OQ), _mm256_set1_ps(1.f), v));
  return _mm256_cvttps_epi32(_mm256_add_ps(_mm256_mul_ps(c, _mm256_set1_ps(255.f)), _mm256_set1_ps(0.5f)));
}

template <Mode M>
PSDFX_AVX2 int rowKernel(uint8_t *d, const uint8_t *s, const uint8_t *m, int n, float op, bool atop) {
  const __m256 k255 = _mm256_set1_ps(255.f), one = _mm256_set1_ps(1.f), zero = _mm256_setzero_ps();
  const __m256 vop = _mm256_set1_ps(op);
  const __m256i m8 = _mm256_set1_epi32(0xff);
  int x = 0;
  for (; x + 8 <= n; x += 8) {
    const __m256i sp = _mm256_loadu_si256((const __m256i *)(s + (size_t)x * 4));
    // as = s.a / 255 * op (マスクがあれば x m / 255)
    __m256 as = _mm256_mul_ps(_mm256_div_ps(_mm256_cvtepi32_ps(_mm256_srli_epi32(sp, 24)), k255), vop);
    if (m) {
      const __m256i mi = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(m + x)));
      as = _mm256_mul_ps(as, _mm256_div_ps(_mm256_cvtepi32_ps(mi), k255));
    }
    const __m256 skip0 = _mm256_cmp_ps(as, zero, _CMP_LE_OQ);
    if (_mm256_movemask_ps(skip0) == 0xff) continue;
    const __m256i dp = _mm256_loadu_si256((const __m256i *)(d + (size_t)x * 4));
    const __m256 ab = _mm256_div_ps(_mm256_cvtepi32_ps(_mm256_srli_epi32(dp, 24)), k255);
    __m256 skip = skip0;
    if (atop) skip = _mm256_or_ps(skip, _mm256_cmp_ps(ab, zero, _CMP_LE_OQ));
    if (_mm256_movemask_ps(skip) == 0xff) continue;
    // チャンネル (R = bit16, G = bit8, B = bit0)
    __m256 bc[3], sc[3];
    for (int c = 0; c < 3; c++) {
      const int sh = 16 - 8 * c;
      bc[c] = _mm256_div_ps(_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(dp, sh), m8)), k255);
      sc[c] = _mm256_div_ps(_mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(sp, sh), m8)), k255);
    }
    __m256 outC[3], outA;
    if (atop) {
      // b = as B + (1 - as) b、アルファはそのまま
      const __m256 oneMinusAs = _mm256_sub_ps(one, as);
      for (int c = 0; c < 3; c++) {
        const __m256 B = blendCh<M>(bc[c], sc[c]);
        outC[c] = _mm256_add_ps(_mm256_mul_ps(as, B), _mm256_mul_ps(oneMinusAs, bc[c]));
      }
      outA = ab;
    } else {
      // ao = as + ab (1 - as)
      // co = as (1 - ab) s + as ab B + (1 - as) ab b、b = ao > 0 ? co / ao : 0
      const __m256 oneMinusAs = _mm256_sub_ps(one, as);
      const __m256 ao = _mm256_add_ps(as, _mm256_mul_ps(ab, oneMinusAs));
      const __m256 w1 = _mm256_mul_ps(as, _mm256_sub_ps(one, ab));
      const __m256 w2 = _mm256_mul_ps(as, ab);
      const __m256 w3 = _mm256_mul_ps(oneMinusAs, ab);
      const __m256 aoPos = _mm256_cmp_ps(ao, zero, _CMP_GT_OQ);
      for (int c = 0; c < 3; c++) {
        const __m256 B = blendCh<M>(bc[c], sc[c]);
        const __m256 co = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(w1, sc[c]), _mm256_mul_ps(w2, B)),
                                        _mm256_mul_ps(w3, bc[c]));
        outC[c] = _mm256_and_ps(_mm256_div_ps(co, ao), aoPos);
      }
      outA = ao;
      if (M == NORM) {
        // 通常モードで下地が透明なら上の色をそのまま (スカラー版と同じ近道)
        const __m256 empty = _mm256_cmp_ps(ab, zero, _CMP_LE_OQ);
        for (int c = 0; c < 3; c++) outC[c] = sel(empty, sc[c], outC[c]);
        outA = sel(empty, as, outA);
      }
    }
    __m256i out = _mm256_slli_epi32(to8(outA), 24);
    for (int c = 0; c < 3; c++) out = _mm256_or_si256(out, _mm256_slli_epi32(to8(outC[c]), 16 - 8 * c));
    out = _mm256_castps_si256(_mm256_blendv_ps(_mm256_castsi256_ps(out), _mm256_castsi256_ps(dp), skip));
    _mm256_storeu_si256((__m256i *)(d + (size_t)x * 4), out);
  }
  return x;
}

bool detectAvx2() {
#if defined(_MSC_VER) && !defined(__clang__)
  int r[4];
  __cpuid(r, 0);
  if (r[0] < 7) return false;
  __cpuid(r, 1);
  const bool osxsave = (r[2] & (1 << 27)) != 0, avx = (r[2] & (1 << 28)) != 0;
  if (!osxsave || !avx) return false;
  if ((_xgetbv(0) & 6) != 6) return false;   // OS が YMM を保存する
  __cpuidex(r, 7, 0);
  return (r[1] & (1 << 5)) != 0;
#else
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx2");
#endif
}

}  // namespace

bool simdAvailable() {
  // 環境変数 PSDFX_SIMD=0 で使わない (比較・切り分け用)
  static const bool ok = [] {
    const char *e = std::getenv("PSDFX_SIMD");
    if (e && e[0] == '0') return false;
    return detectAvx2();
  }();
  return ok;
}

int compositeRowSimd(uint8_t *d, const uint8_t *s, const uint8_t *m, int n, uint32_t key, float op,
                     bool atop) {
  if (!simdAvailable()) return 0;
  switch (modeOf(key)) {
  case NORM: return rowKernel<NORM>(d, s, m, n, op, atop);
  case MUL: return rowKernel<MUL>(d, s, m, n, op, atop);
  case SCRN: return rowKernel<SCRN>(d, s, m, n, op, atop);
  case DARK: return rowKernel<DARK>(d, s, m, n, op, atop);
  case LITE: return rowKernel<LITE>(d, s, m, n, op, atop);
  case OVER: return rowKernel<OVER>(d, s, m, n, op, atop);
  case HLIT: return rowKernel<HLIT>(d, s, m, n, op, atop);
  case SLIT: return rowKernel<SLIT>(d, s, m, n, op, atop);
  case LDDG: return rowKernel<LDDG>(d, s, m, n, op, atop);
  case LBRN: return rowKernel<LBRN>(d, s, m, n, op, atop);
  case DIFF: return rowKernel<DIFF>(d, s, m, n, op, atop);
  case SMUD: return rowKernel<SMUD>(d, s, m, n, op, atop);
  case FSUB: return rowKernel<FSUB>(d, s, m, n, op, atop);
  default: return 0;
  }
}

#else   // x86 以外 (ARM など): SIMD 版なし

bool simdAvailable() { return false; }
int compositeRowSimd(uint8_t *, const uint8_t *, const uint8_t *, int, uint32_t, float, bool) { return 0; }

#endif

}  // namespace psdfx_internal
