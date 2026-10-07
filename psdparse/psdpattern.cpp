// パターン: 文書末尾の 'Patt' / 'Pat2' / 'Pat3' (8 / 16 / 32bit 文書)。
//
// 公開仕様 "Patterns" と "Virtual Memory Array List" より:
//   ブロック  := (長さ(4) + パターン + 4 の倍数への詰め物) の並び
//   パターン  := version(4)=1 + 画像モード(4) + 高さ(2) + 幅(2) + 名前 (Unicode)
//              + ID (Pascal, 詰め物なし) + [Indexed: 256×RGB + 4] + VMA リスト
//   VMA リスト := version(4)=3 + 長さ(4) + 矩形 (上, 左, 下, 右) + チャンネル数(4)
//              + (チャンネル数 + 2) 個の配列
//   配列      := 書き込み済み(4) [0 なら以降なし] + 長さ(4) [0 なら以降なし]
//              + 深度(4) + 矩形(16) + 深度(2) + 圧縮(1: 0 raw / 1 RLE) + データ
// Photoshop はチャンネル数を 24 と書き、実在するチャンネルだけを書き込み済みに
// する。書き込み済みの配列のうち先頭の (モードの色数) 本を色、その次を透明度
// として扱う。
#include "psdparse.h"
#include "psdfile.h"
#include "psdresource.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace psd {

namespace {

struct Rd {
  const std::vector<uint8_t> &b;
  size_t p = 0;
  bool ok = true;
  explicit Rd(const std::vector<uint8_t> &b, size_t start = 0) : b(b), p(start) {}
  bool need(size_t n) { if (p + n > b.size()) ok = false; return ok; }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = ((uint32_t)b[p] << 24) | ((uint32_t)b[p + 1] << 16) | ((uint32_t)b[p + 2] << 8) | b[p + 3];
    p += 4; return v;
  }
  int u16() { if (!need(2)) return 0; int v = (b[p] << 8) | b[p + 1]; p += 2; return v; }
  int u8() { if (!need(1)) return 0; return b[p++]; }
  void skip(size_t n) { if (need(n)) p += n; }
};

bool readTrailingRange(Data &data, int offset, int length, std::vector<uint8_t> &out) {
  if (!data.layerAndMaskTrailing || length < 0) return false;
  out.resize((size_t)length);
  if (length == 0) return true;
  data.layerAndMaskTrailing->init();
  IteratorBase *r = data.layerAndMaskTrailing->cloneRange(offset, length);
  int got = r ? r->getData(out.data(), length) : 0;
  delete r;
  return got == length;
}

int modeColorChannels(int mode) {
  switch (mode) {
  case COLOR_MODE_RGB:  return 3;
  case COLOR_MODE_CMYK: return 4;
  case COLOR_MODE_LAB:  return 3;
  default:              return 1;   // gray / indexed / duotone / bitmap
  }
}

// パターン 1 件のヘッダ (VMA リストの手前まで) を読む
bool parsePatternHeader(Rd &r, PatternInfo &pi) {
  if (r.u32() != 1) return false;
  pi.mode   = (int)r.u32();
  pi.height = (int16_t)r.u16();
  pi.width  = (int16_t)r.u16();
  uint32_t n = r.u32();
  if (!r.need((size_t)n * 2)) return false;
  pi.name.clear();
  for (uint32_t i = 0; i < n; i++) pi.name.push_back((char16_t)r.u16());
  while (!pi.name.empty() && pi.name.back() == 0) pi.name.pop_back();
  int idLen = r.u8();
  if (!r.need((size_t)idLen)) return false;
  pi.id.assign((const char *)&r.b[r.p], (size_t)idLen);
  r.p += (size_t)idLen;
  if (pi.mode == COLOR_MODE_INDEXED) {
    if (!r.need(768 + 4)) return false;
    pi.palette.assign(r.b.begin() + (long)r.p, r.b.begin() + (long)r.p + 768);
    r.p += 768 + 4;
  }
  return r.ok;
}

