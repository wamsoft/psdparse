
#include "psdfile.h"

#include <algorithm>
#include "psdparse.h"   // MemoryReader / VectorReader
#include <cstring>
#include <cmath>
#include <memory>
#include <vector>

#define USE_ZLIB
#ifdef USE_ZLIB

// vcpkg環境のzlibではオンにするとリンクエラーになる
// #define ZLIB_WINAPI

#define NOMINMAX
#include <zlib.h>
#endif
#include <cstring>

// #define ENABLE_BMP_OUTPUT
#ifdef ENABLE_BMP_OUTPUT
namespace psd {
extern bool saveBmp(void* buffer, int width, int height, int size, const char *filename);
}
#endif

namespace psd {

  static const float32_t OPAQ_F32 = 1.0f;
  static const uint32_t  OPAQ_INT_BE = byteSwap32(*(uint32_t*)&(OPAQ_F32));

  // --------------------------------------------------------------------------
  // チャネルコンポーネントパック
  // --------------------------------------------------------------------------

  // コンポーネントカラーを32bitRGBAにパックする
  template <typename T>
  inline uint32_t
  rgbaCompoToRgba32(const ColorFormat &fmt, T r, T g, T b, T a)
  {
    // 値はBigEndian
#ifdef PSD_LITTLE_ENDIAN
    return (uint32_t)(((a&0xff) << fmt.aShift) | ((r&0xff) << fmt.rShift) |
                      ((g&0xff) << fmt.gShift) | ((b&0xff) << fmt.bShift));
#else
    int cmpShift = (sizeof(T) - sizeof(uint8_t)) * 8;
    return (uint32_t)(((a>>cmpShift) << fmt.aShift) | ((r>>cmpShift) << fmt.rShift) |
                      ((g>>cmpShift) << fmt.gShift) | ((b>>cmpShift) << fmt.bShift));

#endif
  }

  // コンポーネントカラーを32bitRGBAにパックする(Aチャネル不透明)
  template <typename T>
  inline uint32_t
  rgbaCompoToRgba32(const ColorFormat &fmt, T r, T g, T b)
  {
    return rgbaCompoToRgba32<T>(fmt, r, g, b, (T)(-1));
  }

  // 32bit (float) の色成分を 8bit の表示値へ。Photoshop の 32bit 文書は線形光
  // (ガンマ 1.0) なので、sRGB の伝達特性で符号化してから量子化する。範囲外
  // (HDR の 1.0 超え / 負値) は頭打ち。Photoshop が保存するサムネイルと一致する。
  inline uint32_t f32ColorTo8(float v) {
    double c = v;
    if (!(c > 0.0)) return 0;              // 負値と NaN
    if (c >= 1.0) return 255;
    c = (c <= 0.0031308) ? (12.92 * c) : (1.055 * std::pow(c, 1.0 / 2.4) - 0.055);
    return (uint32_t)(c * 255.0 + 0.5);
  }
  // アルファ (不透明度) は線形のまま量子化
  inline uint32_t f32AlphaTo8(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return (uint32_t)(v * 255.0f + 0.5f);
  }

  // コンポーネントカラーを32bitRGBAにパックする
  //   32bit コンポーネントは浮動小数点なので特殊化で対応
  template<>
  inline uint32_t
  rgbaCompoToRgba32<uint32_t>(const ColorFormat &fmt,
                              uint32_t _r, uint32_t _g, uint32_t _b, uint32_t _a)
  {
    pun32 r, g, b, a;
#ifdef PSD_LITTLE_ENDIAN
    r.i = byteSwap32(_r);
    g.i = byteSwap32(_g);
    b.i = byteSwap32(_b);
    a.i = byteSwap32(_a);
#else
    r.i = _r;
    g.i = _g;
    b.i = _b;
    a.i = _a;
#endif
    return (uint32_t)((f32AlphaTo8(a.f) << fmt.aShift) |
                      (f32ColorTo8(r.f) << fmt.rShift) |
                      (f32ColorTo8(g.f) << fmt.gShift) |
                      (f32ColorTo8(b.f) << fmt.bShift));
  }

  // コンポーネントカラーを32bitRGBAにパックする(Aチャネル不透明)
  //   32bit コンポーネントは浮動小数点なので特殊化で対応
  template<>
  inline uint32_t
  rgbaCompoToRgba32<uint32_t>(const ColorFormat &fmt,
                              uint32_t r, uint32_t g, uint32_t b)
  {
    return rgbaCompoToRgba32<uint32_t>(fmt, r, g, b, OPAQ_INT_BE);
  }

  // グレーを32bitRGBAにパックする
  template <typename T>
  inline uint32_t
  grayToRgba32(const ColorFormat &fmt, T g, T a)
  {
#ifdef PSD_LITTLE_ENDIAN
    uint8_t _g = g & 0xff;
    return (uint32_t)((a << fmt.aShift) | (_g << fmt.rShift) |
                      (_g << fmt.gShift) | (_g << fmt.bShift));
#else
    int cmpShift = (sizeof(T) - sizeof(uint8_t)) * 8;
    uint8_t _g = (g >> cmpShift) & 0xff;
    return (uint32_t)(((a>>cmpShift) << fmt.aShift) | (_g << fmt.rShift) |
                      (_g << fmt.gShift) | (_g << fmt.bShift));
#endif
  }

  // グレーを32bitRGBAにパックする(Aチャネル不透明)
  template <typename T>
  inline uint32_t
  grayToRgba32(const ColorFormat &fmt, T g)
  {
    return grayToRgba32<T>(fmt, g, (T)(-1));
  }

  // グレーを32bitRGBAにパックする
  //   32bit コンポーネントは浮動小数点なので特殊化で対応
  template<>
  inline uint32_t
  grayToRgba32<uint32_t>(const ColorFormat &fmt, uint32_t _g, uint32_t _a)
  {
    pun32 g, a;
#ifdef PSD_LITTLE_ENDIAN
    g.i = byteSwap32(_g);
    a.i = byteSwap32(_a);
#else
    g.i = _g;
    a.i = _a;
#endif
    uint32_t gInt = f32ColorTo8(g.f);
    return (uint32_t)((f32AlphaTo8(a.f) << fmt.aShift) |
                      (gInt << fmt.rShift) |
                      (gInt << fmt.gShift) |
                      (gInt << fmt.bShift));
  }

  // グレーを32bitRGBAにパックする(Aチャネル不透明)
  //   32bit コンポーネントは浮動小数点なので特殊化で対応
  template <>
  inline uint32_t
  grayToRgba32<uint32_t>(const ColorFormat &fmt, uint32_t g)
  {
    return grayToRgba32<uint32_t>(fmt, g, OPAQ_INT_BE);
  }

  // CMYKを32bitRGBAにパックする
  template <typename T>
  inline uint32_t
  cmykCompoToRgba32(const ColorFormat &fmt, T _c, T _m, T _y, T _k, T _a)
  {
#ifdef PSD_LITTLE_ENDIAN
    uint8_t c = 255 - _c & 0xff;
    uint8_t m = 255 - _m & 0xff;
    uint8_t y = 255 - _y & 0xff;
    uint8_t k = 255 - _k & 0xff;
    uint8_t a = _a & 0xff;
    uint8_t invK = _k & 0xff;
#else
    static const int f = (sizeof(T) - 1) * 8;
    uint8_t c = 255 - (_c >> f) & 0xff;
    uint8_t m = 255 - (_m >> f) & 0xff;
    uint8_t y = 255 - (_y >> f) & 0xff;
    uint8_t k = 255 - (_k >> f) & 0xff;
    uint8_t a = (_a >> f) & 0xff;
    uint8_t invK = (_k >> f) & 0xff;
#endif
    uint8_t r = (uint8_t)((invK * (255 - c)) >> 8);
    uint8_t g = (uint8_t)((invK * (255 - m)) >> 8);
    uint8_t b = (uint8_t)((invK * (255 - y)) >> 8);
    return rgbaCompoToRgba32<uint8_t>(fmt, r, g, b, a);
  }

  // CMYKを32bitRGBAにパックする(Aチャネル不透明)
  template <typename T>
  inline uint32_t
  cmykCompoToRgba32(const ColorFormat &fmt, T c, T m, T y, T k)
  {
    return cmykCompoToRgba32<T>(fmt, c, m, y, k, (T)(-1));
  }

  // MEMO CS5まででは32bit/chのCMYKデータは存在しない
  // CMYKを32bitRGBAにパックする
  //   32bit コンポーネントは浮動小数点なので特殊化で対応
  template <>
  inline uint32_t
  cmykCompoToRgba32<uint32_t>(const ColorFormat &fmt,
                              uint32_t _c, uint32_t _m, uint32_t _y, uint32_t _k,
                              uint32_t _a)
  {
    pun32 c, m, y, k, a;
#ifdef PSD_LITTLE_ENDIAN
    c.i = byteSwap32(_c);
    m.i = byteSwap32(_m);
    y.i = byteSwap32(_y);
    k.i = byteSwap32(_k);
    a.i = byteSwap32(_a);
#else
    c.i = _c;
    m.i = _m
    y.i = _y;
    k.i = _k;
    a.i = _a;
#endif
    float32_t invK = 1.0f - k.f;
    uint8_t r = (uint8_t)((invK * (1.0f - c.f)) * 255);
    uint8_t g = (uint8_t)((invK * (1.0f - m.f)) * 255);
    uint8_t b = (uint8_t)((invK * (1.0f - y.f)) * 255);
    return rgbaCompoToRgba32<uint8_t>(fmt, r, g, b, (uint8_t)(a.f * 255));
  }

