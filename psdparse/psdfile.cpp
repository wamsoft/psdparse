
#include "psdparse.h"
#include "psdfile.h"
#include "psdwrite.h"
#include "psddesc.h"
#include "psdengine.h"

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
  // (kirikiri は iTJSBinaryStream をラップした StreamReader をここに渡す)
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

bool PSDFile::editTextLayer(int index,
                            const std::function<bool(const std::string &,
                                                     std::string &)> &editEngine,
                            const u16str *newTxt,
                            std::string *errorOut) {
  auto fail = [&](const char *msg) {
    if (errorOut) *errorOut = msg;
    return false;
  };
  if (index < 0 || index >= (int)layerList.size()) return fail("layer index out of range");
  if (!editEngine) return fail("no edit function given");

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

    auto *eng = dynamic_cast<DescriptorRawData *>(td.findItem("EngineData"));
    if (!eng) return fail("text layer has no EngineData");

    std::string newEngine;
    if (!editEngine(eng->bytes, newEngine)) return fail("failed to edit EngineData");
    eng->bytes = newEngine;

    if (newTxt) {
      if (auto *txt = dynamic_cast<DescriptorString *>(td.findItem("Txt ")))
        txt->val = *newTxt;
    }

    std::vector<uint8_t> buf;
    MemoryWriter w(buf);
    w.putData(prefix.data(), prefix.size());
    writeDescriptorBody(w, &td);
    if (!suffix.empty()) w.putData(suffix.data(), suffix.size());
    if (!setAdditionalInfoBytes(index, 'TySh', buf.data(), (int)buf.size()))
      return fail("could not store the rebuilt TySh block");

    // パース済みの textData も追随させる (再ロードなしで参照できるように)
    if (newTxt) lay.textData.text = *newTxt;
    return true;
  }
  return fail("layer is not a text layer (no TySh block)");
}

bool PSDFile::setLayerText(int index, const u16str &newText, std::string *errorOut) {
  return editTextLayer(index,
    [&](const std::string &in, std::string &out) {
      return editEngineDataText(in.data(), in.size(), newText, out);
    }, &newText, errorOut);
}

bool PSDFile::setLayerTextUtf8(int index, const char *utf8, std::string *errorOut) {
  return setLayerText(index, utf8ToU16(utf8 ? utf8 : ""), errorOut);
}

bool PSDFile::setLayerRunStyle(int index, int runIndex, const RunStyleEdit &edit,
                               std::string *errorOut) {
  return editTextLayer(index,
    [&](const std::string &in, std::string &out) {
      return editEngineDataRunStyle(in.data(), in.size(), runIndex, edit, out);
    }, nullptr, errorOut);
}

bool PSDFile::setLayerRichText(int index, const u16str &newText,
                               const std::vector<TextRunSpec> &runs,
                               const std::vector<TextParagraphSpec> &paragraphs,
                               std::string *errorOut) {
  bool ok = editTextLayer(index,
    [&](const std::string &in, std::string &out) {
      return editEngineDataRichText(in.data(), in.size(), newText, runs, paragraphs, out);
    }, &newText, errorOut);
  if (ok) {
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
  return true;
}

int PSDFile::duplicateLayer(int index) {
  if (index < 0 || index >= (int)layerList.size()) return -1;
  LayerInfo copy = layerList[(size_t)index];
  assignFreshLayerId(copy, nextLayerId(layerList));  // Photoshop 同様、複製は新 ID
  layerList.insert(layerList.begin() + index + 1, copy);
  layersDirty = true;
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
  return pos;
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