// PackBits を 1 チャンネル分展開する (行バイト数は 2 バイト)
bool unpackRle(const uint8_t *src, size_t n, int rows, size_t rowBytes, std::vector<uint8_t> &out) {
  out.assign((size_t)rows * rowBytes, 0);
  size_t table = (size_t)rows * 2;
  if (n < table) return false;
  size_t in = table;
  for (int y = 0; y < rows; y++) {
    size_t len = ((size_t)src[y * 2] << 8) | src[y * 2 + 1];
    size_t end = std::min(n, in + len);
    size_t o = (size_t)y * rowBytes, oe = o + rowBytes;
    while (in < end && o < oe) {
      int h = src[in++];
      if (h > 128) {
        if (in >= end) break;
        size_t cnt = std::min((size_t)(257 - h), oe - o);
        std::memset(&out[o], src[in++], cnt);
        o += cnt;
      } else if (h < 128) {
        size_t cnt = std::min((size_t)h + 1, end - in);
        size_t w = std::min(cnt, oe - o);
        std::memcpy(&out[o], src + in, w);
        in += cnt; o += w;
      }
    }
    in = end;
  }
  return true;
}

// 8bit の表示値 (16bit は上位バイト、32bit は線形 → sRGB)
uint8_t sample8(const std::vector<uint8_t> &plane, size_t i, int depth) {
  if (depth == 16) return plane[i * 2];
  if (depth == 32) {
    pun32 v;
    v.i = ((uint32_t)plane[i * 4] << 24) | ((uint32_t)plane[i * 4 + 1] << 16) |
          ((uint32_t)plane[i * 4 + 2] << 8) | plane[i * 4 + 3];
    double c = v.f;
    if (!(c > 0.0)) return 0;
    if (c >= 1.0) return 255;
    c = (c <= 0.0031308) ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    return (uint8_t)(c * 255.0 + 0.5);
  }
  return plane[i];
}

// 透明度は線形のまま 8bit へ
uint8_t alpha8(const std::vector<uint8_t> &plane, size_t i, int depth) {
  if (depth == 32) {
    pun32 v;
    v.i = ((uint32_t)plane[i * 4] << 24) | ((uint32_t)plane[i * 4 + 1] << 16) |
          ((uint32_t)plane[i * 4 + 2] << 8) | plane[i * 4 + 3];
    if (!(v.f > 0.0f)) return 0;
    if (v.f >= 1.0f) return 255;
    return (uint8_t)(v.f * 255.0f + 0.5f);
  }
  return sample8(plane, i, depth);
}

}  // anonymous namespace

void loadPatterns(Data &data) {
  data.patterns.clear();
  std::vector<uint8_t> buf;
  for (const auto &b : data.globalBlocks) {
    if (b.key != 'Patt' && b.key != 'Pat2' && b.key != 'Pat3') continue;
    if (!readTrailingRange(data, b.dataOffset, b.dataLength, buf)) continue;
    size_t p = 0;
    while (p + 4 <= buf.size()) {
      Rd r(buf, p);
      uint32_t len = r.u32();
      if (len == 0 || len > buf.size() - r.p) break;
      PatternInfo pi;
      pi.blockKey = b.key;
      pi.offset = b.dataOffset + (int)r.p;
      pi.length = (int)len;
      std::vector<uint8_t> one(buf.begin() + (long)r.p, buf.begin() + (long)(r.p + len));
      Rd pr(one);
      if (parsePatternHeader(pr, pi)) data.patterns.push_back(pi);
      p = r.p + len;
      p += (4 - (len & 3)) & 3;
    }
  }
}

