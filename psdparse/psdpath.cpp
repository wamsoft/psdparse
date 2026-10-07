// パス (ベクタマスク / 保存パス / 作業パス) の読み取り。
//
// 公開仕様 "Path resource format": 26 バイトのレコード (2 バイトの種別 +
// 24 バイト) の並び。座標は符号付き 8.24 固定小数で、文書の高さ / 幅に
// 対する比率を (縦, 横) の順に持つ。長さレコードの 3〜4 バイト目にある
// サブパスの合成方法と、その後ろの index は仕様に無く、psd-tools の
// 読み方に合わせている。
#include "psdparse.h"
#include "psdlayer.h"
#include "psdresource.h"

#include <vector>

namespace psd {

namespace {

const size_t kRecordSize = 26;

uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
double fixed824(const uint8_t *p) { return (double)(int32_t)be32(p) / (double)(1 << 24); }

// (縦, 横) の 8 バイトを x / y へ
PathPoint readPoint(const uint8_t *p) {
  PathPoint pt;
  pt.y = fixed824(p);
  pt.x = fixed824(p + 4);
  return pt;
}

// 中身をまるごと読む (data は読み位置ごと消費しないよう clone する)
bool readAll(IteratorBase *it, std::vector<uint8_t> &out) {
  if (!it) return false;
  IteratorBase *r = it->clone();
  r->init();
  int n = r->size();
  out.resize(n > 0 ? (size_t)n : 0);
  int got = n > 0 ? r->getData(out.data(), n) : 0;
  delete r;
  return got == n;
}

}  // anonymous namespace

bool parsePathRecords(const uint8_t *p, size_t n, PathData &out) {
  out = PathData();
  PathSubpath *cur = nullptr;
  int remaining = 0;          // 現在のサブパスで残っている knot の数
  bool ok = true;
  for (size_t off = 0; off + kRecordSize <= n; off += kRecordSize) {
    const uint8_t *rec = p + off;
    const uint8_t *d = rec + 2;
    switch (be16(rec)) {
    case 0:   // closed subpath length record
    case 3: { // open subpath length record
      if (remaining > 0) ok = false;   // 前のサブパスの knot が足りないまま次へ
      out.subpaths.push_back(PathSubpath());
      cur = &out.subpaths.back();
      cur->closed    = be16(rec) == 0;
      remaining      = be16(d);
      cur->operation = (int16_t)be16(d + 2);
      cur->index     = (int)be32(d + 10);
      cur->knots.reserve((size_t)remaining);
      break;
    }
    case 1: case 2:   // closed knot (linked / unlinked)
    case 4: case 5: { // open knot (linked / unlinked)
      if (!cur || remaining <= 0) { ok = false; break; }
      PathKnot k;
      const int sel = be16(rec);
      k.linked    = (sel == 1 || sel == 4);
      k.preceding = readPoint(d);
      k.anchor    = readPoint(d + 8);
      k.leaving   = readPoint(d + 16);
      cur->knots.push_back(k);
      remaining--;
      break;
    }
    case 6:   // path fill rule record (中身は無い)
      out.hasFillRule = true;
      break;
    case 7:   // clipboard record: top, left, bottom, right, resolution (8.24)
      out.hasClipboard        = true;
      out.clipboardTop        = fixed824(d);
      out.clipboardLeft       = fixed824(d + 4);
      out.clipboardBottom     = fixed824(d + 8);
      out.clipboardRight      = fixed824(d + 12);
      out.clipboardResolution = fixed824(d + 16);
      break;
    case 8:   // initial fill rule record
      out.initialFill = be16(d);
      break;
    default:
      ok = false;
      break;
    }
  }
  if (remaining > 0) ok = false;
  if (n % kRecordSize != 0) ok = false;
  return ok;
}

// 'vmsk' / 'vsms': version(4) + flags(4) + パスレコード列
bool loadLayerVectorMask(LayerInfo &layer, AdditionalLayerInfo &additional) {
  std::vector<uint8_t> buf;
  if (!readAll(additional.data, buf) || buf.size() < 8) return false;
  VectorMask &vm = layer.vectorMask;
  vm = VectorMask();
  vm.present = true;
  vm.key     = additional.key;
  vm.version = (int)be32(buf.data());
  vm.flags   = be32(buf.data() + 4);
  parsePathRecords(buf.data() + 8, buf.size() - 8, vm.path);
  return true;   // パスが一部壊れていても読めた分は使う (load 全体は失敗にしない)
}

// 'pths' (Unicode Path Name): 文書末尾の追加情報。version(4) + descriptor で、
// pathList[] の各要素の pathUnicodeName が保存パス (2000〜2997) の Unicode 名を
// リソース順に持つ。リソース名は Pascal 文字列 (システムの文字コード) なので、
// 日本語などの名前はこちらでないと正しく取れない。
void loadUnicodePathNames(Data &data) {
  if (!data.layerAndMaskTrailing) return;
  for (const auto &b : data.globalBlocks) {
    if (b.key != 'pths' || b.dataLength < 4) continue;
    data.layerAndMaskTrailing->init();
    IteratorBase *r = data.layerAndMaskTrailing->cloneRange(b.dataOffset, b.dataLength);
    if (!r) return;
    (void)r->getInt32(true);   // descriptor version (16)
    Descriptor d;
    bool ok = d.load(r);
    delete r;
    if (!ok) return;
    auto *list = dynamic_cast<DescriptorList*>(d.item("pathList").find());
    if (!list) return;
    size_t li = 0;
    for (auto &sp : data.savedPaths) {
      if (sp.id < 2000 || sp.id > 2997) continue;   // 作業パスは対象外
      if (li >= list->items.size()) break;
      auto *e = dynamic_cast<Descriptor*>(list->items[li++]);
      auto *s = e ? dynamic_cast<DescriptorString*>(e->item("pathUnicodeName").find()) : nullptr;
      if (!s) continue;
      sp.nameUnicode = s->val;
      while (!sp.nameUnicode.empty() && sp.nameUnicode.back() == 0)
        sp.nameUnicode.pop_back();               // 末尾の NUL を落とす
      sp.hasNameUnicode = true;
    }
    return;
  }
}

// 保存パス (2000〜2997) / 作業パス (1025)
bool loadResourcePath(Data &data, ImageResourceInfo &res) {
  std::vector<uint8_t> buf;
  if (!readAll(res.data, buf)) return true;
  SavedPath sp;
  sp.id   = res.id;
  sp.name = res.name;
  parsePathRecords(buf.data(), buf.size(), sp.path);
  data.savedPaths.push_back(sp);
  return true;
}

}  // namespace psd
