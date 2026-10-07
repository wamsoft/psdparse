// 文書の注釈 ('Anno') とレイヤのアートボード ('artb' / 'artd' / 'abdd')。
//
// 'Anno' (公開仕様 "Annotations (Photoshop 6.0)"):
//   major(2)=2 + minor(2)=1 + 件数(4) + 件数 × 注釈
//   注釈 := 長さ(4, 自身を含む) + 種類 ('txtA' テキスト / 'sndA' 音声) + 開閉(1)
//          + フラグ(1) + 追加ブロック数(2) + アイコン矩形(4×4) + ポップアップ矩形(4×4)
//          + 色 (色空間 2 + 4×2) + 作者 / 名前 / 更新日時 (Pascal、偶数長へ詰める)
//          + データ長(4, 自身を含む) + 'txtC' / 'sndM' + 大きさ(4) + データ
//   テキストは BOM 付き UTF-16BE (改行は CR)。
// アートボード: version(4)=16 + descriptor (artboardRect / artboardPresetName /
//   Clr / artboardBackgroundType)。
#include "psdparse.h"
#include "psdlayer.h"
#include "psdresource.h"

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
  std::string bytes(size_t n) {
    if (!need(n)) return std::string();
    std::string s((const char *)&b[p], n); p += n; return s;
  }
  // Pascal 文字列 (長さバイト込みで偶数長へ詰める)
  std::string pascalEven() {
    size_t n = (size_t)u8();
    std::string s = bytes(n);
    if (((n + 1) & 1) != 0) p += 1;
    return s;
  }
};

}  // anonymous namespace

void loadAnnotations(Data &data) {
  data.annotations.clear();
  for (const auto &b : data.globalBlocks) {
    if (b.key != 'Anno' || !data.layerAndMaskTrailing) continue;
    std::vector<uint8_t> buf((size_t)b.dataLength);
    data.layerAndMaskTrailing->init();
    IteratorBase *r = data.layerAndMaskTrailing->cloneRange(b.dataOffset, b.dataLength);
    int got = (r && b.dataLength > 0) ? r->getData(buf.data(), b.dataLength) : 0;
    delete r;
    if (got != b.dataLength) return;
    Rd d(buf);
    d.u16(); d.u16();
    const uint32_t count = d.u32();
    for (uint32_t i = 0; i < count && d.ok && i < 4096; i++) {
      const size_t start = d.p;
      const uint32_t len = d.u32();
      if (len < 4 || !d.need(len - 4)) break;
      AnnotationInfo a;
      a.kind = d.bytes(4);
      a.open = d.u8() != 0;
      a.flags = d.u8();
      d.u16();
      for (int k = 0; k < 4; k++) a.iconRect[k] = (int32_t)d.u32();
      for (int k = 0; k < 4; k++) a.popupRect[k] = (int32_t)d.u32();
      a.colorSpace = d.u16();
      for (int k = 0; k < 4; k++) a.color[k] = d.u16();
      a.author = d.pascalEven();
      a.name = d.pascalEven();
      a.modDate = d.pascalEven();
      const size_t dataStart = d.p;
      const uint32_t dataLen = d.u32();
      std::string tag = d.bytes(4);
      uint32_t size = d.u32();
      if (d.ok && dataLen >= 12 && d.need(size)) {
        if (tag == "txtC") {
          size_t q = d.p, e = d.p + size;
          // BOM (FE FF) があれば読み飛ばす
          if (size >= 2 && d.b[q] == 0xFE && d.b[q + 1] == 0xFF) q += 2;
          for (; q + 1 < e; q += 2) a.text.push_back((char16_t)((d.b[q] << 8) | d.b[q + 1]));
          while (!a.text.empty() && a.text.back() == 0) a.text.pop_back();
        } else {
          a.dataSize = size;     // 音声などは大きさだけ
        }
      }
      (void)dataStart;
      if (d.ok) data.annotations.push_back(a);
      d.p = start + len;
    }
    return;
  }
}

bool loadLayerArtboard(LayerInfo &layer, AdditionalLayerInfo &additional) {
  if (!additional.data) return true;
  IteratorBase *r = additional.data->clone();
  r->init();
  if (r->rest() < 4) { delete r; return true; }
  (void)r->getInt32();
  Descriptor d;
  d.load(r);
  delete r;
  ArtboardInfo &a = layer.artboard;
  a = ArtboardInfo();
  a.present = true;
  auto num = [](Descriptor *x, const char *k) -> double {
    DescriptorItem *it = x ? x->item(k).find() : 0;
    if (auto *v = dynamic_cast<DescriptorDouble*>(it)) return v->val;
    if (auto *v = dynamic_cast<DescriptorUnitFloat*>(it)) return v->val;
    if (auto *v = dynamic_cast<DescriptorInteger*>(it)) return v->val;
    return 0.0;
  };
  if (auto *rc = dynamic_cast<Descriptor*>(d.item("artboardRect").find())) {
    a.left = num(rc, "Left"); a.top = num(rc, "Top ");
    a.right = num(rc, "Rght"); a.bottom = num(rc, "Btom");
  }
  if (auto *s = dynamic_cast<DescriptorString*>(d.item("artboardPresetName").find())) {
    a.presetName = s->val;
    while (!a.presetName.empty() && a.presetName.back() == 0) a.presetName.pop_back();
  }
  a.backgroundType = (int)num(&d, "artboardBackgroundType");
  if (auto *c = dynamic_cast<Descriptor*>(d.item("Clr ").find())) {
    a.hasColor = true;
    a.color[0] = num(c, "Rd  "); a.color[1] = num(c, "Grn "); a.color[2] = num(c, "Bl  ");
  }
  return true;
}

}  // namespace psd
