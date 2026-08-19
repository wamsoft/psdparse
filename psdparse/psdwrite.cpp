
#include "psdwrite.h"

#include <cstring>

namespace psd {

// ===========================================================================
// FileWriter
// ===========================================================================

FileWriter::FileWriter(const char *path) : fp_(nullptr) {
#ifdef _WIN32
  // fopen は ANSI 限定なので UTF-8 → UTF-16 → _wfopen で開く。
  std::wstring w = utf8ToWide(path);
  if (w.empty()) return;
  fp_ = _wfopen(w.c_str(), L"wb");
#else
  fp_ = std::fopen(path, "wb");
#endif
}

FileWriter::~FileWriter() { close(); }

void FileWriter::close() {
  if (fp_) { std::fclose(fp_); fp_ = nullptr; }
}

size_t FileWriter::putData(const void *data, size_t n) {
  if (!fp_ || n == 0) return 0;
  return std::fwrite(data, 1, n, fp_);
}

int64_t FileWriter::tell() const {
  if (!fp_) return -1;
#ifdef _WIN32
  return _ftelli64(fp_);
#else
  return (int64_t)ftello(fp_);
#endif
}

bool FileWriter::seek(int64_t pos) {
  if (!fp_) return false;
#ifdef _WIN32
  return _fseeki64(fp_, pos, SEEK_SET) == 0;
#else
  return fseeko(fp_, (off_t)pos, SEEK_SET) == 0;
#endif
}

// ===========================================================================
// writePSD
// ===========================================================================
//
// PSD ファイルは 5 セクションで構成される。各セクションには長さフィールドが
// 先頭に付いていることが多いので、書き込み時は「placeholder 0 を書く →
// 中身を書く → 戻ってサイズを埋める」の patch-back パターンを多用する。
//
//   1. File Header (26 bytes 固定)
//   2. Color Mode Data (4 byte length + content)
//   3. Image Resources (4 byte length + 各リソースは 8BIM + id + Pascal name +
//      4 byte data size + data + 偶数 padding の繰り返し)
//   4. Layer and Mask Information (4 byte length + Layer Info subsection +
//      Global Layer Mask Info subsection + 任意の追加 info)
//   5. Image Data (compression word + interleaved planes)
//
// Round-trip 戦略:
//   * 構造的に再現可能なフィールド (header, layer record, blend key, …) は
//     parsed フィールドから再シリアライズ
//   * 詳細パースが煩雑な block (layer mask / blending range / additional
//     layer info の生バイト, global layer mask info の生バイト, channel image
//     data の RLE/raw bytes, merged image bytes) は parse 時に保持した
//     IteratorBase からそのまま転送
//
// "patch-back" ヘルパ: ラムダ的に使いたいので明示マクロは使わず、ローカル
// で position を保存して後で書き戻す方式。

namespace {

inline void writeHeader(WriterBase &w, const Header &h) {
  w.putData("8BPS", 4);
  w.putUint16BE((uint16_t)h.version);
  w.putZero(6);  // reserved
  w.putUint16BE((uint16_t)h.channels);
  w.putUint32BE((uint32_t)h.height);
  w.putUint32BE((uint32_t)h.width);
  w.putUint16BE((uint16_t)h.depth);
  w.putUint16BE((uint16_t)h.mode);
}

inline void writeColorModeData(WriterBase &w, const Data &data) {
  if (data.colorModeIterator && data.colorModeSize > 0) {
    w.putUint32BE((uint32_t)data.colorModeSize);
    w.copyAllFrom(data.colorModeIterator);
  } else {
    w.putUint32BE(0);
  }
}

inline void writeImageResources(WriterBase &w, const Data &data) {
  int64_t sizePos = w.tell();
  w.putUint32BE(0); // placeholder
  int64_t bodyStart = w.tell();
  for (const auto &res : data.imageResourceList) {
    w.putData("8BIM", 4);
    w.putUint16BE(res.id);
    int nameLen = (int)res.name.size();
    if (nameLen > 255) nameLen = 255;
    w.putCh(nameLen);
    if (nameLen > 0) w.putData(res.name.data(), (size_t)nameLen);
    // pascal: length byte + chars, total padded to even
    int total = 1 + nameLen;
    if (total & 1) w.putZero(1);
    w.putUint32BE((uint32_t)res.size);
    if (res.data && res.size > 0) w.copyAllFrom(res.data);
    // data section padded to even
    if (res.size & 1) w.putZero(1);
  }
  int64_t bodyEnd = w.tell();
  // patch back
  w.seek(sizePos);
  w.putUint32BE((uint32_t)(bodyEnd - bodyStart));
  w.seek(bodyEnd);
}

// 8 バイト double を big-endian で書く (mask feather / descriptor 用)。
inline void putDoubleBE(WriterBase &w, double d) {
  pun64 v; v.f = d;
  w.putUint32BE((uint32_t)(v.i >> 32));
  w.putUint32BE((uint32_t)(v.i & 0xffffffffu));
}

// descriptor の 4cc/文字列 ID を書く。readId は size==0 を 4 と解釈するので、
// 4 文字 ID は size=0 (4cc 形式)、それ以外は size=len で書く。
inline void writeDescId(WriterBase &w, const std::string &id) {
  if (id.size() == 4) { w.putUint32BE(0); w.putData(id.data(), 4); }
  else { w.putUint32BE((uint32_t)id.size());
         if (!id.empty()) w.putData(id.data(), id.size()); }
}

// descriptor の Unicode 文字列を書く (charCount(4) + UTF-16BE)。
inline void writeDescUnicode(WriterBase &w, const u16str &s) {
  w.putUint32BE((uint32_t)s.size());
  for (char16_t ch : s) w.putUint16BE((uint16_t)ch);
}

// DescriptorReference の各参照アイテムを書く (load の逆)。
inline void writeReferenceItem(WriterBase &w, ReferenceItem *r) {
  w.putUint32BE((uint32_t)r->type);
  if (auto *x = dynamic_cast<ReferenceProperty*>(r)) {
    writeDescUnicode(w, x->name); writeDescId(w, x->classId); writeDescId(w, x->keyId);
  } else if (auto *x = dynamic_cast<ReferenceClass*>(r)) {
    writeDescUnicode(w, x->name); writeDescId(w, x->classId);
  } else if (auto *x = dynamic_cast<ReferenceEnumRef*>(r)) {
    writeDescUnicode(w, x->name); writeDescId(w, x->classId);
    writeDescId(w, x->typeId); writeDescId(w, x->enumId);
  } else if (auto *x = dynamic_cast<ReferenceOffset*>(r)) {
    writeDescUnicode(w, x->name); writeDescId(w, x->classId); w.putInt32BE(x->offset);
  } else if (auto *x = dynamic_cast<ReferenceIdentifier*>(r)) {
    w.putInt32BE(x->identifier);
  } else if (auto *x = dynamic_cast<ReferenceIndex*>(r)) {
    w.putInt32BE(x->index);
  } else if (auto *x = dynamic_cast<ReferenceName*>(r)) {
    writeDescUnicode(w, x->name);
  }
}

// layer mask サブブロックの本体 (4 バイト長の後ろ) をフィールドから直列化する。
// parseLayerMask と対で、real セクションは size>=36、パラメータは flags bit4 で
// 判定される。編集された density/feather に合わせて flags bit4 を張り直す。
inline void serializeLayerMask(WriterBase &w, const LayerMask &m) {
  w.putInt32BE(m.top);  w.putInt32BE(m.left);
  w.putInt32BE(m.bottom); w.putInt32BE(m.right);
  w.putCh(m.defaultColor);
  bool params = (m.userMaskDensity >= 0) || m.hasUserFeather ||
                (m.vectorMaskDensity >= 0) || m.hasVectorFeather;
  int flags = m.flags;
  if (params) flags |= 0x10; else flags &= ~0x10;
  w.putCh(flags);
  if (m.hasReal) {
    w.putCh(m.realFlags);
    w.putCh(m.realUserMaskBackground);
    w.putInt32BE(m.enclosingTop);    w.putInt32BE(m.enclosingLeft);
    w.putInt32BE(m.enclosingBottom); w.putInt32BE(m.enclosingRight);
  }
  if (params) {
    int pf = 0;
    if (m.userMaskDensity   >= 0) pf |= 0x01;
    if (m.hasUserFeather)         pf |= 0x02;
    if (m.vectorMaskDensity >= 0) pf |= 0x04;
    if (m.hasVectorFeather)       pf |= 0x08;
    w.putCh(pf);
    if (pf & 0x01) w.putCh(m.userMaskDensity);
    if (pf & 0x02) putDoubleBE(w, m.userMaskFeather);
    if (pf & 0x04) w.putCh(m.vectorMaskDensity);
    if (pf & 0x08) putDoubleBE(w, m.vectorMaskFeather);
  }
}

// luni (Unicode layer name) 追加情報ブロックを書く。
// レイヤレコードの tagged block は整列パディング無し (padding=1)。
inline void writeLuniBlock(WriterBase &w, const u16str &name) {
  w.putData("8BIM", 4);
  w.putUint32BE((uint32_t)'luni');
  uint32_t dataLen = 4 + 2 * (uint32_t)name.size();  // charCount(4) + UTF16BE
  w.putUint32BE(dataLen);
  w.putUint32BE((uint32_t)name.size());
  for (char16_t ch : name) w.putUint16BE((uint16_t)ch);
}

// extra data をフィールドから再構築する (改名等で useRawBytes==false のとき)。
// mask / blending ranges は生バイト (maskRaw/blendRaw) をそのまま転送し、
// pascal 名を書き直し、additional info は各エントリを複製する。ただし 'luni' は
// lay.layerNameUnicode で置き換える (無ければ末尾に追加)。
inline void writeLayerExtraFromFields(WriterBase &w, const LayerInfo &lay) {
  const LayerExtraData &ex = lay.extraData;
  // layer mask: 編集済みならフィールドから直列化、そうでなければ生バイトを転送。
  if (ex.layerMask.present && ex.layerMask.edited) {
    int64_t p = w.tell();
    w.putUint32BE(0);                          // 長さプレースホルダ
    int64_t s = w.tell();
    serializeLayerMask(w, ex.layerMask);
    int64_t e = w.tell();
    w.seek(p); w.putUint32BE((uint32_t)(e - s)); w.seek(e);
  } else if (ex.maskRaw) {
    w.putUint32BE((uint32_t)ex.maskRaw->size()); w.copyAllFrom(ex.maskRaw);
  } else {
    w.putUint32BE(0);
  }
  // blending ranges
  if (ex.blendRaw) { w.putUint32BE((uint32_t)ex.blendRaw->size()); w.copyAllFrom(ex.blendRaw); }
  else             { w.putUint32BE(0); }
  // pascal 名 (長さバイト込みで 4 バイト境界へパディング)
  std::string pn = ex.layerName;
  if (pn.size() > 255) pn.resize(255);
  w.putCh((int)pn.size());
  if (!pn.empty()) w.putData(pn.data(), pn.size());
  int total = 1 + (int)pn.size();
  int pad = (4 - (total & 3)) & 3;
  w.putZero((size_t)pad);
  // additional layer info
  bool wroteLuni = false, wroteIOpa = false;
  for (const auto &a : ex.additionalLayers) {
    if (a.key == 'luni') {
      writeLuniBlock(w, lay.layerNameUnicode);   // 新しい名前で置換
      wroteLuni = true;
    } else if (a.key == 'iOpa') {
      w.putData("8BIM", 4);                      // fill opacity を新値で置換
      w.putUint32BE((uint32_t)'iOpa');
      w.putUint32BE(4);                          // 1 byte 値 + 3 filler (Photoshop 形式)
      w.putCh(lay.fill_opacity & 0xff);
      w.putZero(3);
      wroteIOpa = true;
    } else {
      w.putData(a.sigType == 1 ? "8B64" : "8BIM", 4);
      w.putUint32BE((uint32_t)a.key);
      w.putUint32BE((uint32_t)a.size);
      if (a.data) w.copyAllFrom(a.data);
    }
  }
  if (!wroteLuni && !lay.layerNameUnicode.empty())
    writeLuniBlock(w, lay.layerNameUnicode);
  if (!wroteIOpa && lay.fill_opacity != 255) {   // 既存 iOpa 無し & 非既定なら追加
    w.putData("8BIM", 4);
    w.putUint32BE((uint32_t)'iOpa');
    w.putUint32BE(4);
    w.putCh(lay.fill_opacity & 0xff);
    w.putZero(3);
  }
}

inline void writeLayerRecord(WriterBase &w, const LayerInfo &lay) {
  w.putInt32BE(lay.top);
  w.putInt32BE(lay.left);
  w.putInt32BE(lay.bottom);
  w.putInt32BE(lay.right);
  w.putUint16BE((uint16_t)lay.channels.size());
  for (const auto &ch : lay.channels) {
    w.putInt16BE((int16_t)ch.id);
    w.putUint32BE((uint32_t)ch.length);
  }
  w.putData("8BIM", 4);
  // blendModeKey は parse 時に getInt32(true) で読んだ値 (host int)。書く時も
  // 同じ BE で出すと元のディスク表現に戻る。
  w.putUint32BE((uint32_t)lay.blendModeKey);
  w.putCh(lay.opacity);
  w.putCh(lay.clipping);
  w.putCh(lay.flag);
  w.putZero(1); // filler

  // extra data: size + content
  int64_t extraSizePos = w.tell();
  w.putUint32BE(0); // placeholder
  int64_t extraStart = w.tell();
  if (lay.extraData.useRawBytes && lay.extraData.rawBytes) {
    w.copyAllFrom(lay.extraData.rawBytes);       // 未編集: 生バイトをそのまま
  } else {
    writeLayerExtraFromFields(w, lay);           // 編集済み: フィールドから再構築
  }
  int64_t extraEnd = w.tell();
  w.seek(extraSizePos);
  w.putUint32BE((uint32_t)(extraEnd - extraStart));
  w.seek(extraEnd);
}

inline void writeLayerInfo(WriterBase &w, const Data &data) {
  int64_t sizePos = w.tell();
  w.putUint32BE(0); // placeholder
  int64_t bodyStart = w.tell();
  int16_t count = (int16_t)data.layerList.size();
  if (data.mergedAlpha) count = (int16_t)(-count);
  w.putInt16BE(count);
  for (const auto &lay : data.layerList) writeLayerRecord(w, lay);
  // channel image data.
  //   未編集 (layersDirty==false): 元の連結ブロブをそのまま転送 → 末尾パディング
  //     まで含めてバイト一致のラウンドトリップを保証。
  //   編集済み (layersDirty==true): 連結ブロブは編集後のレイヤ順/枚数と合わない
  //     ので、各 channel の imageData (compression word + 圧縮データ, ちょうど
  //     channel.length バイト) をレイヤ毎に個別に書き出して再構築する。末尾は
  //     下の even パディングで詰める。
  if (!data.layersDirty && data.channelImageData) {
    w.copyAllFrom(data.channelImageData);
  } else {
    for (const auto &lay : data.layerList) {
      for (const auto &ch : lay.channels) {
        if (ch.imageData && ch.length > 0) w.copyNFrom(ch.imageData, (size_t)ch.length);
      }
    }
  }
  int64_t bodyEnd = w.tell();
  // PSD 仕様: layer info の長さは 2 の倍数 padding が必要。
  if ((bodyEnd - bodyStart) & 1) { w.putZero(1); bodyEnd++; }
  w.seek(sizePos);
  w.putUint32BE((uint32_t)(bodyEnd - bodyStart));
  w.seek(bodyEnd);
}

inline void writeGlobalLayerMaskInfo(WriterBase &w, const Data &data) {
  if (data.globalLayerMaskInfoRaw) {
    int64_t sizePos = w.tell();
    w.putUint32BE(0);
    int64_t start = w.tell();
    w.copyAllFrom(data.globalLayerMaskInfoRaw);
    int64_t end = w.tell();
    w.seek(sizePos);
    w.putUint32BE((uint32_t)(end - start));
    w.seek(end);
  } else {
    w.putUint32BE(0); // 空ブロック
  }
}

inline void writeLayerAndMask(WriterBase &w, const Data &data) {
  int64_t sizePos = w.tell();
  w.putUint32BE(0);
  int64_t bodyStart = w.tell();
  writeLayerInfo(w, data);
  writeGlobalLayerMaskInfo(w, data);
  // global layer mask info より後ろにあった secondary layer info (Lr16/Lr32 等)
  if (data.layerAndMaskTrailing && data.trailingPatched) {
    // 文書末尾の追加情報のうち 1 ブロックだけ差し替える / 削除する。
    // 前後は元のバイトをそのまま流すので、巨大な lnk2 があってもメモリに
    // 載せずに済む。
    IteratorBase *t = data.layerAndMaskTrailing;
    t->init();
    int total = t->size();
    int off   = data.trailingPatchOffset;
    int after = off + data.trailingPatchLength;
    if (off > 0) {
      t->init();
      IteratorBase *head = t->cloneRange(0, off);
      w.copyAllFrom(head);
      delete head;
    }
    if (!data.trailingPatchBytes.empty())
      w.putData(data.trailingPatchBytes.data(), data.trailingPatchBytes.size());
    if (after < total) {
      t->init();
      IteratorBase *tail = t->cloneRange(after, total - after);
      w.copyAllFrom(tail);
      delete tail;
    }
  } else if (data.layerAndMaskTrailing) {
    w.copyAllFrom(data.layerAndMaskTrailing);
  }
  int64_t bodyEnd = w.tell();
  w.seek(sizePos);
  w.putUint32BE((uint32_t)(bodyEnd - bodyStart));
  w.seek(bodyEnd);
}

inline void writeImageData(WriterBase &w, const Data &data) {
  if (data.imageData) w.copyAllFrom(data.imageData);
}

} // anonymous namespace

// ---- generic descriptor 直列化 (psddesc の load の逆) --------------------

void writeDescriptorBody(WriterBase &w, const Descriptor *d) {
  writeDescUnicode(w, d->name);
  writeDescId(w, d->classId);
  // keyOrder が揃っていれば元のディスク順で、無ければ map 順で書く。
  if (d->keyOrder.size() == d->itemMap.size()) {
    w.putUint32BE((uint32_t)d->keyOrder.size());
    for (const auto &key : d->keyOrder) {
      auto it = d->itemMap.find(key);
      if (it == d->itemMap.end()) continue;
      writeDescId(w, key);
      writeDescriptorItem(w, it->second);
    }
  } else {
    w.putUint32BE((uint32_t)d->itemMap.size());
    for (const auto &kv : d->itemMap) {
      writeDescId(w, kv.first);
      writeDescriptorItem(w, kv.second);
    }
  }
}

void writeDescriptorItem(WriterBase &w, DescriptorItem *it) {
  // 型ごとに [type 4cc] + 値。DescriptorReference と DescriptorRawData は type
  // タグを共有するので dynamic_cast でディスパッチする (descItemToPy と同様)。
  if (auto *x = dynamic_cast<Descriptor*>(it)) {            // Objc / GlbO
    w.putUint32BE((uint32_t)x->type);
    writeDescriptorBody(w, x);
  } else if (auto *x = dynamic_cast<DescriptorList*>(it)) { // VlLs
    w.putUint32BE((uint32_t)'VlLs');
    w.putUint32BE((uint32_t)x->items.size());
    for (auto *i : x->items) writeDescriptorItem(w, i);
  } else if (auto *x = dynamic_cast<DescriptorDouble*>(it)) {
    w.putUint32BE((uint32_t)'doub'); putDoubleBE(w, x->val);
  } else if (auto *x = dynamic_cast<DescriptorUnitFloat*>(it)) {
    w.putUint32BE((uint32_t)'UntF'); w.putUint32BE((uint32_t)x->unit); putDoubleBE(w, x->val);
  } else if (auto *x = dynamic_cast<DescriptorString*>(it)) {
    w.putUint32BE((uint32_t)'TEXT'); writeDescUnicode(w, x->val);
  } else if (auto *x = dynamic_cast<DescriptorEnumerated*>(it)) {
    w.putUint32BE((uint32_t)'enum'); writeDescId(w, x->typeId); writeDescId(w, x->enumId);
  } else if (auto *x = dynamic_cast<DescriptorInteger*>(it)) {
    w.putUint32BE((uint32_t)'long'); w.putInt32BE(x->val);
  } else if (auto *x = dynamic_cast<DescriptorBoolean*>(it)) {
    w.putUint32BE((uint32_t)'bool'); w.putCh(x->val ? 1 : 0);
  } else if (auto *x = dynamic_cast<DescriptorClass*>(it)) { // type / GlbC
    w.putUint32BE((uint32_t)x->type); writeDescUnicode(w, x->name); writeDescId(w, x->classId);
  } else if (auto *x = dynamic_cast<DescriptorAlias*>(it)) {
    w.putUint32BE((uint32_t)'alis');
    w.putUint32BE((uint32_t)x->alias.size());
    if (!x->alias.empty()) w.putData(x->alias.data(), x->alias.size());
  } else if (auto *x = dynamic_cast<DescriptorReference*>(it)) {  // 'obj '
    w.putUint32BE((uint32_t)'obj ');
    w.putUint32BE((uint32_t)x->items.size());
    for (auto *r : x->items) writeReferenceItem(w, r);
  } else if (auto *x = dynamic_cast<DescriptorRawData*>(it)) {    // 'tdta'
    w.putUint32BE((uint32_t)'tdta');
    w.putUint32BE((uint32_t)x->bytes.size());
    if (!x->bytes.empty()) w.putData(x->bytes.data(), x->bytes.size());
  }
}

bool writePSD(WriterBase &w, const Data &data) {
  if (!w.ok()) return false;
  writeHeader(w, data.header);
  writeColorModeData(w, data);
  writeImageResources(w, data);
  writeLayerAndMask(w, data);
  writeImageData(w, data);
  return w.ok();
}

} // namespace psd
