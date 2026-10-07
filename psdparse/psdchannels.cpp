// アルファ / スポットチャンネル: 合成画像の色チャンネルの後ろに続く余分な
// チャンネルの名前・表示設定と、画素の取り出し。
//
// 公開仕様 "Image Resource IDs":
//   1006  アルファチャンネル名 (Pascal 文字列の並び)
//   1045  アルファチャンネル名 (Unicode 文字列の並び。1006 より優先)
//   1077  DisplayInfo: version(4) + チャンネルごとに 13 バイト
//         (色空間 2 + 色 4×2 + 不透明度 2 + 種類 1)
// どれも「色チャンネルの後ろの余分なチャンネル」に順に対応する。合成画像に
// 透明度がある文書では、その透明度も 1 件として数えられる ("Transparency")。
#include "psdparse.h"
#include "psdfile.h"
#include "psdresource.h"

#include <vector>

namespace psd {

namespace {

bool readResource(Data &data, int id, std::vector<uint8_t> &out) {
  for (auto &res : data.imageResourceList) {
    if (res.id != id || !res.data) continue;
    IteratorBase *r = res.data->clone();
    r->init();
    int n = r->size();
    out.resize(n > 0 ? (size_t)n : 0);
    int got = n > 0 ? r->getData(out.data(), n) : 0;
    delete r;
    return got == n;
  }
  return false;
}

uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

}  // anonymous namespace

int colorChannelCount(int mode) {
  switch (mode) {
  case COLOR_MODE_RGB:  case COLOR_MODE_LAB: return 3;
  case COLOR_MODE_CMYK:                      return 4;
  case COLOR_MODE_MULTICHANNEL:              return 0;   // 全部がインキ (スポット) チャンネル
  default:                                   return 1;   // bitmap / gray / indexed / duotone
  }
}

void loadAlphaChannels(Data &data) {
  data.alphaChannels.clear();
  const int first = colorChannelCount(data.header.mode);
  const int extra = data.header.channels - first;
  if (extra <= 0) return;
  data.alphaChannels.resize((size_t)extra);
  for (int i = 0; i < extra; i++) data.alphaChannels[(size_t)i].plane = first + i;

  std::vector<uint8_t> b;
  if (readResource(data, 1045, b)) {
    size_t p = 0;
    for (int i = 0; i < extra && p + 4 <= b.size(); i++) {
      uint32_t n = be32(&b[p]);
      p += 4;
      if (p + (size_t)n * 2 > b.size()) break;
      u16str s;
      for (uint32_t k = 0; k < n; k++) s.push_back((char16_t)((b[p + k * 2] << 8) | b[p + k * 2 + 1]));
      p += (size_t)n * 2;
      while (!s.empty() && s.back() == 0) s.pop_back();
      data.alphaChannels[(size_t)i].name = s;
    }
  } else if (readResource(data, 1006, b)) {
    size_t p = 0;
    for (int i = 0; i < extra && p < b.size(); i++) {
      size_t n = b[p++];
      if (p + n > b.size()) break;
      // Pascal 名はシステムの文字コード。ASCII の範囲だけそのまま写す
      u16str s;
      for (size_t k = 0; k < n; k++) s.push_back((char16_t)b[p + k]);
      data.alphaChannels[(size_t)i].name = s;
      data.alphaChannels[(size_t)i].nameRaw.assign((const char *)&b[p], n);
      p += n;
    }
  }
  if (readResource(data, 1077, b) && b.size() >= 4) {
    size_t p = 4;
    for (int i = 0; i < extra && p + 13 <= b.size(); i++, p += 13) {
      AlphaChannelInfo &a = data.alphaChannels[(size_t)i];
      a.hasDisplay = true;
      a.colorSpace = (b[p] << 8) | b[p + 1];
      for (int k = 0; k < 4; k++) a.color[k] = (b[p + 2 + k * 2] << 8) | b[p + 3 + k * 2];
      a.opacity = (b[p + 10] << 8) | b[p + 11];
      a.kind = b[p + 12];
    }
  }
}

bool PSDFile::getMergedChannel(int plane, std::vector<uint8_t> &out) {
  if (plane < 0 || plane >= header.channels) return false;
  std::vector<std::vector<uint8_t>> planes;
  if (!decodeMergedPlanes(planes) || plane >= (int)planes.size()) return false;
  const std::vector<uint8_t> &src = planes[(size_t)plane];
  const size_t n = (size_t)header.width * header.height;
  out.assign(n, 0);
  switch (header.depth) {
  case 8:
    for (size_t i = 0; i < n && i < src.size(); i++) out[i] = src[i];
    break;
  case 16:
    for (size_t i = 0; i < n && i * 2 < src.size(); i++) out[i] = src[i * 2];
    break;
  case 32:
    for (size_t i = 0; i < n && i * 4 + 3 < src.size(); i++) {
      pun32 v;
      v.i = be32(&src[i * 4]);
      out[i] = !(v.f > 0.0f) ? 0 : v.f >= 1.0f ? 255 : (uint8_t)(v.f * 255.0f + 0.5f);
    }
    break;
  case 1:   // 1bit: 1 が黒
    for (size_t i = 0; i < n; i++) {
      size_t x = i % (size_t)header.width, y = i / (size_t)header.width;
      size_t byte = y * (((size_t)header.width + 7) / 8) + x / 8;
      if (byte < src.size()) out[i] = (src[byte] & (0x80 >> (x & 7))) ? 0 : 255;
    }
    break;
  default:
    return false;
  }
  return true;
}

}  // namespace psd
