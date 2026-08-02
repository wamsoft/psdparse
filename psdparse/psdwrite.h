#ifndef __psdwrite_h__
#define __psdwrite_h__

#include "psdbase.h"
#include "psddata.h"
#include "psddesc.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace psd {

// WriterBase: 出力側の I/O 抽象 (IteratorBase の対)。
//
// putData() / tell() / seek() の 3 つだけが純粋仮想。BE 整数や Pascal 文字列
// 等の高水準ヘルパは全てこれらの上に default 実装。
//
// seek/tell は length-prefixed セクション (image resources / layer & mask /
// extra data / ...) のサイズパッチ用に必須 (placeholder 書く → 中身書く →
// seek 戻ってサイズ書く → 末尾に seek 戻る)。
class WriterBase {
public:
  virtual ~WriterBase() = default;

  // 必須: バイト書き込み + 位置取得/設定 + 成否確認
  virtual size_t putData(const void *data, size_t n) = 0;
  virtual int64_t tell() const = 0;
  virtual bool seek(int64_t pos) = 0;
  virtual bool ok() const = 0;

  // 高水準ヘルパ
  void putCh(int b) {
    uint8_t v = (uint8_t)b;
    putData(&v, 1);
  }
  void putBytes(const uint8_t *p, size_t n) { putData(p, n); }
  void putZero(size_t n) {
    static const uint8_t z[64] = {0};
    while (n > sizeof(z)) { putData(z, sizeof(z)); n -= sizeof(z); }
    if (n > 0) putData(z, n);
  }
  void putUint16BE(uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    putData(b, 2);
  }
  void putUint32BE(uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >>  8), (uint8_t) v };
    putData(b, 4);
  }
  void putInt16BE(int16_t v)   { putUint16BE((uint16_t)v); }
  void putInt32BE(int32_t v)   { putUint32BE((uint32_t)v); }

  // Iterator から末尾までを丸ごとコピー。戻り値はコピーしたバイト数。
  size_t copyAllFrom(IteratorBase *it) {
    if (!it) return 0;
    it->init();
    size_t total = 0;
    uint8_t buf[8192];
    while (!it->eoi()) {
      int got = it->getData(buf, (int)sizeof(buf));
      if (got <= 0) break;
      putData(buf, (size_t)got);
      total += (size_t)got;
    }
    return total;
  }

  // Iterator の先頭から最大 n バイトだけコピー。cloneOffset 由来で終端が親
  // ブロック末尾まで延びている iterator (channel.imageData 等) を、宣言された
  // 長さ分だけ切り出して書くのに使う。戻り値はコピーしたバイト数。
  size_t copyNFrom(IteratorBase *it, size_t n) {
    if (!it || n == 0) return 0;
    it->init();
    size_t total = 0;
    uint8_t buf[8192];
    while (total < n) {
      size_t left = n - total;
      int want = (int)(left < sizeof(buf) ? left : sizeof(buf));
      int got = it->getData(buf, want);
      if (got <= 0) break;
      putData(buf, (size_t)got);
      total += (size_t)got;
    }
    return total;
  }
};

// FILE * ベースの WriterBase 実装。fopen/fwrite/_fseeki64/_ftelli64 使用。
// path は UTF-8 (Win32 では内部で UTF-16 → _wfopen)。
class FileWriter : public WriterBase {
public:
  explicit FileWriter(const char *path);
  ~FileWriter() override;
  FileWriter(const FileWriter &) = delete;
  FileWriter &operator=(const FileWriter &) = delete;

  bool ok() const override { return fp_ != nullptr; }
  size_t putData(const void *data, size_t n) override;
  int64_t tell() const override;
  bool seek(int64_t pos) override;
  void close();

private:
  FILE *fp_;
};

// メモリ上の std::vector<uint8_t> に書き込む WriterBase 実装。seek/tell 対応
// (patch-back や descriptor の直列化に使う)。buf の所有権は呼び出し元。
class MemoryWriter : public WriterBase {
public:
  explicit MemoryWriter(std::vector<uint8_t> &buf) : buf_(buf), pos_(0) {}
  bool ok() const override { return true; }
  size_t putData(const void *data, size_t n) override {
    if (n == 0) return 0;
    size_t need = pos_ + n;
    if (need > buf_.size()) buf_.resize(need);
    std::memcpy(buf_.data() + pos_, data, n);
    pos_ += n;
    return n;
  }
  int64_t tell() const override { return (int64_t)pos_; }
  bool seek(int64_t p) override {
    if (p < 0) return false;
    pos_ = (size_t)p;
    if (pos_ > buf_.size()) buf_.resize(pos_);
    return true;
  }
private:
  std::vector<uint8_t> &buf_;
  size_t pos_;
};

// Photoshop の generic descriptor 直列化 (psddesc の load の逆)。
//   writeDescriptorBody: 先頭 type 無しの本体 (name + classId + count + items)。
//     lfx2 等トップレベル descriptor はこれで書ける。
//   writeDescriptorItem: [type 4cc] + 値。nested descriptor / list の要素用。
void writeDescriptorBody(WriterBase &w, const Descriptor *d);
void writeDescriptorItem(WriterBase &w, DescriptorItem *it);

// Data 全体を PSD ファイルフォーマットで w に書き出す。ラウンドトリップ
// (load → save → re-load で構造一致) を目標とする。w.ok() && writePSD()==true
// で成功。
bool writePSD(WriterBase &w, const Data &data);

} // namespace psd

#endif // __psdwrite_h__