  // CMYKを32bitRGBAにパックする(Aチャネル不透明)
  //   32bit コンポーネントは浮動小数点なので特殊化で対応
  template <>
  inline uint32_t
  cmykCompoToRgba32<uint32_t>(const ColorFormat &fmt,
                              uint32_t c, uint32_t m, uint32_t y, uint32_t k)
  {
    return cmykCompoToRgba32<uint32_t>(fmt, c, m, y, k, OPAQ_INT_BE);
  }
  
  // --------------------------------------------------------------------------
  // チャネルマージ
  // --------------------------------------------------------------------------

  // 1bit bitmap を統合し pitch byte 単位で merged にフィルする
  void mergeChannelsBitmap(void *merged, int width, int height,
                           uint8_t *src, const ColorFormat &format, int bufPitchByte)
  {
    int pitchPixel = std::abs(bufPitchByte) / 4;
    int linePixel  = std::min(width, pitchPixel);

    uint32_t c[2];
    c[0] = grayToRgba32<uint8_t>(format, 0xff, 0xff);
    c[1] = grayToRgba32<uint8_t>(format, 0x0, 0xff);

    int fullByte = linePixel / 8;
    int oddBits  = linePixel % 8;
    for (int y = 0; y < height; y++) {
      uint8_t *bits = src + ((width + 7) / 8) * y;
      uint32_t *pix = (uint32_t *)((uint8_t *)merged + bufPitchByte * y);
      for (int x = 0; x < fullByte; x++) {
        *pix++ = c[*bits & (1 << 7)];
        *pix++ = c[*bits & (1 << 6)];
        *pix++ = c[*bits & (1 << 5)];
        *pix++ = c[*bits & (1 << 4)];
        *pix++ = c[*bits & (1 << 3)];
        *pix++ = c[*bits & (1 << 2)];
        *pix++ = c[*bits & (1 << 1)];
        *pix++ = c[*bits & (1 << 0)];
        bits++;
      }
      for (int x = 0; x < oddBits; x++) {
        *pix++ = c[*bits & (1 << (7 - x))];
      }
    }
  }

  // グレイチャネルを統合し pitch byte 単位で merged にフィルする
  template <typename T>
  void mergeChannelsGray(void *merged, int width, int height,
                         std::vector<int> &ids,
                         std::vector<uint8_t*> &decodedChannels,
                         const ColorFormat &format, int bufPitchByte)
  {
    T *chA = 0, *chG = 0;
    for(uint32_t i = 0; i < ids.size(); i ++) {
      switch (ids[i]) {
      case CH_ID_TRANSP: chA = (T*)decodedChannels[i]; break;
      case CH_ID_GRAY:   chG = (T*)decodedChannels[i]; break;
      default: /* TODO err */                          break;
      }
    }

    int pitchPixel = std::abs(bufPitchByte) / 4;
    int linePixel  = std::min(width, pitchPixel);
    if (chA) {
      for (int y = 0; y < height; y++) {
        uint32_t *pix = (uint32_t *)((uint8_t *)merged + bufPitchByte * y);
        T *g = chG + width * y;
        T *a = chA + width * y;
        for (int x = 0; x < linePixel; x++) {
          *pix++ = grayToRgba32<T>(format, *g++, *a++);
        }
      }
    } else {
      for (int y = 0; y < height; y++) {
        uint32_t *pix = (uint32_t *)((uint8_t *)merged + bufPitchByte * y);
        T *g = chG + width * y;
        for (int x = 0; x < linePixel; x++) {
          *pix++ = grayToRgba32<T>(format, *g++);
        }
      }
    }
  }

  // インデックスカラーを統合し pitch byte 単位で merged にフィルする
  void mergeChannelsIndex(void *merged, int width, int height,
                          uint8_t *src, ColorTable &table,
                          const ColorFormat &format, int bufPitchByte)
  {
    int pitchPixel = std::abs(bufPitchByte) / 4;
    int linePixel  = std::min(width, pitchPixel);

    for (int y = 0; y < height; y++) {
      uint8_t *index = src + width * y;
      uint32_t *pix = (uint32_t *)((uint8_t *)merged + bufPitchByte * y);
      for (int x = 0; x < linePixel; x++) {
        ColorRgba &c = table.colors[*index++];
        *pix++ = rgbaCompoToRgba32<uint8_t>(format, c.r, c.g, c.b, c.a);
      }
    }
  }  

  // RGBチャネルを統合し pitch byte 単位で merged にフィルする
  template <typename T>
  void mergeChannelsRgb(void *merged, int width, int height,
                        std::vector<int> &ids, std::vector<uint8_t*> &decodedChannels,
                        const ColorFormat &format, int bufPitchByte)
  {
    T *chR = 0, *chG = 0, *chB = 0, *chA = 0;
    for(uint32_t i = 0; i < ids.size(); i ++) {
      switch (ids[i]) {
      case CH_ID_TRANSP:     chA = (T*)decodedChannels[i]; break;
      case CH_ID_RGB_R:      chR = (T*)decodedChannels[i]; break;
      case CH_ID_RGB_G:      chG = (T*)decodedChannels[i]; break;
      case CH_ID_RGB_B:      chB = (T*)decodedChannels[i]; break;
      default: /* TODO err */                              break;
      }
    }

    int pitchPixel = std::abs(bufPitchByte) / 4;
    int linePixel  = std::min(width, pitchPixel);
    if (chA) {
      for (int y = 0; y < height; y++) {
        uint32_t *pix = (uint32_t *)((uint8_t *)merged + bufPitchByte * y);
        T *a = chA + width * y;
        T *r = chR + width * y;
        T *g = chG + width * y;
        T *b = chB + width * y;
        for (int x = 0; x < linePixel; x++) {
          *pix++ = rgbaCompoToRgba32<T>(format, *r++, *g++, *b++, *a++);
        }
      }
    } else {
      for (int y = 0; y < height; y++) {
        uint32_t *pix = (uint32_t *)((uint8_t *)merged + bufPitchByte * y);
        T *r = chR + width * y;
        T *g = chG + width * y;
        T *b = chB + width * y;
        for (int x = 0; x < linePixel; x++) {
          *pix++ = rgbaCompoToRgba32<T>(format, *r++, *g++, *b++);
        }
      }
    }
  }

  // CMYKチャネルを統合し pitch byte 単位で merged にフィルする
  template <typename T>
  void mergeChannelsCmyk(void *merged, int width, int height,
                         std::vector<int> &ids,
                         std::vector<uint8_t*> &decodedChannels,
                         const ColorFormat &format, int bufPitchByte)
  {
    T *chC = 0, *chM = 0, *chY = 0, *chK = 0, *chA = 0;
    for(uint32_t i = 0; i < ids.size(); i ++) {
      switch (ids[i]) {
      case CH_ID_TRANSP: chA = (T*)decodedChannels[i]; break;
      case CH_ID_CMYK_C: chC = (T*)decodedChannels[i]; break;
      case CH_ID_CMYK_M: chM = (T*)decodedChannels[i]; break;
      case CH_ID_CMYK_Y: chY = (T*)decodedChannels[i]; break;
      case CH_ID_CMYK_K: chK = (T*)decodedChannels[i]; break;
      default: /* TODO err */                          break;
      }
    }

    int pitchPixel = std::abs(bufPitchByte) / 4;
    int linePixel  = std::min(width, pitchPixel);
    if (chA) {
      for (int y = 0; y < height; y++) {
        uint32_t *pix = (uint32_t *)((uint8_t *)merged + bufPitchByte * y);
        T *ap = chA + width * y;
        T *cp = chC + width * y;
        T *mp = chM + width * y;
        T *yp = chY + width * y;
        T *kp = chK + width * y;
        for (int x = 0; x < linePixel; x++) {
          *pix++ = cmykCompoToRgba32<T>(format, *cp++, *mp++, *yp++, *kp++, *ap++);
        }
      }
    } else {
      for (int y = 0; y < height; y++) {
        uint32_t *pix = (uint32_t *)((uint8_t *)merged + bufPitchByte * y);
        T *cp = chC + width * y;
        T *mp = chM + width * y;
        T *yp = chY + width * y;
        T *kp = chK + width * y;
        for (int x = 0; x < linePixel; x++) {
          *pix++ = cmykCompoToRgba32<T>(format, *cp++, *mp++, *yp++, *kp++);
        }
      }
    }
  }

  // CIELAB(D65) を sRGB 8bit へ変換する。 L∈[0,100], a,b∈[-128,127]。
  // Photoshop の Lab とは参照白(D50)やレンダリング intent が厳密には異なるため
  // 近似だが、従来 Lab は完全に未対応 (全ゼロ出力) だったのを実用値に置き換える。
  inline void labToRgb8(double L, double a, double b,
                        uint8_t &R, uint8_t &G, uint8_t &B)
  {
    double fy = (L + 16.0) / 116.0;
    double fx = fy + a / 500.0;
    double fz = fy - b / 200.0;
    auto finv = [](double t) {
      double t3 = t * t * t;
      return (t3 > 0.008856) ? t3 : (t - 16.0 / 116.0) / 7.787;
    };
    // D65 reference white
    double X = 0.95047 * finv(fx);
    double Y = 1.00000 * finv(fy);
    double Z = 1.08883 * finv(fz);
    // XYZ -> linear sRGB
    double rl =  3.2406 * X - 1.5372 * Y - 0.4986 * Z;
    double gl = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
    double bl =  0.0557 * X - 0.2040 * Y + 1.0570 * Z;
    auto gamma = [](double c) {
      c = (c <= 0.0031308) ? (12.92 * c) : (1.055 * std::pow(c, 1.0 / 2.4) - 0.055);
      return (c < 0.0) ? 0.0 : (c > 1.0 ? 1.0 : c);
    };
    R = (uint8_t)(gamma(rl) * 255.0 + 0.5);
    G = (uint8_t)(gamma(gl) * 255.0 + 0.5);
    B = (uint8_t)(gamma(bl) * 255.0 + 0.5);
  }

