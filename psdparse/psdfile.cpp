
#include "psdparse.h"
#include "psdfile.h"
#include "psdwrite.h"
#include "psddesc.h"
#include "psdengine.h"

#include <cmath>
#include <cstring>
#include <iostream>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
#endif

namespace psd {

// ============================================================================
// PSDFile::Mapping -- pimpl for OS-level memory-mapped file
// ============================================================================
//
// 構築に成功するとファイル全域が読み取り専用 mmap される。MemoryReader が
// data()/size() を覗くだけで使えるので、IteratorBase は OS のページキャッシュ
// 越しに必要な分だけ実ディスクから読まれる。PSDFile 全体が「ファイルを開い
// たまま、構造情報だけ走査し、レイヤピクセルは要求されたときだけページ
// インさせる」設計の心臓部。

struct PSDFile::Mapping {
#ifdef _WIN32
  HANDLE hFile  = INVALID_HANDLE_VALUE;
  HANDLE hMap   = nullptr;
  LPVOID view   = nullptr;
  size_t length = 0;

  static std::unique_ptr<Mapping> open(const wchar_t *wpath) {
    auto m = std::unique_ptr<Mapping>(new Mapping());
    m->hFile = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (m->hFile == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(m->hFile, &sz) || sz.QuadPart == 0 ||
        (uint64_t)sz.QuadPart > 0x7FFFFFFFFFFFFFFFull) {
      return nullptr;
    }
    m->length = (size_t)sz.QuadPart;
    m->hMap = CreateFileMappingW(m->hFile, nullptr, PAGE_READONLY,
                                 0, 0, nullptr);
    if (!m->hMap) return nullptr;
    m->view = MapViewOfFile(m->hMap, FILE_MAP_READ, 0, 0, 0);
    if (!m->view) return nullptr;
    return m;
  }

  ~Mapping() {
    if (view)                       UnmapViewOfFile(view);
    if (hMap)                       CloseHandle(hMap);
    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
  }
#else
  void  *ptr    = nullptr;
  size_t length = 0;
  int    fd     = -1;

  static std::unique_ptr<Mapping> open(const char *path) {
    auto m = std::unique_ptr<Mapping>(new Mapping());
    m->fd = ::open(path, O_RDONLY);
    if (m->fd < 0) return nullptr;
    struct stat st{};
    if (fstat(m->fd, &st) < 0 || st.st_size <= 0) return nullptr;
    m->length = (size_t)st.st_size;
    m->ptr = mmap(nullptr, m->length, PROT_READ, MAP_PRIVATE, m->fd, 0);
    if (m->ptr == MAP_FAILED) { m->ptr = nullptr; return nullptr; }
    return m;
  }

  ~Mapping() {
    if (ptr) munmap(ptr, length);
    if (fd >= 0) ::close(fd);
  }
#endif

