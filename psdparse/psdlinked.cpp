// スマートオブジェクト: 文書末尾の埋め込み / リンクファイル (lnk2 / lnk3 / lnkD /
// lnkE) と、レイヤ側の配置情報 (SoLd / SoLE / PlLd)。
//
// 公開仕様 "Linked Layer": ブロックの中身は item の並びで、各 item は 8 バイトの
// 長さ + 本体 (4 の倍数へ詰める)。本体は
//   種別 ('liFD' 埋め込み / 'liFE' 外部 / 'liFA' エイリアス) + version (1..8)
//   + uuid (Pascal 文字列, 詰め物なし) + ファイル名 (Unicode) + file type (4)
//   + creator (4) + データ長 (8) + open descriptor の有無 (1) [+ version + descriptor]
//   + 種別ごとの続き (liFD はファイルの中身そのもの)
// 外部 / エイリアスの続きは psd-tools の読み方に合わせている。
//
// ファイルの中身は巨大になりうるので、ここでは位置だけを覚え、取り出しは
// PSDFile::getLinkedFileData が要求されたときに行う。
#include "psdparse.h"
#include "psdlayer.h"
#include "psdresource.h"

#include <cstring>
#include <vector>

namespace psd {

namespace {

// IteratorBase 上の簡単な読み取り (範囲外なら ok = false)
struct Rd {
  IteratorBase *r;
  bool ok = true;
  explicit Rd(IteratorBase *r) : r(r) {}
  int pos() { return r->size() - r->rest(); }
  bool need(int64_t n) {
    if (n < 0 || n > r->rest()) ok = false;
    return ok;
  }
  uint32_t u32() { return need(4) ? (uint32_t)r->getInt32(true) : 0; }
  uint64_t u64() { return need(8) ? (uint64_t)r->getInt64(true) : 0; }
  int u8() { return need(1) ? r->getCh() : 0; }
  std::string bytes(int n) {
    std::string s;
    if (!need(n) || n == 0) return s;
    s.resize((size_t)n);
    r->getData(&s[0], n);
    return s;
  }
  void skip(int64_t n) { if (need(n)) r->advance((int)n); }
  u16str unicode() {
    u16str s;
    uint32_t n = u32();
    if (!need((int64_t)n * 2)) return s;
    s.reserve(n);
    for (uint32_t i = 0; i < n; i++) s.push_back((char16_t)r->getInt16(true));
    while (!s.empty() && s.back() == 0) s.pop_back();   // 末尾の NUL を落とす
    return s;
  }
  // version(4) + descriptor を読み飛ばす
  void skipDescriptorBlock() {
    skip(4);
    if (!ok) return;
    Descriptor d;
    if (!d.load(r)) ok = false;
  }
};

std::string fourcc(uint32_t v) {
  std::string s(4, ' ');
  for (int i = 0; i < 4; i++) s[(size_t)i] = (char)((v >> (24 - 8 * i)) & 0xff);
  return s;
}

// item 1 件。itemStart は trailing 上の位置 (データ位置の計算用)
bool parseLinkedItem(IteratorBase *r, int key, int itemStart, LinkedFileInfo &out) {
  Rd d(r);
  out.blockKey = key;
  out.kind     = d.bytes(4);
  out.version  = (int)d.u32();
  int n        = d.u8();
  out.uuid     = d.bytes(n);
  out.fileName = d.unicode();
  out.fileType = d.bytes(4);
  out.creator  = d.bytes(4);
  out.dataSize = d.u64();
  if (d.u8() != 0) d.skipDescriptorBlock();   // open descriptor
  if (!d.ok) return false;
  if (out.kind == "liFD") {
    if (!d.need((int64_t)out.dataSize)) return false;
    out.hasData    = true;
    out.dataOffset = itemStart + d.pos();
  } else if (out.kind == "liFE") {
    d.skipDescriptorBlock();                  // リンク先の情報
    if (out.version > 3) d.skip(4 + 4 + 8);   // 更新日時 (年 + 月日時分 + 秒)
    d.skip(8);                                // 外部ファイルの大きさ
    if (d.ok && out.version > 2 && out.dataSize > 0 && d.need((int64_t)out.dataSize)) {
      out.hasData    = true;                  // 外部ファイルの写しを抱えている
      out.dataOffset = itemStart + d.pos();
    }
  }
  return d.ok;
}

double readDouble(IteratorBase *r) {
  pun64 v;
  v.i = (uint64_t)r->getInt64(true);
  return v.f;
}

double descNumber(Descriptor &d, const char *key, bool &found) {
  DescriptorItem *it = d.item(key).find();
  if (auto *x = dynamic_cast<DescriptorDouble*>(it))    { found = true; return x->val; }
  if (auto *x = dynamic_cast<DescriptorUnitFloat*>(it)) { found = true; return x->val; }
  if (auto *x = dynamic_cast<DescriptorInteger*>(it))   { found = true; return x->val; }
  found = false;
  return 0.0;
}

// UTF-16 → UTF-8。descriptor の文字列は末尾に NUL を含むことがあるので落とす。
std::string u16ToUtf8(const u16str &in) {
  u16str s = in;
  while (!s.empty() && s.back() == 0) s.pop_back();
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    uint32_t c = s[i];
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.size()) {
      uint32_t lo = s[i + 1];
      if (lo >= 0xdc00 && lo <= 0xdfff) { c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00); i++; }
    }
    if (c < 0x80) out.push_back((char)c);
    else if (c < 0x800) { out.push_back((char)(0xc0 | (c >> 6))); out.push_back((char)(0x80 | (c & 0x3f))); }
    else if (c < 0x10000) {
      out.push_back((char)(0xe0 | (c >> 12)));
      out.push_back((char)(0x80 | ((c >> 6) & 0x3f)));
      out.push_back((char)(0x80 | (c & 0x3f)));
    } else {
      out.push_back((char)(0xf0 | (c >> 18)));
      out.push_back((char)(0x80 | ((c >> 12) & 0x3f)));
      out.push_back((char)(0x80 | ((c >> 6) & 0x3f)));
      out.push_back((char)(0x80 | (c & 0x3f)));
    }
  }
  return out;
}

}  // anonymous namespace