  // Lab チャネルを統合し pitch byte 単位で merged にフィルする。
  // 8bit: L=0..255→0..100, a/b=0..255→-128..127。 16bit は上位バイト近似。
  // 32bit Lab は Photoshop に存在しないため非対応。
  template <typename T>
  void mergeChannelsLab(void *merged, int width, int height,
                        std::vector<int> &ids,
                        std::vector<uint8_t*> &decodedChannels,
                        const ColorFormat &format, int bufPitchByte)
  {
    T *chL = 0, *chA = 0, *chB = 0, *chAlpha = 0;
    for (uint32_t i = 0; i < ids.size(); i++) {
      switch (ids[i]) {
      case CH_ID_TRANSP: chAlpha = (T*)decodedChannels[i]; break;
      case 0:            chL     = (T*)decodedChannels[i]; break;  // L
      case 1:            chA     = (T*)decodedChannels[i]; break;  // a
      case 2:            chB     = (T*)decodedChannels[i]; break;  // b
      default: break;
      }
    }
    if (!chL || !chA || !chB) return;

    // T の 1 サンプルを 0..255 相当へ正規化するスケール。
    const double norm = (sizeof(T) == 1) ? 1.0 : (255.0 / 65535.0);

    int pitchPixel = std::abs(bufPitchByte) / 4;
    int linePixel  = std::min(width, pitchPixel);
    for (int y = 0; y < height; y++) {
      uint32_t *pix = (uint32_t *)((uint8_t *)merged + bufPitchByte * y);
      T *lp = chL + width * y;
      T *ap = chA + width * y;
      T *bp = chB + width * y;
      T *alp = chAlpha ? chAlpha + width * y : 0;
      for (int x = 0; x < linePixel; x++) {
        double Lv = ((double)(*lp++) * norm) / 255.0 * 100.0;
        double av = ((double)(*ap++) * norm) - 128.0;
        double bv = ((double)(*bp++) * norm) - 128.0;
        uint8_t R, G, B;
        labToRgb8(Lv, av, bv, R, G, B);
        if (alp) {
          uint8_t A = (uint8_t)((double)(*alp++) * norm);
          *pix++ = rgbaCompoToRgba32<uint8_t>(format, R, G, B, A);
        } else {
          *pix++ = rgbaCompoToRgba32<uint8_t>(format, R, G, B);
        }
      }
    }
  }

  // 展開直後 (big-endian) のサンプル 1 個を 0..1 で読み書きする。
  //   uint8_t: 8bit / uint16_t: 16bit 整数 / uint32_t: 32bit float
  template <typename T> double loadSample(const uint8_t *p);
  template <> inline double loadSample<uint8_t>(const uint8_t *p) { return p[0] / 255.0; }
  template <> inline double loadSample<uint16_t>(const uint8_t *p) {
    return ((p[0] << 8) | p[1]) / 65535.0;
  }
  template <> inline double loadSample<uint32_t>(const uint8_t *p) {
    pun32 v;
    v.i = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    return v.f;
  }
  template <typename T> void storeSample(uint8_t *p, double v);
  template <> inline void storeSample<uint8_t>(uint8_t *p, double v) {
    p[0] = (uint8_t)(std::min(1.0, std::max(0.0, v)) * 255.0 + 0.5);
  }
  template <> inline void storeSample<uint16_t>(uint8_t *p, double v) {
    uint16_t x = (uint16_t)(std::min(1.0, std::max(0.0, v)) * 65535.0 + 0.5);
    p[0] = (uint8_t)(x >> 8); p[1] = (uint8_t)(x & 0xff);
  }
  template <> inline void storeSample<uint32_t>(uint8_t *p, double v) {
    pun32 x; x.f = (float)v;
    p[0] = (uint8_t)(x.i >> 24); p[1] = (uint8_t)(x.i >> 16);
    p[2] = (uint8_t)(x.i >> 8);  p[3] = (uint8_t)(x.i & 0xff);
  }

  // アルファチャネルにマスクチャネルをマージ
  template <typename T>
  void
  mergeMaskToAlpha(uint8_t *aCh, int al, int at, int ar, int ab,
                   uint8_t *mCh, int ml, int mt, int mr, int mb,
                   uint8_t defaultColor)
  {
    int aw = ar - al; int ah = ab - at;
    int mw = mr - ml; int mh = mb - mt;
    int aPitch = aw * sizeof(T);
    int mPitch = mw * sizeof(T);

    // マージ領域を処理
    // alphaとmaskの重なり領域
    int l = std::max(al, ml); int t = std::max(at, mt);
    int r = std::min(ar, mr); int b = std::min(ab, mb);
    int w = r - l; int h = b - t;
    if (w > 0 && h > 0) {
      // maskをalphaにマージ
      // チャネルは展開直後の big-endian のまま (16bit は整数、32bit は float)。
      // 0..1 に正規化して掛け、同じ表現へ戻す。
      int aOffsetX = l - al; int aOffsetY = t - at;
      int mOffsetX = l - ml; int mOffsetY = t - mt;
      for (int y = 0; y < h; y++) {
        uint8_t *ap = aCh + (aOffsetY + y) * aPitch + aOffsetX * (int)sizeof(T);
        uint8_t *mp = mCh + (mOffsetY + y) * mPitch + mOffsetX * (int)sizeof(T);
        for (int x = 0; x < w; x++, ap += sizeof(T), mp += sizeof(T)) {
          storeSample<T>(ap, loadSample<T>(ap) * loadSample<T>(mp));
        }
      }
    } else {
      // alphaとmaskの重なりがないのでここで終わり
      if (defaultColor == 0) {
        memset(aCh, 0x0, aw * ah * sizeof(T));
      }
      return;
    }

    // デフォルトカラーが0xffの場合は、マージ領域外は
    // αチャネルの値そのままになるので、何もしないで戻る
    if (defaultColor != 0) {
      return;
    }

    // マージ対象外の領域を処理
    // αチャネル領域をマージ範囲の座標で9個の矩形に分割
    struct rectangle {
      int w, h, l, t, r, b;
      rectangle() : w(0), h(0), l(0), t(0), r(0), b(0) {}
      void set(int _l, int _t, int _r, int _b) {
        l = _l; t = _t; r = _r; b = _b; w = r - l; h = b - t;
      }
      bool valid() { return (w > 0 && h > 0); }
      void invalidate() { w = h = 0; }
    } rect[3][3];
    
    // 座標をαチャネル領域相対に変換
    ml = std::max(0,  ml - al); mt = std::max(0,  mt - at);
    mr = std::min(aw, mr - al); mb = std::min(ah, mb - at);
    al = 0;  at = 0;
    ar = aw; ab = ah;

    rect[0][0].set(al, at, ml, mt);
    rect[0][1].set(ml, at, mr, mt);
    rect[0][2].set(mr, at, ar, mt);
    rect[1][0].set(al, mt, ml, mb);
    rect[1][1].invalidate(); // マージ対象なのでinvalidのまま残して無視
    rect[1][2].set(mr, mt, ar, mb);
    rect[2][0].set(al, mb, ml, ab);
    rect[2][1].set(ml, mb, mr, ab);
    rect[2][2].set(mr, mb, ar, ab);
  
    // 同じ行の矩形領域を結合
    bool lineCombined[3] = { true, true, true };
    for (int y = 0; y < 3; y++) {
      for (int x = 1; x >= 0; x--) {
        if (rect[y][x].valid() && rect[y][x+1].valid()) {
          rect[y][x].r = rect[y][x+1].r;
          rect[y][x].w = rect[y][x].r - rect[y][x].l;
          rect[y][x+1].invalidate();
        } else {
          lineCombined[y] = false;
        }
      }
    }

    // マージ対象以外をケア
    for (int y = 0; y < 3; y++) {
      if (lineCombined[y]) {
        // 行結合状態ならmemsetで領域一括セット
        rectangle &r = rect[y][0];
        uint8_t *out = aCh + r.t * aPitch;
        memset(out, 0x0, r.h * aPitch);
      } else {
        // 行結合済でなければ個別にライン単位処理
        for (int x = 0; x < 3; x++) {
          rectangle &r = rect[y][x];
          if (r.valid()) {
            for (int l = 0; l < r.h; l++) {
              uint8_t *out = aCh + (r.t + l) * aPitch + r.l * sizeof(T);
              memset(out, 0x0, r.w * sizeof(T));
            }
          }
        }
      }
    }
  }

  // --------------------------------------------------------------------------
  // 圧縮展開
  // --------------------------------------------------------------------------