  const uint8_t *data() const {
#ifdef _WIN32
    return (const uint8_t *)view;
#else
    return (const uint8_t *)ptr;
#endif
  }
  size_t size() const { return length; }
};

// ============================================================================
// PSDFile
// ============================================================================

PSDFile::PSDFile() : isLoaded(false) {}
PSDFile::~PSDFile() = default;

void PSDFile::clearData() {
  Data::clearData();
  isLoaded = false;
  mapping_.reset();
  std::vector<uint8_t>().swap(ownedBuffer_);
  ownedStream_.reset();
}

bool PSDFile::load(const char *filename) {
  clearData();
  isLoaded = false;
#ifdef _WIN32
  std::wstring w = utf8ToWide(filename);
  if (w.empty()) return false;
  mapping_ = Mapping::open(w.c_str());
#else
  mapping_ = Mapping::open(filename);
#endif
  if (!mapping_) {
    std::cerr << "mmap failed for: '" << (filename ? filename : "(null)") << "'\n";
    return false;
  }
  MemoryReader reader(mapping_->data(), (int)mapping_->size());
  if (!parsePSD(reader, *this)) { clearData(); return false; }
  isLoaded = processParsed();
  if (!isLoaded) clearData();
  return isLoaded;
}

bool PSDFile::loadFromMemory(const uint8_t *data, size_t size) {
  clearData();
  isLoaded = false;
  if (data == nullptr || size == 0) return false;
  ownedBuffer_.assign(data, data + size);
  MemoryReader reader(ownedBuffer_.data(), (int)ownedBuffer_.size());
  if (!parsePSD(reader, *this)) {
    clearData();
    return false;
  }
  isLoaded = processParsed();
  if (!isLoaded) clearData();
  return isLoaded;
}

bool PSDFile::loadFromReader(IteratorBase &reader) {
  // 汎用エントリ。reader が指す storage の維持責任は呼び出し元。
  // (組み込み先の独自ストリームは StreamReader::Source でラップして渡す)
  clearData();
  isLoaded = false;
  if (!parsePSD(reader, *this)) {
    clearData();
    return false;
  }
  isLoaded = processParsed();
  if (!isLoaded) clearData();
  return isLoaded;
}

namespace {
// std::istream を StreamReader::Source として晒すアダプタ。
// PSDFile が StreamReader 経由でロードした際、shared_ptr<Source> 経由で
// 全 IteratorBase クローンが同一の istream を共有する (read のたびに
// seek + read; キャッシュは reader 側持ち)。
class IStreamSource : public StreamReader::Source {
public:
  IStreamSource(std::istream &s, size_t totalSize)
    : s_(&s), size_(totalSize) {}
  size_t size() const override { return size_; }
  size_t read(uint8_t *out, size_t offset, size_t len) override {
    s_->clear();
    s_->seekg((std::streamoff)offset, std::ios::beg);
    if (!s_->good()) return 0;
    s_->read(reinterpret_cast<char *>(out), (std::streamsize)len);
    auto got = s_->gcount();
    return (got > 0) ? (size_t)got : 0;
  }
private:
  std::istream *s_;
  size_t size_;
};
} // namespace

bool PSDFile::parseFromStream_(std::istream &stream) {
  // 呼び出し元が clearData() / ownedStream_ の管理を担う。
  isLoaded = false;
  stream.clear();
  stream.seekg(0, std::ios::end);
  std::streamoff total = stream.tellg();
  if (total <= 0) return false;
  stream.seekg(0, std::ios::beg);
  auto src = std::make_shared<IStreamSource>(stream, (size_t)total);
  StreamReader reader(src);
  if (!parsePSD(reader, *this)) return false;
  isLoaded = processParsed();
  return isLoaded;
}

bool PSDFile::loadFromStream(std::istream &stream) {
  clearData();
  bool ok = parseFromStream_(stream);
  if (!ok) clearData();
  return ok;
}

bool PSDFile::save(const char *filename) {
  if (!isLoaded) return false;
  FileWriter w(filename);
  if (!w.ok()) return false;
  return writePSD(w, *this);
}

// --- 新規作成 --------------------------------------------------------------

bool PSDFile::createBlank(int width, int height, int mode) {
  if (width <= 0 || height <= 0) return false;
  if (mode != COLOR_MODE_RGB) return false;   // v1: 8bit RGB のみ
  clearData();                                 // iterator/mapping を解放
  layerList.clear();
  imageResourceList.clear();
  header = Header();                           // hres/vres=72 の既定
  header.version  = 1;
  header.channels = 3;                         // 合成画像は RGB 3ch (α無し)
  header.height   = height;
  header.width    = width;
  header.depth    = 8;
  header.mode     = mode;
  colorModeSize     = 0;
  colorModeIterator = nullptr;
  mergedAlpha  = false;
  layersDirty  = true;                         // save() はレイヤ毎再構築経路へ
  globalLayerMaskInfo = GlobalLayerMaskInfo();
  // 白の合成画像: [00 00] (raw) + 3 プレーン分の 0xFF。
  auto buf = std::make_shared<std::vector<uint8_t>>();
  buf->push_back(0); buf->push_back(0);        // compression = 0 (raw)
  buf->insert(buf->end(), (size_t)width * (size_t)height * 3, 0xFF);
  imageData = new VectorReader(buf);
  isLoaded = true;
  return true;
}

// --- extra data 項目編集 (mask / fill opacity) -----------------------------

namespace {
// マスク編集の共通前処理: マスクを持つレイヤの LayerMask を返し、編集フラグを立てる。
LayerMask *beginMaskEdit(std::vector<LayerInfo> &layers, int index) {
  if (index < 0 || index >= (int)layers.size()) return nullptr;
  LayerExtraData &ex = layers[(size_t)index].extraData;
  if (!ex.layerMask.present) return nullptr;
  ex.layerMask.edited = true;
  ex.useRawBytes = false;
  return &ex.layerMask;
}
} // namespace

bool PSDFile::setMaskDisabled(int index, bool disabled) {
  LayerMask *m = beginMaskEdit(layerList, index);
  if (!m) return false;
  if (disabled) m->flags |= 0x02; else m->flags &= ~0x02;
  return true;
}

bool PSDFile::setMaskDensity(int index, int density) {
  LayerMask *m = beginMaskEdit(layerList, index);
  if (!m) return false;
  if (density < 0) density = 0; if (density > 255) density = 255;
  m->userMaskDensity = density;
  return true;
}

bool PSDFile::setMaskFeather(int index, double feather) {
  LayerMask *m = beginMaskEdit(layerList, index);
  if (!m) return false;
  m->userMaskFeather = feather;
  m->hasUserFeather = true;
  return true;
}

bool PSDFile::setMaskDefaultColor(int index, int color) {
  LayerMask *m = beginMaskEdit(layerList, index);
  if (!m) return false;
  if (color < 0) color = 0; if (color > 255) color = 255;
  m->defaultColor = color;
  return true;
}

bool PSDFile::setFillOpacity(int index, int opacity) {
  if (index < 0 || index >= (int)layerList.size()) return false;
  LayerInfo &lay = layerList[(size_t)index];
  if (opacity < 0) opacity = 0; if (opacity > 255) opacity = 255;
  lay.fill_opacity = opacity;
  lay.extraData.useRawBytes = false;   // save 時に iOpa を再構築
  return true;
}

bool PSDFile::setAdditionalInfoBytes(int index, int key, const uint8_t *data, int size) {
  if (index < 0 || index >= (int)layerList.size()) return false;
  if (size < 0) return false;
  LayerExtraData &ex = layerList[(size_t)index].extraData;
  auto buf = std::make_shared<std::vector<uint8_t>>(data, data + size);
  for (auto &a : ex.additionalLayers) {
    if (a.key == key) {
      delete a.data;
      a.data = new VectorReader(buf);
      a.size = size;
      ex.useRawBytes = false;
      return true;
    }
  }
  ex.additionalLayers.push_back(AdditionalLayerInfo(0, key, size, new VectorReader(buf)));
  ex.useRawBytes = false;
  return true;
}

// --- テキストレイヤ編集 ------------------------------------------------------
//
// TySh ブロックの構造:
//   prefix (56 バイト)  version(2) + transform(6*8) + textVer(2) + descVer(4)
//   descriptor          'Txt ' (併記される本文) と EngineData (実体) を含む
//   suffix              warp descriptor + bounds
//
// 本文の実体は EngineData 側 (Adobe 独自のミニ言語) にあり、'Txt ' は
// Photoshop が併せて持っている複製。両方を更新しないと開いたときに食い違う。

// TySh ブロックを prefix / descriptor / suffix に分解して fn へ渡し、書き換わった
// ものを再直列化して差し戻す。テキスト関連の編集は全部これを土台にしている。
bool PSDFile::editTyShBlock(int index,
                            const std::function<bool(std::vector<uint8_t> &,
                                                     Descriptor &)> &fn,
                            std::string *errorOut) {
  auto fail = [&](const char *msg) {
    if (errorOut) *errorOut = msg;
    return false;
  };
  if (index < 0 || index >= (int)layerList.size()) return fail("layer index out of range");
  if (!fn) return fail("no edit function given");

  LayerInfo &lay = layerList[(size_t)index];
  for (auto &a : lay.extraData.additionalLayers) {
    if (a.key != 'TySh' || !a.data) continue;

    IteratorBase *rd = a.data->clone();
    rd->init();
    std::vector<uint8_t> prefix(56);
    if (rd->getData(prefix.data(), 56) != 56) {
      delete rd;
      return fail("TySh block too short");
    }
    Descriptor td;
    td.load(rd);
    int restLen = rd->rest();
    std::vector<uint8_t> suffix((size_t)(restLen > 0 ? restLen : 0));
    if (restLen > 0) rd->getData(suffix.data(), restLen);   // warp + bounds
    delete rd;

    if (!fn(prefix, td)) return fail("text layer edit failed");

    std::vector<uint8_t> buf;
    MemoryWriter w(buf);
    w.putData(prefix.data(), prefix.size());
    writeDescriptorBody(w, &td);
    if (!suffix.empty()) w.putData(suffix.data(), suffix.size());
    if (!setAdditionalInfoBytes(index, 'TySh', buf.data(), (int)buf.size()))
      return fail("could not store the rebuilt TySh block");
    return true;
  }
  return fail("layer is not a text layer (no TySh block)");
}

bool PSDFile::editTextLayer(int index,
                            const std::function<bool(const std::string &,
                                                     std::string &)> &editEngine,
                            const u16str *newTxt,
                            std::string *errorOut) {
  if (!editEngine) {
    if (errorOut) *errorOut = "no edit function given";
    return false;
  }
  bool noEngine = false;
  bool ok = editTyShBlock(index, [&](std::vector<uint8_t> &, Descriptor &td) {
    auto *eng = dynamic_cast<DescriptorRawData *>(td.findItem("EngineData"));
    if (!eng) { noEngine = true; return false; }
    std::string newEngine;
    if (!editEngine(eng->bytes, newEngine)) return false;
    eng->bytes = newEngine;
    if (newTxt) {
      if (auto *txt = dynamic_cast<DescriptorString *>(td.findItem("Txt ")))
        txt->val = *newTxt;
    }
    return true;
  }, errorOut);
  if (!ok && noEngine && errorOut) *errorOut = "text layer has no EngineData";
  // パース済みの textData も追随させる (再ロードなしで参照できるように)
  if (ok && newTxt) layerList[(size_t)index].textData.text = *newTxt;
  return ok;
}

// --- テキストの配置と流し込み枠 ---------------------------------------------

namespace {
// TySh prefix の transform は byte 2 から big-endian の double が 6 つ。
double readBEDouble(const uint8_t *p) {
  uint8_t b[8];
  for (int i = 0; i < 8; ++i) b[i] = p[7 - i];
  double d;
  std::memcpy(&d, b, 8);
  return d;
}
void writeBEDouble(uint8_t *p, double v) {
  uint8_t b[8];
  std::memcpy(b, &v, 8);
  for (int i = 0; i < 8; ++i) p[i] = b[7 - i];
}
// bounds / boundingBox の 4 辺 (UnitFloat か Double のどちらかで入っている)
bool boundsValue(Descriptor *d, const char *key, double &out) {
  if (!d) return false;
  DescriptorItem *it = d->findItem(key);
  if (DescriptorUnitFloat *u = dynamic_cast<DescriptorUnitFloat *>(it)) { out = u->val; return true; }
  if (DescriptorDouble *f = dynamic_cast<DescriptorDouble *>(it))       { out = f->val; return true; }
  return false;
}
bool setBoundsValue(Descriptor *d, const char *key, double v) {
  if (!d) return false;
  DescriptorItem *it = d->findItem(key);
  if (DescriptorUnitFloat *u = dynamic_cast<DescriptorUnitFloat *>(it)) { u->val = v; return true; }
  if (DescriptorDouble *f = dynamic_cast<DescriptorDouble *>(it))       { f->val = v; return true; }
  return false;
}
} // anonymous

bool PSDFile::getLayerTextTransform(int index, double m[6], std::string *errorOut) const {
  if (index < 0 || index >= (int)layerList.size()) {
    if (errorOut) *errorOut = "layer index out of range";
    return false;
  }
  const LayerInfo &lay = layerList[(size_t)index];
  for (const auto &a : lay.extraData.additionalLayers) {
    if (a.key != 'TySh' || !a.data) continue;
    IteratorBase *rd = a.data->clone();
    rd->init();
    uint8_t prefix[56];
    bool ok = (rd->getData(prefix, 56) == 56);
    delete rd;
    if (!ok) { if (errorOut) *errorOut = "TySh block too short"; return false; }
    for (int i = 0; i < 6; ++i) m[i] = readBEDouble(prefix + 2 + i * 8);
    return true;
  }
  if (errorOut) *errorOut = "layer is not a text layer (no TySh block)";
  return false;
}

bool PSDFile::setLayerTextTransform(int index, const double m[6], std::string *errorOut) {
  bool ok = editTyShBlock(index, [&](std::vector<uint8_t> &prefix, Descriptor &) {
    for (int i = 0; i < 6; ++i) writeBEDouble(prefix.data() + 2 + i * 8, m[i]);
    return true;
  }, errorOut);
  if (ok) {
    invalidateTextEngineData();         // 位置は Txt2 へ写せない
    TextLayerData &td = layerList[(size_t)index].textData;
    for (int i = 0; i < 6; ++i) td.transform[i] = m[i];
  }
  return ok;
}

bool PSDFile::moveTextLayer(int index, double dx, double dy, std::string *errorOut) {
  double m[6];
  if (!getLayerTextTransform(index, m, errorOut)) return false;
  m[4] += dx;
  m[5] += dy;
  if (!setLayerTextTransform(index, m, errorOut)) return false;

  // レイヤ矩形も同じだけずらす。ずらさないと PSD 内蔵のラスタが元の位置に
  // 残り、Photoshop で開き直すまで見た目が二重にずれる。
  LayerInfo &lay = layerList[(size_t)index];
  const int idx = (int)std::lround(dx);
  const int idy = (int)std::lround(dy);
  lay.left += idx;   lay.right  += idx;
  lay.top  += idy;   lay.bottom += idy;
  // マスクを持っていれば一緒に動かす
  LayerMask &mk = lay.extraData.layerMask;
  if (mk.present) {
    mk.left += idx; mk.right  += idx;
    mk.top  += idy; mk.bottom += idy;
    mk.enclosingLeft += idx; mk.enclosingRight  += idx;
    mk.enclosingTop  += idy; mk.enclosingBottom += idy;
    mk.edited = true;                    // マスク矩形はフィールドから再直列化する
    lay.extraData.useRawBytes = false;
  }
  layersDirty = true;
  return true;
}

bool PSDFile::getLayerTextBounds(int index, double &l, double &t, double &r, double &b,
                                 std::string *errorOut) const {
  if (index < 0 || index >= (int)layerList.size()) {
    if (errorOut) *errorOut = "layer index out of range";
    return false;
  }
  const LayerInfo &lay = layerList[(size_t)index];
  for (const auto &a : lay.extraData.additionalLayers) {
    if (a.key != 'TySh' || !a.data) continue;
    IteratorBase *rd = a.data->clone();
    rd->init();
    uint8_t prefix[56];
    if (rd->getData(prefix, 56) != 56) {
      delete rd;
      if (errorOut) *errorOut = "TySh block too short";
      return false;
    }
    Descriptor td;
    td.load(rd);
    delete rd;
    Descriptor *bd = dynamic_cast<Descriptor *>(td.findItem("bounds"));
    if (!bd) { if (errorOut) *errorOut = "text layer has no bounds"; return false; }
    bool ok = boundsValue(bd, "Left", l) && boundsValue(bd, "Top ", t) &&
              boundsValue(bd, "Rght", r) && boundsValue(bd, "Btom", b);
    if (!ok && errorOut) *errorOut = "could not read the bounds values";
    return ok;
  }
  if (errorOut) *errorOut = "layer is not a text layer (no TySh block)";
  return false;
}

bool PSDFile::setLayerTextBounds(int index, double l, double t, double r, double b,
                                 std::string *errorOut) {
  // 枠が変われば折り返しも変わる。Txt2 側の枠は写せないので落とす。
  bool ok = editTyShBlock(index, [&](std::vector<uint8_t> &, Descriptor &td) {
    Descriptor *bd = dynamic_cast<Descriptor *>(td.findItem("bounds"));
    if (!bd) return false;
    if (!setBoundsValue(bd, "Left", l) || !setBoundsValue(bd, "Top ", t) ||
        !setBoundsValue(bd, "Rght", r) || !setBoundsValue(bd, "Btom", b))
      return false;
    // boundingBox (実際の字形の範囲) は Photoshop が開いたときに計算し直すが、
    // 枠からはみ出したままだと表示がおかしくなるので枠の中へ丸めておく。
    if (Descriptor *bb = dynamic_cast<Descriptor *>(td.findItem("boundingBox"))) {
      double v;
      if (boundsValue(bb, "Left", v) && v < l) setBoundsValue(bb, "Left", l);
      if (boundsValue(bb, "Top ", v) && v < t) setBoundsValue(bb, "Top ", t);
      if (boundsValue(bb, "Rght", v) && v > r) setBoundsValue(bb, "Rght", r);
      if (boundsValue(bb, "Btom", v) && v > b) setBoundsValue(bb, "Btom", b);
    }
    return true;
  }, errorOut);
  if (ok) invalidateTextEngineData();
  return ok;
}

bool PSDFile::setLayerText(int index, const u16str &newText, std::string *errorOut) {
  bool ok = editTextLayer(index,
    [&](const std::string &in, std::string &out) {
      return editEngineDataText(in.data(), in.size(), newText, out);
    }, &newText, errorOut);
  // TySh 側は単一ランへ畳まれるので、Txt2 も同じ扱い (長さ指定なし) で揃える。
  if (ok) syncTextEngineData(index, newText, std::vector<int>(), std::vector<int>());
  return ok;
}

bool PSDFile::setLayerTextUtf8(int index, const char *utf8, std::string *errorOut) {
  return setLayerText(index, utf8ToU16(utf8 ? utf8 : ""), errorOut);
}

bool PSDFile::setLayerRunStyle(int index, int runIndex, const RunStyleEdit &edit,
                               std::string *errorOut) {
  bool ok = editTextLayer(index,
    [&](const std::string &in, std::string &out) {
      return editEngineDataRunStyle(in.data(), in.size(), runIndex, edit, out);
    }, nullptr, errorOut);
  if (ok) invalidateTextEngineData();   // 書式は Txt2 へ写せない
  return ok;
}

bool PSDFile::setLayerRichText(int index, const u16str &newText,
                               const std::vector<TextRunSpec> &runs,
                               const std::vector<TextParagraphSpec> &paragraphs,
                               std::string *errorOut, bool formattingUnchanged) {
  bool ok = editTextLayer(index,
    [&](const std::string &in, std::string &out) {
      return editEngineDataRichText(in.data(), in.size(), newText, runs, paragraphs, out);
    }, &newText, errorOut);
  if (ok) {
    // Txt2 への追随。長さだけなら写せるが、書式 (フォント / サイズ / 色 / 行揃え)
    // の指定が乗っている場合は Txt2 側のスタイルシートを作り直さないと辻褄が
    // 合わないので、追随をあきらめて Txt2 を落とす。
    bool styled = false;
    for (const TextRunSpec &r : runs) {
      const RunStyleEdit &e = r.style;
      if (e.hasFont || e.hasSize || e.hasColor || e.hasTracking || e.hasKerning ||
          e.hasBold || e.hasItalic || e.hasUnderline) { styled = true; break; }
    }
    for (const TextParagraphSpec &p : paragraphs)
      if (p.hasJustification) { styled = true; break; }

    if (styled && !formattingUnchanged) {
      dropTextEngineData();
    } else {
      std::vector<int> paraLens, styleLens;
      for (const TextParagraphSpec &p : paragraphs) paraLens.push_back(p.length);
      for (const TextRunSpec &r : runs)             styleLens.push_back(r.length);
      syncTextEngineData(index, newText, paraLens, styleLens);
    }

    // パース済みの runs / paragraphs も新しい構成に追随させる (再ロード無しで
    // 参照できるように)。中身の書式までは戻さず、長さだけ合わせる。
    LayerInfo &lay = layerList[(size_t)index];
    if (!runs.empty()) {
      lay.textData.runs.resize(runs.size());
      for (size_t i = 0; i < runs.size(); i++) lay.textData.runs[i].length = runs[i].length;
    }
    if (!paragraphs.empty()) {
      lay.textData.paragraphs.resize(paragraphs.size());
      for (size_t i = 0; i < paragraphs.size(); i++) {
        lay.textData.paragraphs[i].length = paragraphs[i].length;
        if (paragraphs[i].hasJustification)
          lay.textData.paragraphs[i].justification = paragraphs[i].justification;
      }
      lay.textData.justification = lay.textData.paragraphs[0].justification;
    }
  }
  return ok;
}

bool PSDFile::setLayerJustification(int index, int paraIndex, int justification,
                                    std::string *errorOut) {
  bool ok = editTextLayer(index,
    [&](const std::string &in, std::string &out) {
      return editEngineDataJustification(in.data(), in.size(), paraIndex,
                                         justification, out);
    }, nullptr, errorOut);
  if (ok) {
    invalidateTextEngineData();         // 行揃えは Txt2 へ写せない
    LayerInfo &lay = layerList[(size_t)index];
    for (size_t i = 0; i < lay.textData.paragraphs.size(); i++) {
      if (paraIndex >= 0 && (size_t)paraIndex != i) continue;
      lay.textData.paragraphs[i].justification = justification;
    }
    if (!lay.textData.paragraphs.empty())
      lay.textData.justification = lay.textData.paragraphs[0].justification;
  }
  return ok;
}

bool PSDFile::getLayerFonts(int index, std::vector<std::string> &outUtf8Names,
                            std::string *errorOut) const {
  outUtf8Names.clear();
  auto fail = [&](const char *msg) {
    if (errorOut) *errorOut = msg;
    return false;
  };
  if (index < 0 || index >= (int)layerList.size()) return fail("layer index out of range");

  const LayerInfo &lay = layerList[(size_t)index];
  for (const auto &a : lay.extraData.additionalLayers) {
    if (a.key != 'TySh' || !a.data) continue;
    IteratorBase *rd = a.data->clone();
    rd->init();
    std::vector<uint8_t> prefix(56);
    if (rd->getData(prefix.data(), 56) != 56) { delete rd; return fail("TySh block too short"); }
    Descriptor td;
    td.load(rd);
    delete rd;
    auto *eng = dynamic_cast<DescriptorRawData *>(td.findItem("EngineData"));
    if (!eng) return fail("text layer has no EngineData");
    if (!listEngineDataFonts(eng->bytes.data(), eng->bytes.size(), outUtf8Names))
      return fail("could not read the font table");
    return true;
  }
  return fail("layer is not a text layer (no TySh block)");
}

// --- 構造編集 --------------------------------------------------------------

namespace {

// 文書内の既存 lyid の最大値 + 1 (lyid 無しレイヤは layerId<=0 なので無視される)。
int nextLayerId(const std::vector<LayerInfo> &layers) {
  int maxId = 0;
  for (const auto &l : layers) if (l.layerId > maxId) maxId = l.layerId;
  return maxId + 1;
}

// レイヤに新しい lyid を割り当て、extra data の 'lyid' ブロックも新値で置き換える。
// Photoshop は複製時に新 ID を振るので、複製系 API はこれを通す。
void assignFreshLayerId(LayerInfo &lay, int newId) {
  lay.layerId = newId;
  auto buf = std::make_shared<std::vector<uint8_t>>();
  buf->push_back((uint8_t)(newId >> 24)); buf->push_back((uint8_t)(newId >> 16));
  buf->push_back((uint8_t)(newId >> 8));  buf->push_back((uint8_t)(newId & 0xff));
  LayerExtraData &ex = lay.extraData;
  for (auto &a : ex.additionalLayers) {
    if (a.key == 'lyid') {
      delete a.data;
      a.data = new VectorReader(buf);
      a.size = 4;
      ex.useRawBytes = false;
      return;
    }
  }
  ex.additionalLayers.push_back(AdditionalLayerInfo(0, 'lyid', 4, new VectorReader(buf)));
  ex.useRawBytes = false;
}

}  // anonymous namespace

bool PSDFile::deleteLayer(int index) {
  if (index < 0 || index >= (int)layerList.size()) return false;
  layerList.erase(layerList.begin() + index);
  layersDirty = true;
  relinkGroups();          // 並びが変わったので親子関係を貼り直す
  return true;
}

bool PSDFile::moveLayer(int from, int to) {
  int n = (int)layerList.size();
  if (from < 0 || from >= n || to < 0 || to >= n) return false;
  if (from == to) return true;
  LayerInfo tmp = layerList[(size_t)from];   // deep copy (iterator を clone)
  layerList.erase(layerList.begin() + from);
  layerList.insert(layerList.begin() + to, tmp);
  layersDirty = true;
  relinkGroups();
  return true;
}

bool PSDFile::moveLayerRange(int from, int count, int to) {
  int n = (int)layerList.size();
  if (from < 0 || count <= 0 || from + count > n) return false;
  if (to < 0 || to > n) return false;
  if (to >= from && to <= from + count) return true;   // 自分の中への移動 = 何もしない

  std::vector<LayerInfo> block(layerList.begin() + from,
                               layerList.begin() + from + count);
  layerList.erase(layerList.begin() + from, layerList.begin() + from + count);
  // 取り除いたぶん挿入位置がずれる
  int dest = (to > from) ? to - count : to;
  if (dest < 0) dest = 0;
  if (dest > (int)layerList.size()) dest = (int)layerList.size();
  layerList.insert(layerList.begin() + dest, block.begin(), block.end());
  layersDirty = true;
  relinkGroups();
  return true;
}

// 同じ階層の隣の兄弟と入れ替える。
//
// layerList は下から上の順で、グループは [区切り][子...][フォルダ] という
// 連続した並び。自分の塊 (フォルダなら区切りごと) を求め、その外側にある
// 次の兄弟の塊を求めて、両者を入れ替える。
bool PSDFile::moveLayerSibling(int index, bool up, int *newIndexOut) {
  int n = (int)layerList.size();
  if (index < 0 || index >= n) return false;

  int myStart = 0, myCount = 0;
  if (!groupSpan(index, myStart, myCount)) return false;
  const int myEnd = myStart + myCount;            // 半開区間 [myStart, myEnd)
  const int parentIdx = layerList[(size_t)index].parentIndex;

  int sibStart = 0, sibCount = 0;
  if (up) {
    // 上 = layerList の後ろ側。自分の塊の直後を見る。
    int i = myEnd;
    if (i >= n) return false;                     // 文書の最上位で行き止まり
    LayerType t = layerList[(size_t)i].layerType;
    if (t == LAYER_TYPE_FOLDER && i == parentIdx) return false;  // 親の底に到達
    if (t == LAYER_TYPE_HIDDEN) {
      // 兄弟グループの入口。対応するフォルダまでが塊。
      int depth = 0;
      int j = i + 1;
      for (; j < n; j++) {
        LayerType u = layerList[(size_t)j].layerType;
        if (u == LAYER_TYPE_HIDDEN) depth++;
        else if (u == LAYER_TYPE_FOLDER) {
          if (depth == 0) break;
          depth--;
        }
      }
      if (j >= n) return false;                   // 壊れた構造
      sibStart = i;
      sibCount = j - i + 1;
    } else {
      sibStart = i;
      sibCount = 1;
    }
    // 自分の塊を兄弟の後ろへ
    if (!moveLayerRange(myStart, myCount, sibStart + sibCount)) return false;
    if (newIndexOut) *newIndexOut = index + sibCount;
  } else {
    // 下 = layerList の前側。自分の塊の直前を見る。
    int i = myStart - 1;
    if (i < 0) return false;                      // 文書の最下位で行き止まり
    LayerType t = layerList[(size_t)i].layerType;
    if (t == LAYER_TYPE_HIDDEN && parentIdx >= 0) return false;  // 親の天井に到達
    if (t == LAYER_TYPE_FOLDER) {
      int s2 = 0, c2 = 0;
      if (!groupSpan(i, s2, c2)) return false;
      sibStart = s2;
      sibCount = c2;
    } else {
      sibStart = i;
      sibCount = 1;
    }
    if (!moveLayerRange(myStart, myCount, sibStart)) return false;
    if (newIndexOut) *newIndexOut = index - sibCount;
  }
  return true;
}

int PSDFile::duplicateLayer(int index) {
  if (index < 0 || index >= (int)layerList.size()) return -1;
  LayerInfo copy = layerList[(size_t)index];
  assignFreshLayerId(copy, nextLayerId(layerList));  // Photoshop 同様、複製は新 ID
  layerList.insert(layerList.begin() + index + 1, copy);
  layersDirty = true;
  relinkGroups();          // 並びが変わったので親子関係を貼り直す
  return index + 1;
}

int PSDFile::copyLayerFrom(const PSDFile &src, int srcIndex, int destIndex) {
  if (srcIndex < 0 || srcIndex >= (int)src.layerList.size()) return -1;
  LayerInfo copy = src.layerList[(size_t)srcIndex]; // channel/extra は src を参照
  copy.owner  = this;   // owner はどこからも参照されないが整合のため付け替え
  copy.parent = nullptr;
  assignFreshLayerId(copy, nextLayerId(layerList)); // 取り込み先の文書内で一意な ID
  int pos = (destIndex < 0 || destIndex > (int)layerList.size())
              ? (int)layerList.size() : destIndex;
  layerList.insert(layerList.begin() + pos, copy);
  layersDirty = true;
  relinkGroups();
  return pos;
}

// --- 文書末尾の追加情報 (Txt2 など) ------------------------------------------
//
// レイヤ&マスク情報の末尾には、文書ぜんたいに効く追加情報ブロックが
//   '8BIM' | '8B64' + key(4) + length(4) + data + (4 の倍数への詰め物)
// の並びで置かれている。psdparse は普段ここを丸ごと素通しするが、Txt2
// (文書ぜんたいのテキストエンジン状態) だけは書き換え / 削除が要る。
// Photoshop は Txt2 をレイヤ毎の TySh より優先して読むため、TySh だけ直しても
// 編集が届かない。
namespace {

// trailing を先頭から辿って key のブロックを探す。見つかったら
//   blockOffset … ブロック先頭 ('8BIM' の位置)
//   blockTotal  … 詰め物まで含めたブロック長
//   dataOffset  … 中身の先頭
//   dataLength  … 中身の長さ
// を返す。
bool scanTrailingBlock(IteratorBase *t, int key, int &blockOffset, int &blockTotal,
                       int &dataOffset, int &dataLength) {
  if (!t) return false;
  t->init();
  const int total = t->size();
  int p = 0;
  while (p + 12 <= total) {
    uint8_t hdr[12];
    t->init();
    IteratorBase *h = t->cloneRange(p, 12);
    int got = h ? h->getData(hdr, 12) : 0;
    delete h;
    if (got != 12) return false;
    if (std::memcmp(hdr, "8BIM", 4) != 0 && std::memcmp(hdr, "8B64", 4) != 0) return false;
    int k = ((int)hdr[4] << 24) | ((int)hdr[5] << 16) | ((int)hdr[6] << 8) | (int)hdr[7];
    uint32_t len = ((uint32_t)hdr[8] << 24) | ((uint32_t)hdr[9] << 16) |
                   ((uint32_t)hdr[10] << 8) | (uint32_t)hdr[11];
    if (len > (uint32_t)(total - p - 12)) return false;
    int next = p + 12 + (int)len;
    next += (4 - (next % 4)) % 4;              // 4 の倍数へ詰める
    if (next > total) next = total;
    if (k == key) {
      blockOffset = p;
      blockTotal  = next - p;
      dataOffset  = p + 12;
      dataLength  = (int)len;
      return true;
    }
    p = next;
  }
  return false;
}

} // anonymous namespace

bool PSDFile::getDocumentAdditionalInfo(int key, std::string &out) {
  // すでに差し替え済みならそちらを返す (編集を積み重ねられるように)。
  if (trailingPatched && trailingPatchKey == key) {
    if (trailingPatchBytes.size() < 12) return false;   // 削除済み
    out.assign(trailingPatchBytes.begin() + 12, trailingPatchBytes.end());
    // 詰め物を落とす
    uint32_t len = ((uint32_t)(uint8_t)trailingPatchBytes[8]  << 24) |
                   ((uint32_t)(uint8_t)trailingPatchBytes[9]  << 16) |
                   ((uint32_t)(uint8_t)trailingPatchBytes[10] <<  8) |
                    (uint32_t)(uint8_t)trailingPatchBytes[11];
    if (len <= out.size()) out.resize(len);
    return true;
  }
  int bo, bt, dof, dlen;
  if (!scanTrailingBlock(layerAndMaskTrailing, key, bo, bt, dof, dlen)) return false;
  out.assign((size_t)dlen, '\0');
  if (dlen > 0) {
    layerAndMaskTrailing->init();
    IteratorBase *d = layerAndMaskTrailing->cloneRange(dof, dlen);
    int got = d ? d->getData(&out[0], dlen) : 0;
    delete d;
    if (got != dlen) return false;
  }
  return true;
}

bool PSDFile::setDocumentAdditionalInfo(int key, const char *data, size_t size) {
  int bo, bt, dof, dlen;
  if (!scanTrailingBlock(layerAndMaskTrailing, key, bo, bt, dof, dlen)) return false;
  if (trailingPatched && trailingPatchKey != key) return false;  // 差し替えは 1 キーまで

  std::string blk;
  if (data) {
    // '8BIM' + key + length + data + 詰め物
    blk.append("8BIM", 4);
    for (int i = 3; i >= 0; i--) blk.push_back((char)((key >> (i * 8)) & 0xff));
    uint32_t n = (uint32_t)size;
    for (int i = 3; i >= 0; i--) blk.push_back((char)((n >> (i * 8)) & 0xff));
    blk.append(data, size);
    // trailing 先頭からの位置が 4 の倍数になるよう詰める
    size_t endPos = (size_t)bo + blk.size();
    blk.append((4 - (endPos % 4)) % 4, '\0');
  }
  trailingPatched      = true;
  trailingPatchKey     = key;
  trailingPatchOffset  = bo;
  trailingPatchLength  = bt;
  trailingPatchBytes   = blk;
  return true;
}

bool PSDFile::removeDocumentAdditionalInfo(int key) {
  return setDocumentAdditionalInfo(key, 0, 0);
}

bool PSDFile::hasDocumentAdditionalInfo(int key) {
  if (trailingPatched && trailingPatchKey == key) return trailingPatchBytes.size() >= 12;
  int bo, bt, dof, dlen;
  return scanTrailingBlock(layerAndMaskTrailing, key, bo, bt, dof, dlen);
}

// --- Txt2 (文書ぜんたいの Text Engine Data) の追随 ----------------------------

bool PSDFile::getLayerTextIndex(int index, int &out) const {
  if (index < 0 || index >= (int)layerList.size()) return false;
  const LayerInfo &lay = layerList[(size_t)index];
  for (const auto &a : lay.extraData.additionalLayers) {
    if (a.key != 'TySh' || !a.data) continue;
    IteratorBase *rd = a.data->clone();
    rd->init();
    std::vector<uint8_t> prefix(56);
    if (rd->getData(prefix.data(), 56) != 56) { delete rd; return false; }
    Descriptor td;
    td.load(rd);
    delete rd;
    DescriptorInteger *ti = td.item("TextIndex");
    if (!ti) return false;
    out = ti->val;
    return true;
  }
  return false;
}

void PSDFile::setTextEngineDataPolicy(TextEngineDataPolicy p) {
  textPolicy_ = p;
  if (p == TEXTENGINE_REMOVE) dropTextEngineData();
}

bool PSDFile::dropTextEngineData() {
  if (!hasDocumentAdditionalInfo('Txt2')) return true;
  if (!removeDocumentAdditionalInfo('Txt2')) return false;
  textEngineDropped_ = true;
  return true;
}

// 書式 / 位置を変える編集は Txt2 へ写せない。KEEP を明示されていない限り
// Txt2 を落として TySh へフォールバックさせる。
void PSDFile::invalidateTextEngineData() {
  if (textPolicy_ == TEXTENGINE_KEEP) return;
  dropTextEngineData();
}

bool PSDFile::syncTextEngineData(int index, const u16str &newText,
                                 const std::vector<int> &paragraphLengths,
                                 const std::vector<int> &styleLengths) {
  if (textPolicy_ == TEXTENGINE_KEEP)   return true;
  if (textPolicy_ == TEXTENGINE_REMOVE) return dropTextEngineData();
  if (!hasDocumentAdditionalInfo('Txt2')) return true;   // 元から無い (旧い PSD)

  int textIndex = -1;
  std::string blob, out;
  // 追随できない条件に当たったら、黙って古いまま残すのではなく Txt2 を落として
  // TySh へフォールバックさせる。Photoshop に古い本文を見せるよりは安全。
  if (!getLayerTextIndex(index, textIndex))            return dropTextEngineData();
  if (!getDocumentAdditionalInfo('Txt2', blob))        return dropTextEngineData();
  if (!editTextEngineDataText(blob.data(), blob.size(), textIndex, newText,
                              paragraphLengths, styleLengths, out))
    return dropTextEngineData();
  return setDocumentAdditionalInfo('Txt2', out.data(), out.size());
}

bool PSDFile::loadFromStream(std::unique_ptr<std::istream> stream) {
  // clearData() を先にやってから ownedStream_ にセットする。
  // 順序を逆にすると委譲先の clearData が握ったばかりの stream を消す (旧バグ)。
  clearData();
  if (!stream) return false;
  ownedStream_ = std::move(stream);
  bool ok = parseFromStream_(*ownedStream_);
  if (!ok) clearData();
  return ok;
}

} // namespace psd