void loadLinkedFiles(Data &data) {
  data.linkedFiles.clear();
  if (!data.layerAndMaskTrailing) return;
  for (const auto &b : data.globalBlocks) {
    if (b.key != 'lnk2' && b.key != 'lnk3' && b.key != 'lnkD' && b.key != 'lnkE') continue;
    int p = b.dataOffset;
    const int end = b.dataOffset + b.dataLength;
    while (p + 8 <= end) {
      data.layerAndMaskTrailing->init();
      IteratorBase *h = data.layerAndMaskTrailing->cloneRange(p, 8);
      uint64_t len = h ? (uint64_t)h->getInt64(true) : 0;
      delete h;
      if (len == 0 || len > (uint64_t)(end - p - 8)) break;
      const int itemStart = p + 8;
      data.layerAndMaskTrailing->init();
      IteratorBase *item = data.layerAndMaskTrailing->cloneRange(itemStart, (int)len);
      if (item) {
        LinkedFileInfo info;
        if (parseLinkedItem(item, b.key, itemStart, info)) data.linkedFiles.push_back(info);
        delete item;
      }
      p = itemStart + (int)len;
      p += (4 - ((int)len & 3)) & 3;          // item は 4 の倍数へ詰める
    }
  }
}

// SoLd / SoLE: 'soLD' + version(4) + descriptor version(4) + descriptor
// PlLd: 'plcL' + version + uuid (Pascal) + page/total/antiAlias/type + transform
//       (8 doubles) + warp
bool loadLayerSmartObject(LayerInfo &layer, AdditionalLayerInfo &additional) {
  if (!additional.data) return true;
  SmartObjectInfo &so = layer.smartObject;
  // SoLd があれば PlLd より優先 (CS3 以降は両方書かれ、SoLd の方が詳しい)
  if (so.present && so.key != 'PlLd' && additional.key == 'PlLd') return true;
  IteratorBase *r = additional.data->clone();
  r->init();
  SmartObjectInfo s;
  s.present = true;
  s.key = additional.key;
  if (additional.key == 'PlLd') {
    Rd d(r);
    d.skip(8);                                  // 'plcL' + version
    int n = d.u8();
    s.uuid = d.bytes(n);
    s.page = (int)d.u32();
    s.totalPages = (int)d.u32();
    s.antiAlias = (int)d.u32();
    s.placedType = (int)d.u32();
    if (d.need(64)) {
      for (int i = 0; i < 8; i++) s.transform[i] = readDouble(r);
      s.hasTransform = true;
    }
    if (!d.ok) { delete r; return true; }
  } else {
    if (r->rest() < 12) { delete r; return true; }
    r->advance(12);
    Descriptor desc;
    if (!desc.load(r)) { delete r; return true; }
    if (auto *x = dynamic_cast<DescriptorString*>(desc.item("Idnt").find()))
      s.uuid = u16ToUtf8(x->val);
    if (auto *x = dynamic_cast<DescriptorString*>(desc.item("placed").find()))
      s.placedId = u16ToUtf8(x->val);
    bool f = false;
    s.page       = (int)descNumber(desc, "PgNm", f);
    s.totalPages = (int)descNumber(desc, "totalPages", f);
    s.antiAlias  = (int)descNumber(desc, "Annt", f);
    s.placedType = (int)descNumber(desc, "Type", f);
    if (auto *t = dynamic_cast<DescriptorList*>(desc.item("Trnf").find())) {
      if (t->items.size() == 8) {
        s.hasTransform = true;
        for (int i = 0; i < 8; i++) {
          auto *v = dynamic_cast<DescriptorDouble*>(t->items[(size_t)i]);
          s.transform[i] = v ? v->val : 0.0;
        }
      }
    }
    if (auto *sz = dynamic_cast<Descriptor*>(desc.item("Sz  ").find())) {
      bool fw = false, fh = false;
      s.width  = descNumber(*sz, "Wdth", fw);
      s.height = descNumber(*sz, "Hght", fh);
      s.hasSize = fw && fh;
    }
    if (auto *fx = dynamic_cast<Descriptor*>(desc.item("filterFX").find())) {
      s.hasFilters = true;
      if (auto *e = dynamic_cast<DescriptorBoolean*>(fx->item("enab").find()))
        s.filtersEnabled = e->val;
    }
  }
  delete r;
  so = s;
  return true;
}

}  // namespace psd