  // RLE圧縮(PackBits)を展開する
  //   dstSize / srcSize: 展開先 / 入力の大きさ。壊れたデータや、矩形と食い違う
  //     チャンネル (マスク矩形が空なのに -2 チャンネルがある等) でも範囲外へ
  //     読み書きしないよう、どちらかの端に着いたらそこで打ち切る。
  //   countBytes: 行バイト数テーブルの 1 行あたりのバイト数 (PSD は 2、PSB は 4)
  // 戻り値はこのチャンネルの行データの合計バイト数 (次チャンネルの開始位置用)。
  inline uint32_t decodePackBits(uint8_t *dst, size_t dstSize,
                                 const uint8_t *src, size_t srcSize, int height,
                                 int channels=1, int targetCh=0, size_t lineDataOffset=0,
                                 int countBytes=2)
  {
    const size_t headerSize = (size_t)countBytes * height * channels;
    const size_t tableAt    = (size_t)countBytes * height * targetCh;
    if (height <= 0 || tableAt + (size_t)countBytes * height > srcSize) return 0;
    const uint8_t *lineBytesBE = src + tableAt;
    size_t   in        = headerSize + lineDataOffset;   // src 上の読み位置
    size_t   out       = 0;                             // dst 上の書き位置
    uint32_t readBytes = 0;

    for (int y = 0; y < height; y++, lineBytesBE += countBytes) {
      uint32_t lineBytes = 0;
      for (int i = 0; i < countBytes; i++) lineBytes = (lineBytes << 8) | lineBytesBE[i];
      readBytes += lineBytes;
      size_t lineEnd = in + lineBytes;
      if (lineEnd > srcSize) lineEnd = srcSize;
      while (in < lineEnd) {
        uint8_t header = src[in++];
        if (header > 128) {
          // ランレングス分同値コピー
          size_t n = (size_t)(257 - header);
          if (in >= lineEnd) break;
          uint8_t v = src[in++];
          if (n > dstSize - out) n = dstSize - out;
          memset(dst + out, v, n);
          out += n;
        } else if (header < 128) {
          // ランがないのでベタコピー
          size_t n = (size_t)header + 1;
          if (n > lineEnd - in) n = lineEnd - in;
          size_t w = n > dstSize - out ? dstSize - out : n;
          memcpy(dst + out, src + in, w);
          in  += n;
          out += w;
        }
        // 128 は何もしない (PackBits の no-op)
      }
      in = lineEnd;
    }

    return readBytes;
  }

#ifdef USE_ZLIB
  // prediction なしのunzip展開
  bool decodeZipWithoutPrediction(void *dst, int dstSize, void *src, int srcSize)
  {
    z_stream zs;
    memset(&zs, 0, sizeof(z_stream));
    zs.zalloc    = Z_NULL;
    zs.zfree     = Z_NULL;
    zs.opaque    = Z_NULL;
    zs.data_type = Z_BINARY;
    zs.next_in   = (Bytef *)src;
    zs.avail_in  = srcSize;
    zs.next_out  = (Bytef *)dst;
    zs.avail_out = dstSize;

    int ret = inflateInit(&zs);
    if(ret != Z_OK) {
      return false;
    }

    do {
      ret = inflate(&zs, Z_NO_FLUSH);
      if (ret != Z_OK) {
        break;
      }
    } while (zs.avail_out != 0);

    inflateEnd(&zs);

    return (ret == Z_STREAM_END);
  }

  // prediction つき zip
  bool decodeZipWithPrediction(void *dst, int dstSize, void *src, int srcSize,
                                int width, int height, int depth)
  {
    void *buf = 0;
    if (depth == 8 || depth == 16)  {
      buf = dst;
    } else if (depth == 32) {
      buf = new uint8_t[dstSize];
    } else {
      return false; // err
    }
    bool success = decodeZipWithoutPrediction(buf, dstSize, src, srcSize);
    if(!success) {
      if (buf != dst) {
        delete [] buf;
      }
      return false;
    }

    // added
    if (depth == 8)  {
      uint8_t *ptrData = (uint8_t*)buf;
      for (int i = 0; i < height; i++) {
        uint8_t *ptr    = ptrData + i * width;
        uint8_t *ptrEnd = ptrData + (i + 1) * width;
        ptr++;
        while (ptr < ptrEnd) {
          *ptr += *(ptr - 1);
          ptr++;
        }
      }
    } else if (depth == 16)  { // changed
      uint8_t *ptrData = (uint8_t *)buf;
      for (int i = 0; i < height; i++) {
#ifdef PSD_LITTLE_ENDIAN
        // 後のチャンネルマージ時に big endian として取り扱うので
        // ここではバイトスワップせずに be のままバイト単位の計算を行う
        uint8_t *ptr = ptrData + i * width * 2;
        for (int x = 0; x < (width-1)*2; x+=2) {
          ptr[x+2] += ptr[x+0] + (ptr[x+3] + ptr[x+1]) / 256;
          ptr[x+3] += ptr[x+1];
        }
#else
        uint16_t *ptr    = (uint16_t *)(ptrData + i * width * 2);
        uint16_t *ptrEnd = (uint16_t *)(ptrData + (i + 1) * width * 2);
        ptr++;
        while (ptr < ptrEnd) {
          *ptr += *(ptr - 1);
          ptr++;
        }
#endif
      }
    } else if (depth == 32) {
      // 4バイトをバイト毎に並べてあるのでバイト単位でデルタ復元
      uint8_t *ptrData = (uint8_t *)buf;
      for (int i = 0; i < height; i++) {
        uint8_t *ptr    = ptrData + i * width * 4;
        uint8_t *ptrEnd = ptrData + (i + 1) * width * 4;
        ptr++;
        while (ptr < ptrEnd) {
          *ptr = (uint8_t)(*ptr + *(ptr - 1));
          ptr++;
        }
      }

      // バイト単位で復元したものを4バイト値にパック
      int offset1 = width;
      int offset2 = 2 * offset1;
      int offset3 = 3 * offset1;
      for (int i = 0; i < height; i++) {
        uint8_t* dstPtr    = (uint8_t *)dst + i * width * 4;
        uint8_t* dstPtrEnd = (uint8_t *)dst + (i + 1) * width * 4;
        uint8_t* srcPtr    = ptrData + i * width * 4;
        while (dstPtr < dstPtrEnd) {
          *dstPtr++ = *(srcPtr);
          *dstPtr++ = *(srcPtr + offset1);
          *dstPtr++ = *(srcPtr + offset2);
          *dstPtr++ = *(srcPtr + offset3);
          srcPtr++;
        }
      }
      delete [] buf;
    }
    return true;
  }
#endif // USE_ZLIB

  // --------------------------------------------------------------------------
  // 画像取得
  // --------------------------------------------------------------------------
  // レイヤー画像を取得
  bool PSDFile::getLayerImageById(int layerId, void *buf, const ColorFormat &format,
                                  int bufPitchByte, ImageMode mode)
  {
    LayerInfo *layer = getLayerById(layerId);
    if (layer) {
      return getLayerImage(*layer, buf, format, bufPitchByte, mode);
    } else {
      return false;
    }
  }

  // 圧縮データ encodedLen バイトから復元できるバイト数の上限の目安。壊れたファイルが
  // 宣言する巨大な寸法を、確保する前に弾くために使う。
  //   raw: そのまま / RLE (PackBits): 2 バイトで最大 128 バイト / ZIP: zlib の最大伸長率
  static int64_t maxDecodedBytes(int compression, int64_t encodedLen) {
    if (encodedLen < 0) return 0;
    switch (compression) {
    case 0:  return encodedLen;
    case 1:  return encodedLen * 64;
    case 2:
    case 3:  return encodedLen * 1032 + 1024;
    default: return 0;
    }
  }

  static int64_t bytesPerPlane(int width, int height, int depth) {
    if (width <= 0 || height <= 0) return 0;
    if (depth == 1) return (int64_t)((width + 7) / 8) * height;
    return (int64_t)width * height * (depth / 8);
  }

  bool PSDFile::canDecodeLayerImage(const LayerInfo &layer, ImageMode mode)
  {
    const LayerMask &mask = layer.extraData.layerMask;
    const int w = (mode == IMAGE_MODE_MASK) ? mask.width : layer.width;
    const int h = (mode == IMAGE_MODE_MASK) ? mask.height : layer.height;
    if (w <= 0 || h <= 0 || w > 300000 || h > 300000) return false;
    if (header.depth != 1 && header.depth != 8 && header.depth != 16 && header.depth != 32) return false;
    for (const auto &ch : layer.channels) {
      if (!ch.imageData) continue;
      const bool isMask = ch.isMaskChannel();
      if (mode == IMAGE_MODE_IMAGE && isMask) continue;
      if (mode == IMAGE_MODE_MASK && !isMask) continue;
      const int64_t need = isMask ? bytesPerPlane(mask.width, mask.height, header.depth)
                                  : bytesPerPlane(layer.width, layer.height, header.depth);
      if (need == 0) continue;
      IteratorBase *r = ch.imageData->clone();
      r->init();
      const int avail = r->rest();
      const int comp = avail >= 2 ? r->getInt16() : -1;
      delete r;
      const int64_t len = std::min<int64_t>((int64_t)ch.length - 2, (int64_t)avail - 2);
      if (need > maxDecodedBytes(comp, len)) return false;
    }
    return true;
  }