bool PSDFile::getPatternImage(int index, std::vector<uint8_t> &bgra, int &width, int &height) {
  if (index < 0 || index >= (int)patterns.size()) return false;
  const PatternInfo &pi = patterns[(size_t)index];
  std::vector<uint8_t> one;
  if (!readTrailingRange(*this, pi.offset, pi.length, one)) return false;
  Rd r(one);
  PatternInfo tmp;
  if (!parsePatternHeader(r, tmp)) return false;
  // VMA リスト
  if (r.u32() != 3) return false;
  uint32_t listLen = r.u32();
  if (!r.need(listLen)) return false;
  const int top = (int32_t)r.u32(), left = (int32_t)r.u32();
  const int bottom = (int32_t)r.u32(), right = (int32_t)r.u32();
  const int w = right - left, h = bottom - top;
  if (w <= 0 || h <= 0 || w > 30000 || h > 30000) return false;
  const uint32_t nch = r.u32();
  const int ncolor = modeColorChannels(pi.mode);
  std::vector<std::vector<uint8_t>> planes;   // 書き込み済みの配列を順に
  int depth = 8;
  for (uint32_t c = 0; c < nch + 2 && r.ok && c < 64; c++) {
    if (r.u32() == 0) continue;                  // 書き込みなし
    uint32_t len = r.u32();
    if (len == 0) continue;
    if (len < 23 || !r.need(len)) return false;
    size_t start = r.p;
    depth = (int)r.u32();
    r.skip(16);                                  // 配列ごとの矩形
    int pixelDepth = r.u16();
    int compression = r.u8();
    if (pixelDepth == 1 || pixelDepth == 8 || pixelDepth == 16 || pixelDepth == 32) depth = pixelDepth;
    const size_t bpp = depth == 16 ? 2 : depth == 32 ? 4 : 1;
    const size_t rowBytes = (size_t)w * bpp;
    const uint8_t *src = &one[r.p];
    const size_t srcLen = start + len - r.p;
    std::vector<uint8_t> plane;
    if (compression == 0) {
      if (srcLen < rowBytes * h) return false;
      plane.assign(src, src + rowBytes * h);
    } else if (compression == 1) {
      if (!unpackRle(src, srcLen, h, rowBytes, plane)) return false;
    } else {
      return false;                              // 未対応の圧縮
    }
    planes.push_back(plane);
    r.p = start + len;
  }
  if ((int)planes.size() < ncolor) return false;
  const bool hasAlpha = (int)planes.size() > ncolor;
  width = w; height = h;
  bgra.assign((size_t)w * h * 4, 255);
  for (size_t i = 0; i < (size_t)w * h; i++) {
    uint8_t R, G, B;
    if (pi.mode == COLOR_MODE_RGB) {
      R = sample8(planes[0], i, depth); G = sample8(planes[1], i, depth); B = sample8(planes[2], i, depth);
    } else if (pi.mode == COLOR_MODE_CMYK) {
      // 格納はインキ量の反転 (0 = 100%)
      double c = sample8(planes[0], i, depth) / 255.0, m = sample8(planes[1], i, depth) / 255.0;
      double y = sample8(planes[2], i, depth) / 255.0, k = sample8(planes[3], i, depth) / 255.0;
      R = (uint8_t)(c * k * 255.0 + 0.5); G = (uint8_t)(m * k * 255.0 + 0.5); B = (uint8_t)(y * k * 255.0 + 0.5);
    } else if (pi.mode == COLOR_MODE_INDEXED && tmp.palette.size() == 768) {
      uint8_t idx = planes[0][i];
      R = tmp.palette[idx * 3]; G = tmp.palette[idx * 3 + 1]; B = tmp.palette[idx * 3 + 2];
    } else {
      R = G = B = sample8(planes[0], i, depth);   // gray / duotone / Lab (明度のみ)
    }
    bgra[i * 4 + 0] = B; bgra[i * 4 + 1] = G; bgra[i * 4 + 2] = R;
    if (hasAlpha) bgra[i * 4 + 3] = alpha8(planes[(size_t)ncolor], i, depth);
  }
  return true;
}

}  // namespace psd