  bool PSDFile::canDecodeMergedImage()
  {
    if (!imageData) return false;
    if (header.width <= 0 || header.height <= 0 ||
        header.width > 300000 || header.height > 300000 ||
        header.channels <= 0 || header.channels > 56) return false;
    const int64_t need = bytesPerPlane(header.width, header.height, header.depth) * header.channels;
    if (need == 0) return false;
    IteratorBase *r = imageData->clone();
    r->init();
    const int avail = r->rest();
    const int comp = avail >= 2 ? r->getInt16() : -1;
    delete r;
    return need <= maxDecodedBytes(comp, (int64_t)avail - 2);
  }

  // レイヤー画像を取得
  bool PSDFile::getLayerImage(const LayerInfo &layer, void *buf, const ColorFormat &format,
                              int bufPitchByte, ImageMode mode)
  {
    if (!canDecodeLayerImage(layer, mode)) return false;
    const psd::LayerMask &mask  = layer.extraData.layerMask;

    int imageWidth  = layer.width;
    int imageHeight = layer.height;
    int imagePixels = imageWidth * imageHeight;
    int imageChannelBytes = 0;
    
    int maskWidth   = mask.width;
    int maskHeight  = mask.height;
    // 壊れた矩形 (負 / 巨大) では展開しない。マスクだけ壊れているならマスク無し扱い。
    auto sane = [](int w, int h) {
      return w > 0 && h > 0 && w <= 300000 && h <= 300000 && (int64_t)w * h <= (1LL << 28);
    };
    if (!sane(maskWidth, maskHeight)) { maskWidth = 0; maskHeight = 0; }
    if (mode != IMAGE_MODE_MASK && !sane(imageWidth, imageHeight)) return false;
    if (mode == IMAGE_MODE_MASK && maskWidth == 0) return false;
    int maskPixels  = maskWidth * maskHeight;
    int maskChannelBytes  = 0;

    if (bufPitchByte == 0) {
      bufPitchByte = imageWidth * 4;
    }

    switch (header.depth) {
    case 1:
      imageChannelBytes = (imageWidth + 7) / 8 * imageHeight;
      maskChannelBytes  = 0;
      break;
    case 8:
      imageChannelBytes = imagePixels;
      maskChannelBytes  = maskPixels;
      break;
    case 16:
      imageChannelBytes = imagePixels * 2;
      maskChannelBytes  = maskPixels * 2;
      break;
    case 32:
      imageChannelBytes = imagePixels * 4;
      maskChannelBytes  = maskPixels * 4;
      break;
    default:
      return false;
      break;
    }

    uint8_t *tmpSourceBuffer = 0;
    int tmpSourceBufferSize  = 0;
    int channels = (int)layer.channels.size();
    std::vector<uint8_t*> decodedChannels; // (channels);
    std::vector<int>      channelIds; // (channels);

    dprint("layer: %s (%d ch)\n", layer.extraData.layerName.c_str(), channels);
    dprint(" image: (%d x %d) %d pixels, %d bytes/ch\n", imageWidth, imageHeight, imagePixels, imageChannelBytes);
    dprint(" mask:  (%d x %d) %d pixels, %d bytes/ch\n", maskWidth, maskHeight, maskPixels, maskChannelBytes);

    int alphaChannelIndex = -1;
    int maskChannelIndex = -1;
    for (int i = 0; i < channels; i ++)	{
      const ChannelInfo &channel = layer.channels[i];
      if (channel.imageData == 0) {
        continue;
      }

      // 分離取得モードでは、それぞれ不要なチャネルは処理しない
      if ((mode == IMAGE_MODE_IMAGE && channel.isMaskChannel()) ||
          (mode == IMAGE_MODE_MASK  && !channel.isMaskChannel())) {
        continue;
      }

      // ソースチャネルバッファの準備
      channel.imageData->init();
      int compressionId = channel.imageData->getInt16();
      int dataLength    = channel.length - 2; // 2: 頭についてる compress id 分減らす
      // 宣言長は実際に残っているデータ量までに抑える (壊れた長さで巨大な確保をしない)
      if (dataLength > channel.imageData->rest()) dataLength = channel.imageData->rest();
      if (dataLength < 0) dataLength = 0;
      if (compressionId != 0) {
        // 圧縮の場合は一時ソースバッファにコピーしておく
        if (dataLength > tmpSourceBufferSize) {
          delete[] tmpSourceBuffer;
          tmpSourceBuffer     = new uint8_t[dataLength];
          tmpSourceBufferSize = dataLength;
        }
        channel.imageData->getData(tmpSourceBuffer, dataLength);
      }
      
      // 展開先チャネルバッファ&チャネルidのセット
      int bufSize = channel.isMaskChannel() ? maskChannelBytes : imageChannelBytes;
      uint8_t *decodedChannel = new uint8_t[bufSize]();   // 展開が途中で切れても未初期化を見せない
      uint8_t channelId       = channel.id;

      int width = channel.isMaskChannel() ? maskWidth : imageWidth;
      int height = channel.isMaskChannel() ? maskHeight : imageHeight;
      
      // チャネルデータ展開
      switch (compressionId) {
      case 0: // raw
        channel.imageData->getData(decodedChannel, bufSize);
        break;
      case 1: // RLE(PackBits)
        decodePackBits(decodedChannel, (size_t)bufSize,
                       tmpSourceBuffer, (size_t)(dataLength > 0 ? dataLength : 0),
                       height, 1, 0, 0, rowCountBytes());
        break;
      case 2:	// zip (w/o prediction)
#ifdef USE_ZLIB
        decodeZipWithoutPrediction(decodedChannel, bufSize,
                                   tmpSourceBuffer, dataLength);
#else
        memset(decodedChannel, 0xff, bufSize);
#endif
        break;        
      case 3:	// zip (w/ prediction)
#ifdef USE_ZLIB
        decodeZipWithPrediction(decodedChannel, bufSize,
                                tmpSourceBuffer, dataLength,
                                width, height, header.depth);
#else
        memset(decodedChannel, 0xff, bufSize);
#endif
        break;
      default:
        memset(decodedChannel, 0xff, bufSize);
        break;
      }

      // TODO real user mask と user mask が同時に入ってるケース
      switch (channel.id) {
      // decodedChannels 上の位置で覚える (飛ばしたチャンネルがあるとずれるので i ではない)
      case CH_ID_TRANSP:     alphaChannelIndex = (int)decodedChannels.size(); break;
      case CH_ID_UMASK:      maskChannelIndex  = (int)decodedChannels.size(); break;
      case CH_ID_REAL_UMASK: maskChannelIndex  = (int)decodedChannels.size(); break;
      default: break;
      }

      decodedChannels.push_back(decodedChannel);
      channelIds.push_back(channel.id);
    }
    
    if (tmpSourceBuffer) {
      delete[] tmpSourceBuffer;
    }

    // 色チャンネルが欠けている (壊れたファイル) と合成で null を参照するので、
    // 足りない分を 0 で埋めたチャンネルとして補う。
    if (mode != IMAGE_MODE_MASK) {
      int need = 0;
      switch (header.mode) {
      case COLOR_MODE_RGB: case COLOR_MODE_LAB: need = 3; break;
      case COLOR_MODE_CMYK:                     need = 4; break;
      default:                                  need = 1; break;
      }
      for (int id = 0; id < need; id++) {
        bool have = false;
        for (int cid : channelIds) if (cid == id) have = true;
        if (have) continue;
        decodedChannels.push_back(new uint8_t[imageChannelBytes > 0 ? imageChannelBytes : 1]());
        channelIds.push_back(id);
      }
    }

    if (mode == IMAGE_MODE_MASK && maskChannelIndex < 0) {
      return false;
    }

    // イメージ取得モードに合わせてデータを調整
    int colorMode = header.mode;
    switch (mode) {
    case IMAGE_MODE_IMAGE:
      // イメージのみ。とくに調整は不要
      break;
    case IMAGE_MODE_MASKEDIMAGE:      
      // マスクチャネルをアルファチャネルに繰り込む
      if (maskChannelIndex >= 0 &&
          alphaChannelIndex >= 0) {
        uint8_t *maskChannel  = decodedChannels[maskChannelIndex];
        uint8_t *alphaChannel = decodedChannels[alphaChannelIndex];
        switch (header.depth) {
        case 8:
          mergeMaskToAlpha<uint8_t>(
            alphaChannel, layer.left, layer.top, layer.right, layer.bottom,
            maskChannel, mask.left, mask.top, mask.right, mask.bottom, mask.defaultColor);
          break;
        case 16:
          mergeMaskToAlpha<uint16_t>(
            alphaChannel, layer.left, layer.top, layer.right, layer.bottom,
            maskChannel, mask.left, mask.top, mask.right, mask.bottom, mask.defaultColor);
          break;
        case 32:
          mergeMaskToAlpha<uint32_t>(
            alphaChannel, layer.left, layer.top, layer.right, layer.bottom,
            maskChannel, mask.left, mask.top, mask.right, mask.bottom, mask.defaultColor);
          break;
        default:
          break;
        }
      }
      break;
    case IMAGE_MODE_MASK:
      // maskモードのときはグレー画像にフェイクする
      colorMode     = COLOR_MODE_GRAYSCALE;
      imageWidth    = maskWidth;
      imageHeight   = maskHeight;
      channelIds[0] = CH_ID_GRAY;
      break;
    default:
      break;
    }

    // チャネルデータをピクセルデータにマージ
    switch(colorMode) {
    case COLOR_MODE_BITMAP:
      mergeChannelsBitmap(buf, imageWidth, imageHeight,
                          decodedChannels[0], format, bufPitchByte);
      break;
    case COLOR_MODE_GRAYSCALE:
    case COLOR_MODE_DUOTONE:   // Duotone は grayscale として格納される (Adobe 仕様)
      switch (header.depth) {
      case 8:
        mergeChannelsGray<uint8_t>(buf, imageWidth, imageHeight, channelIds,
                                   decodedChannels, format, bufPitchByte);
        break;
      case 16:
        mergeChannelsGray<uint16_t>(buf, imageWidth, imageHeight, channelIds,
                                   decodedChannels, format, bufPitchByte);
        break;
      case 32:
        mergeChannelsGray<uint32_t>(buf, imageWidth, imageHeight, channelIds,
                                   decodedChannels, format, bufPitchByte);
        break;
      default:
        break;
      }
      break;
    case COLOR_MODE_RGB:
      switch (header.depth) {
      case 8:
        mergeChannelsRgb<uint8_t>(buf, imageWidth, imageHeight, channelIds,
                                  decodedChannels, format, bufPitchByte);
        break;
      case 16:
        mergeChannelsRgb<uint16_t>(buf, imageWidth, imageHeight, channelIds,
                                   decodedChannels, format, bufPitchByte);
        break;
      case 32:
        mergeChannelsRgb<uint32_t>(buf, imageWidth, imageHeight, channelIds,
                                   decodedChannels, format, bufPitchByte);
        break;
      default:
        break;
      }
      break;
    case COLOR_MODE_INDEXED:
      mergeChannelsIndex(buf, imageWidth, imageHeight, 
                         decodedChannels[0], colorTable, format, bufPitchByte);
      break;
    case COLOR_MODE_CMYK:
      switch (header.depth) {
      case 8:
        mergeChannelsCmyk<uint8_t>(buf, imageWidth, imageHeight, channelIds,
                                  decodedChannels, format, bufPitchByte);
        break;
      case 16:
        mergeChannelsCmyk<uint16_t>(buf, imageWidth, imageHeight, channelIds,
                                   decodedChannels, format, bufPitchByte);
        break;
      case 32:
        mergeChannelsCmyk<uint32_t>(buf, imageWidth, imageHeight, channelIds,
                                   decodedChannels, format, bufPitchByte);
        break;
      default:
        break;
      }
      break;
    case COLOR_MODE_LAB:
      switch (header.depth) {
      case 8:
        mergeChannelsLab<uint8_t>(buf, imageWidth, imageHeight, channelIds,
                                  decodedChannels, format, bufPitchByte);
        break;
      case 16:
        mergeChannelsLab<uint16_t>(buf, imageWidth, imageHeight, channelIds,
                                   decodedChannels, format, bufPitchByte);
        break;
      default:  // 32bit Lab は存在しない
        break;
      }
      break;
    case COLOR_MODE_MULTICHANNEL:
      // 任意スポットチャネル群で RGB への正準的な変換が無いため未対応
      break;
    default:
      break;
    }
    
    for (int i = 0; i < (int)decodedChannels.size(); i ++)	{
      delete[] decodedChannels[i];
    }

#ifdef ENABLE_BMP_OUTPUT
    char name[256];
    sprintf(name, "layer_%s.bmp", layer.extraData.layerName.c_str());
    saveBmp(buf, imageWidth, imageHeight, imagePixels*4, name);
#endif

    return true;
  }

  // --------------------------------------------------------------------------
  // 画素編集 (E4): PackBits エンコーダ + チャンネル/レイヤ構築
  // --------------------------------------------------------------------------
  namespace {

  // PackBits(RLE) で 1 行を符号化して out に追記する。decodePackBits と対で、
  // ヘッダバイトは 0..127 (リテラル長-1) と 129..255 (連長 257-n) のみを使い、
  // 128 (デコーダが誤動作する値) は決して出さない。
  void packBitsEncodeRow(std::vector<uint8_t> &out, const uint8_t *src, int n) {
    int i = 0;
    while (i < n) {
      int run = 1;
      while (i + run < n && src[i + run] == src[i] && run < 128) run++;
      if (run >= 2) {
        out.push_back((uint8_t)(257 - run));  // 129..255
        out.push_back(src[i]);
        i += run;
      } else {
        int start = i;
        int lit = 0;
        while (i < n && lit < 128) {
          if (i + 1 < n && src[i + 1] == src[i]) break;  // 連長が始まる → リテラル終端
          i++; lit++;
        }
        out.push_back((uint8_t)(lit - 1));   // 0..127
        out.insert(out.end(), src + start, src + start + lit);
      }
    }
  }

  // 行バイト数テーブルの 1 エントリ (countBytes = 2 / PSB は 4) を BE で書く。
  void putRowCount(std::vector<uint8_t> &out, size_t pos, size_t v, int countBytes) {
    for (int i = countBytes - 1; i >= 0; i--, v >>= 8)
      out[pos + (size_t)i] = (uint8_t)(v & 0xff);
  }

  // 1 チャンネル分の RLE バイト列を作る:
  //   [00 01] (compression=1) + [height 個の行バイト数 (BE, PSD は 2 / PSB は 4 バイト)]
  //   + [各行の圧縮データ]
  std::shared_ptr<std::vector<uint8_t>> buildRleChannel(const uint8_t *plane, int w, int h,
                                                        int countBytes = 2) {
    auto buf = std::make_shared<std::vector<uint8_t>>();
    std::vector<uint8_t> &out = *buf;
    out.push_back(0); out.push_back(1);          // compression word = 1 (RLE)
    size_t countPos = out.size();
    out.resize(out.size() + (size_t)h * countBytes, 0);   // 行バイト数テーブル
    for (int y = 0; y < h; y++) {
      size_t before = out.size();
      packBitsEncodeRow(out, plane + (size_t)y * w, w);
      putRowCount(out, countPos + (size_t)y * countBytes, out.size() - before, countBytes);
    }
    return buf;
  }

  void putBE16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)(x & 0xff));
  }
  void putBE32(std::vector<uint8_t> &v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)(x & 0xff));
  }

  // BGRA を 4 チャンネル (-1:A, 0:R, 1:G, 2:B) に分解して RLE 符号化し lay に設定。
  // 既存のマスクチャンネル (-2/-3) は末尾に保持する (画素差し替えでマスクを失わない)。
  void buildLayerChannels(LayerInfo &lay, const uint8_t *bgra, int w, int h,
                          int countBytes) {
    int px = w * h;
    std::vector<uint8_t> R((size_t)px), G((size_t)px), B((size_t)px), A((size_t)px);
    for (int i = 0; i < px; i++) {
      B[i] = bgra[i * 4 + 0]; G[i] = bgra[i * 4 + 1];
      R[i] = bgra[i * 4 + 2]; A[i] = bgra[i * 4 + 3];
    }
    std::vector<ChannelInfo> maskChannels;   // 既存マスクを退避
    for (const auto &c : lay.channels)
      if (c.isMaskChannel()) maskChannels.push_back(c);
    lay.channels.clear();
    const struct { int id; const std::vector<uint8_t> *p; } chs[] = {
      { -1, &A }, { 0, &R }, { 1, &G }, { 2, &B },
    };
    for (const auto &c : chs) {
      auto cbuf = buildRleChannel(c.p->data(), w, h, countBytes);
      ChannelInfo ci(c.id, (int)cbuf->size());
      ci.imageData = new VectorReader(cbuf);   // push_back で clone (buf を共有)
      lay.channels.push_back(ci);
    }
    for (const auto &mc : maskChannels) lay.channels.push_back(mc);
  }

  // 新規レイヤ用の extra data (layer mask=0, blending ranges=0, pascal 名,
  // luni, lyid) を組み立てて生バイトで返す。
  std::shared_ptr<std::vector<uint8_t>> buildLayerExtra(const std::string &nameUtf8,
                                                        int layerId) {
    auto buf = std::make_shared<std::vector<uint8_t>>();
    std::vector<uint8_t> &v = *buf;
    putBE32(v, 0);   // layer mask data length = 0
    putBE32(v, 0);   // blending ranges length = 0
    // pascal 名 (システムバイト列)。UTF-8 バイトをそのまま、255 で切り詰め。
    std::string pn = nameUtf8;
    if (pn.size() > 255) pn.resize(255);
    v.push_back((uint8_t)pn.size());
    v.insert(v.end(), pn.begin(), pn.end());
    int total = 1 + (int)pn.size();
    int pad = (4 - (total & 3)) & 3;             // 名前は 4 バイト境界へ
    for (int i = 0; i < pad; i++) v.push_back(0);
    // luni (Unicode 名)
    {
      u16str u = utf8ToU16(nameUtf8);
      std::vector<uint8_t> d;
      putBE32(d, (uint32_t)u.size());
      for (char16_t ch : u) putBE16(d, (uint16_t)ch);   // charCount(4)+2n → 常に偶数
      const char sig[8] = { '8','B','I','M','l','u','n','i' };
      v.insert(v.end(), sig, sig + 8);
      putBE32(v, (uint32_t)d.size());
      v.insert(v.end(), d.begin(), d.end());
    }
    // lyid (Layer ID)
    {
      const char sig[8] = { '8','B','I','M','l','y','i','d' };
      v.insert(v.end(), sig, sig + 8);
      putBE32(v, 4);
      putBE32(v, (uint32_t)layerId);
    }
    return buf;
  }

  // フォルダの区切り / フォルダ本体用の extra data。
  // buildLayerExtra と同じ構成に 'lsct' (section divider setting) を足したもの。
  //   lsctType : 1=開いたフォルダ / 2=閉じたフォルダ / 3=区切り
  // フォルダ側は blend key を含めて 12 バイトで書く (通過 'pass' の指定に必要)。
  std::shared_ptr<std::vector<uint8_t>> buildFolderExtra(const std::string &nameUtf8,
                                                         int layerId, int lsctType,
                                                         int blendModeKey) {
    auto buf = buildLayerExtra(nameUtf8, layerId);
    std::vector<uint8_t> &v = *buf;
    const char sig[8] = { '8','B','I','M','l','s','c','t' };
    v.insert(v.end(), sig, sig + 8);
    if (lsctType == 3) {
      putBE32(v, 4);
      putBE32(v, (uint32_t)lsctType);
    } else {
      putBE32(v, 12);
      putBE32(v, (uint32_t)lsctType);
      const char bim[4] = { '8','B','I','M' };
      v.insert(v.end(), bim, bim + 4);
      putBE32(v, (uint32_t)blendModeKey);
    }
    return buf;
  }

  }  // anonymous namespace

  int PSDFile::addFolder(const char *nameUtf8, int from, int count,
                         bool closed, int blendModeKey, int opacity) {
    const int n = (int)layerList.size();
    if (from < 0 || count < 0 || from + count > n) return -1;
    if (header.depth != 8 || header.mode != COLOR_MODE_RGB) return -1;

    int maxId = 0;
    for (const auto &l : layerList) if (l.layerId > maxId) maxId = l.layerId;

    // 矩形が空のダミーレイヤを作る。チャンネルは 0 行の RLE (2 バイト) になる。
    auto makeMarker = [&](const char *name, int lsctType, int blendKey,
                          int opa, LayerType type, int id) {
      LayerInfo lay;
      lay.owner = this;
      lay.parent = nullptr;
      lay.parentIndex = -1;
      lay.top = lay.left = lay.bottom = lay.right = 0;
      lay.width = lay.height = 0;
      lay.blendModeKey = blendKey;
      lay.blendMode = blendKeyToMode(blendKey);
      lay.opacity = opa & 0xff;
      lay.clipping = 0;
      lay.flag = 0;
      lay.fill_opacity = 255;
      lay.layerType = type;
      lay.layerId = id;
      lay.layerName = name ? name : "";
      lay.layerNameUnicode = utf8ToU16(lay.layerName);
      lay.extraData.layerName = lay.layerName;
      const uint8_t dummy = 0;
      buildLayerChannels(lay, &dummy, 0, 0, rowCountBytes());   // 幅高さ 0 → 各チャンネル 2 バイト
      lay.extraData.rawBytes =
        new VectorReader(buildFolderExtra(lay.layerName, lay.layerId,
                                          lsctType, blendKey));
      return lay;
    };

    // 中身の下に区切り、上にフォルダ本体。layerList は index 0 が最下層。
    LayerInfo divider = makeMarker("</Layer group>", 3, 'norm', 255,
                                   LAYER_TYPE_HIDDEN, maxId + 1);
    LayerInfo folder  = makeMarker(nameUtf8, closed ? 2 : 1, blendModeKey, opacity,
                                   LAYER_TYPE_FOLDER, maxId + 2);

    layerList.insert(layerList.begin() + from, divider);
    const int folderPos = from + count + 1;
    layerList.insert(layerList.begin() + folderPos, folder);
    layersDirty = true;
    relinkGroups();          // 並びが変わったので親子関係を貼り直す
    return folderPos;
  }

  bool PSDFile::setLayerPixels(int index, const uint8_t *bgra, int width, int height) {
    if (index < 0 || index >= (int)layerList.size()) return false;
    if (!bgra || width <= 0 || height <= 0) return false;
    if (header.depth != 8 || header.mode != COLOR_MODE_RGB) return false;
    LayerInfo &lay = layerList[(size_t)index];
    buildLayerChannels(lay, bgra, width, height, rowCountBytes());
    lay.right  = lay.left + width;
    lay.bottom = lay.top  + height;
    lay.width  = width;
    lay.height = height;
    layersDirty = true;
    return true;
  }

  int PSDFile::addLayer(const char *nameUtf8, int left, int top,
                        const uint8_t *bgra, int width, int height,
                        int blendModeKey, int opacity, int destIndex) {
    if (!bgra || width <= 0 || height <= 0) return -1;
    if (header.depth != 8 || header.mode != COLOR_MODE_RGB) return -1;
    LayerInfo lay;
    lay.owner = this;
    lay.parent = nullptr;
    lay.parentIndex = -1;
    lay.top = top; lay.left = left;
    lay.bottom = top + height; lay.right = left + width;
    lay.width = width; lay.height = height;
    lay.blendModeKey = blendModeKey;
    lay.blendMode = blendKeyToMode(blendModeKey);
    lay.opacity = opacity & 0xff;
    lay.clipping = 0;
    lay.flag = 0;
    lay.fill_opacity = 255;
    lay.layerType = LAYER_TYPE_NORMAL;
    int maxId = 0;
    for (const auto &l : layerList) if (l.layerId > maxId) maxId = l.layerId;
    lay.layerId = maxId + 1;
    lay.layerName = nameUtf8 ? nameUtf8 : "";
    lay.layerNameUnicode = utf8ToU16(lay.layerName);
    lay.extraData.layerName = lay.layerName;
    buildLayerChannels(lay, bgra, width, height, rowCountBytes());
    auto extra = buildLayerExtra(lay.layerName, lay.layerId);
    lay.extraData.rawBytes = new VectorReader(extra);
    int pos = (destIndex < 0 || destIndex > (int)layerList.size())
                ? (int)layerList.size() : destIndex;
    layerList.insert(layerList.begin() + pos, lay);
    layersDirty = true;
    relinkGroups();          // 並びが変わったので親子関係を貼り直す
    return pos;
  }

  bool PSDFile::setMergedImage(const uint8_t *bgra, int width, int height) {
    if (!bgra || width <= 0 || height <= 0) return false;
    if (header.depth != 8 || header.mode != COLOR_MODE_RGB) return false;
    if (width != header.width || height != header.height) return false;
    int nch = header.channels;
    if (nch < 3) nch = 3;
    if (nch > 4) nch = 4;   // RGB(3) / RGBA(4) のみ
    int px = width * height;
    auto buf = std::make_shared<std::vector<uint8_t>>();
    buf->reserve((size_t)2 + (size_t)px * nch);
    buf->push_back(0); buf->push_back(0);   // compression = 0 (raw)
    // merged プレーンは R,G,B(,A) 順。BGRA からその順で取り出す。
    const int comp[4] = { 2, 1, 0, 3 };     // R,G,B,A の BGRA インデックス
    for (int c = 0; c < nch; c++) {
      int s = comp[c];
      for (int i = 0; i < px; i++) buf->push_back(bgra[(size_t)i * 4 + s]);
    }
    delete imageData;
    imageData = new VectorReader(buf);
    return true;
  }

  bool PSDFile::setMergedImageSolid(uint8_t r, uint8_t g, uint8_t b) {
    if (header.depth != 8 || header.mode != COLOR_MODE_RGB) return false;
    const int w = header.width, h = header.height;
    if (w <= 0 || h <= 0) return false;
    int nch = header.channels;
    if (nch < 3) nch = 3;
    if (nch > 4) nch = 4;
    const uint8_t val[4] = { r, g, b, 255 };

    // merged セクションの RLE は、レイヤのチャンネルと並びが違う:
    // 圧縮ワードのあと「全チャンネルぶんの行バイト数テーブル」をまとめて置き、
    // その後ろに行データが続く。
    auto buf = std::make_shared<std::vector<uint8_t>>();
    buf->push_back(0); buf->push_back(1);                 // compression = 1 (RLE)
    const size_t countPos = buf->size();
    const int cb = rowCountBytes();
    buf->resize(buf->size() + (size_t)h * nch * cb, 0);

    std::vector<uint8_t> row((size_t)w), enc;
    size_t line = 0;
    for (int c = 0; c < nch; c++) {
      // 一様な行なので 1 回だけ符号化して使い回す。
      std::fill(row.begin(), row.end(), val[c]);
      enc.clear();
      packBitsEncodeRow(enc, row.data(), w);
      for (int y = 0; y < h; y++) {
        buf->insert(buf->end(), enc.begin(), enc.end());
        putRowCount(*buf, countPos + line * cb, enc.size(), cb);
        line++;
      }
    }
    delete imageData;
    imageData = new VectorReader(buf);
    return true;
  }

  bool PSDFile::setLayerMaskPixels(int index, const uint8_t *gray,
                                   int top, int left, int width, int height) {
    if (index < 0 || index >= (int)layerList.size()) return false;
    if (!gray || width <= 0 || height <= 0) return false;
    if (header.depth != 8) return false;   // 8bit のみ
    LayerInfo &lay = layerList[(size_t)index];
    // 既存のマスクチャンネル (-2/-3) を除いて再構成し、新しい user mask (-2) を末尾へ。
    std::vector<ChannelInfo> kept;
    for (const auto &c : lay.channels)
      if (!c.isMaskChannel()) kept.push_back(c);
    lay.channels.swap(kept);
    auto cbuf = buildRleChannel(gray, width, height, rowCountBytes());
    ChannelInfo mc(CH_ID_UMASK, (int)cbuf->size());   // -2
    mc.imageData = new VectorReader(cbuf);
    lay.channels.push_back(mc);
    // マスク幾何を更新 (無ければ新規作成)
    LayerMask &m = lay.extraData.layerMask;
    if (!m.present) {
      m = LayerMask();
      m.present = true;
      m.defaultColor = 0;   // 矩形外は隠す
      m.flags = 0;
    }
    m.hasReal = false;      // 単純な user mask
    m.top = top; m.left = left; m.bottom = top + height; m.right = left + width;
    m.width = width; m.height = height;
    m.edited = true;
    lay.extraData.useRawBytes = false;
    layersDirty = true;
    return true;
  }

  bool PSDFile::setLayerName(int index, const char *nameUtf8) {
    if (index < 0 || index >= (int)layerList.size()) return false;
    LayerInfo &lay = layerList[(size_t)index];
    std::string s = nameUtf8 ? nameUtf8 : "";
    lay.layerName            = s;
    lay.layerNameUnicode     = utf8ToU16(s);
    lay.extraData.layerName  = s;
    lay.extraData.useRawBytes = false;   // save 時にフィールドから再構築
    return true;
  }

  // 合成画像の全チャンネルを展開する。各プレーンは header.depth の生サンプル
  // (16/32bit は big-endian のまま)。色チャンネルの後ろに、透明度 / アルファ /
  // スポットチャンネルが header.channels まで続く。
  bool PSDFile::decodeMergedPlanes(std::vector<std::vector<uint8_t>> &planes)
  {
    planes.clear();
    if (!canDecodeMergedImage()) return false;
    const int imageWidth  = header.width;
    const int imageHeight = header.height;
    const int imagePixels = imageWidth * imageHeight;
    int imageChannelBytes = 0;
    switch (header.depth) {
    case 1:  imageChannelBytes = (imageWidth + 7) / 8 * imageHeight; break;
    case 8:  imageChannelBytes = imagePixels;                        break;
    case 16: imageChannelBytes = imagePixels * 2;                    break;
    case 32: imageChannelBytes = imagePixels * 4;                    break;
    default: return false;
    }
    // ソースチャネルバッファの準備
    imageData->init();
    uint8_t *tmpSourceBuffer = 0;
    int compressionId = imageData->getInt16();
    int dataLength    = imageData->rest();
    if (compressionId != 0) {
      tmpSourceBuffer = new uint8_t[dataLength];
      imageData->getData(tmpSourceBuffer, dataLength);
    }

    // 展開先チャネルバッファの準備
    int channels = header.channels;
    planes.assign((size_t)channels, std::vector<uint8_t>((size_t)imageChannelBytes, 0));
    std::vector<uint8_t*> decodedChannels(channels);
    for (int i = 0; i < channels; i ++) decodedChannels[i] = planes[(size_t)i].data();
    
    // チャネルデータ展開
    switch (compressionId) {
    case 0: // raw
      {
        for (int i = 0; i < channels; i ++)	{
          imageData->getData(decodedChannels[i], imageChannelBytes);
        }
      }
      break;
    case 1: // RLE(PackBits)
      {
        uint32_t nextOffset = 0;
        for (int i = 0; i < channels; i ++)	{
          nextOffset += decodePackBits(decodedChannels[i], (size_t)imageChannelBytes,
                                       tmpSourceBuffer, (size_t)dataLength,
                                       imageHeight, channels, i, nextOffset,
                                       rowCountBytes());
        }
      }
      break;
    case 2:	// zip (w/o prediction)
    case 3:	// zip (w/ prediction)
      {
        // 合成画像の ZIP は全チャンネルぶんで 1 本の zlib ストリーム。まとめて
        // 展開してからチャンネルへ分ける (以前はチャンネルごとにストリームの
        // 先頭から展開していて、2 枚目以降が 1 枚目と同じになっていた)。
        // 予測 (差分) は行単位なので、チャンネルを縦に積んだ 1 枚の画像とみなせる。
#ifdef USE_ZLIB
        std::vector<uint8_t> all((size_t)imageChannelBytes * channels);
        bool zok = (compressionId == 2)
          ? decodeZipWithoutPrediction(all.data(), (int)all.size(), tmpSourceBuffer, dataLength)
          : decodeZipWithPrediction(all.data(), (int)all.size(), tmpSourceBuffer, dataLength,
                                    imageWidth, imageHeight * channels, header.depth);
        if (zok) {
          for (int i = 0; i < channels; i ++)
            memcpy(decodedChannels[i], all.data() + (size_t)imageChannelBytes * i, imageChannelBytes);
        }
#endif
      }
      break;
    default:
      break;
    }

    if (tmpSourceBuffer) {
      delete[] tmpSourceBuffer;
    }
    return true;
  }

  bool PSDFile::getMergedImage(void *buf, const ColorFormat &format, int bufPitchByte)
  {
    int imageWidth  = header.width;
    int imageHeight = header.height;
    int imagePixels = imageWidth * imageHeight;
    int imageChannelBytes = 0;
    switch (header.depth) {
    case 1:  imageChannelBytes = (imageWidth + 7) / 8 * imageHeight; break;
    case 8:  imageChannelBytes = imagePixels;                        break;
    case 16: imageChannelBytes = imagePixels * 2;                    break;
    case 32: imageChannelBytes = imagePixels * 4;                    break;
    default:
      return false;
      break;
    }
    if (bufPitchByte == 0) {
      bufPitchByte = imageWidth * 4;
    }

    // チャネルデータ展開 (decodeMergedPlanes)
    std::vector<std::vector<uint8_t>> planes;
    if (!decodeMergedPlanes(planes)) return false;
    int channels = (int)planes.size();
    std::vector<uint8_t*> decodedChannels(channels);
    std::vector<int>      channelIds(channels);
    for (int i = 0; i < channels; i ++)	{
      decodedChannels[i] = planes[(size_t)i].data();
      channelIds[i] = i;
    }

    switch(header.mode) {
    case COLOR_MODE_BITMAP:
      mergeChannelsBitmap(buf, imageWidth, imageHeight,
                          decodedChannels[0], format, bufPitchByte);
      break;
    case COLOR_MODE_GRAYSCALE:
    case COLOR_MODE_DUOTONE:   // Duotone は grayscale として格納される (Adobe 仕様)
      switch (header.depth) {
      case 8:
        mergeChannelsGray<uint8_t>(buf, imageWidth, imageHeight, channelIds,
                                   decodedChannels, format, bufPitchByte);
        break;
      case 16:
        mergeChannelsGray<uint16_t>(buf, imageWidth, imageHeight, channelIds,
                                   decodedChannels, format, bufPitchByte);
        break;
      case 32:
        mergeChannelsGray<uint32_t>(buf, imageWidth, imageHeight, channelIds,
                                   decodedChannels, format, bufPitchByte);
        break;
      default:
        break;
      }
      break;
    case COLOR_MODE_RGB:
      if (channelIds.size() > 3) {
        channelIds[3] = CH_ID_TRANSP;
      }
      switch (header.depth) {
      case 8:
        mergeChannelsRgb<uint8_t>(buf, imageWidth, imageHeight, channelIds,
                                  decodedChannels, format, bufPitchByte);
        break;
      case 16:
        mergeChannelsRgb<uint16_t>(buf, imageWidth, imageHeight, channelIds,
                                   decodedChannels, format, bufPitchByte);
        break;
      case 32:
        mergeChannelsRgb<uint32_t>(buf, imageWidth, imageHeight, channelIds,
                                   decodedChannels, format, bufPitchByte);
        break;
      default:
        break;
      }
      break;
    case COLOR_MODE_INDEXED:
      mergeChannelsIndex(buf, imageWidth, imageHeight, 
                         decodedChannels[0], colorTable, format, bufPitchByte);
      break;
    case COLOR_MODE_CMYK:
      switch (header.depth) {
      case 8:
        mergeChannelsCmyk<uint8_t>(buf, imageWidth, imageHeight, channelIds,
                                  decodedChannels, format, bufPitchByte);
        break;
      case 16:
        mergeChannelsCmyk<uint16_t>(buf, imageWidth, imageHeight, channelIds,
                                   decodedChannels, format, bufPitchByte);
        break;
      case 32:
        mergeChannelsCmyk<uint32_t>(buf, imageWidth, imageHeight, channelIds,
                                   decodedChannels, format, bufPitchByte);
        break;
      default:
        break;
      }
      break;
    case COLOR_MODE_LAB:
      switch (header.depth) {
      case 8:
        mergeChannelsLab<uint8_t>(buf, imageWidth, imageHeight, channelIds,
                                  decodedChannels, format, bufPitchByte);
        break;
      case 16:
        mergeChannelsLab<uint16_t>(buf, imageWidth, imageHeight, channelIds,
                                   decodedChannels, format, bufPitchByte);
        break;
      default:  // 32bit Lab は存在しない
        break;
      }
      break;
    case COLOR_MODE_MULTICHANNEL:
      // 任意スポットチャネル群で RGB への正準的な変換が無いため未対応
      break;
    default:
      break;
    }

#ifdef ENABLE_BMP_OUTPUT
    saveBmp(buf, imageWidth, imageHeight, imagePixels*4, "merged.bmp");
#endif

    return true;
  }

} // namespace psd
