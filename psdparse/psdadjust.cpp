// 調整レイヤのパラメータ。
//
// 公開仕様 "Adjustment layer" のバイナリ形式 (levl / curv / hue2 / blnc / selc /
// brit / thrs / post / nvrt / mixr / phfl / expA / grdm) と、descriptor 形式
// (vibA / blwh / CgEd / clrL) を読む。仕様に書かれていない細部 (Curves の
// 'Crv ' 追記部、Levels の 'Lvls' 追記部、Gradient Map の後半) は psd-tools の
// 読み方に合わせている。
#include "psdparse.h"

#include <cstring>
#include <vector>

namespace psd {

namespace {

// 境界チェック付きの読み取り
struct Rd {
  const std::vector<uint8_t> &b;
  size_t p = 0;
  bool ok = true;
  explicit Rd(const std::vector<uint8_t> &b) : b(b) {}
  bool need(size_t n) { if (p + n > b.size()) ok = false; return ok; }
  size_t rest() const { return p < b.size() ? b.size() - p : 0; }
  int u8() { if (!need(1)) return 0; return b[p++]; }
  int u16() { if (!need(2)) return 0; int v = (b[p] << 8) | b[p + 1]; p += 2; return v; }
  int s16() { return (int16_t)u16(); }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = ((uint32_t)b[p] << 24) | ((uint32_t)b[p + 1] << 16) | ((uint32_t)b[p + 2] << 8) | b[p + 3];
    p += 4;
    return v;
  }
  float f32() { pun32 v; v.i = u32(); return v.f; }
  std::string fourcc() { if (!need(4)) return ""; std::string s((const char*)&b[p], 4); p += 4; return s; }
  void skip(size_t n) { if (need(n)) p += n; }
  u16str unicode() {
    u16str s;
    uint32_t n = u32();
    if (!need((size_t)n * 2)) return s;
    for (uint32_t i = 0; i < n; i++) s.push_back((char16_t)u16());
    while (!s.empty() && s.back() == 0) s.pop_back();
    return s;
  }
};

bool readBlock(const AdditionalLayerInfo &a, std::vector<uint8_t> &out) {
  if (!a.data) return false;
  IteratorBase *r = a.data->clone();
  r->init();
  int n = r->size();
  out.resize(n > 0 ? (size_t)n : 0);
  int got = n > 0 ? r->getData(out.data(), n) : 0;
  delete r;
  return got == n;
}

void scalar(AdjustmentInfo &o, const char *k, double v) { o.scalars.push_back({k, v}); }
void array(AdjustmentInfo &o, const char *k, std::vector<double> v) { o.arrays.push_back({k, std::move(v)}); }
void table(AdjustmentInfo &o, const char *k, std::vector<std::vector<double>> v) { o.tables.push_back({k, std::move(v)}); }

bool decodeLevels(Rd &r, AdjustmentInfo &o) {
  if (r.u16() != 2) return false;
  auto rec = [&]() {
    std::vector<double> row;
    row.push_back(r.u16()); row.push_back(r.u16());   // 入力の黒 / 白
    row.push_back(r.u16()); row.push_back(r.u16());   // 出力の黒 / 白
    row.push_back(r.u16() / 100.0);                   // ガンマ (x100 で格納)
    return row;
  };
  std::vector<std::vector<double>> rows;
  for (int i = 0; i < 29; i++) rows.push_back(rec());
  // 追記部 'Lvls' + version(3) + 総数: 29 件を超えるチャンネル (多チャンネル文書)
  if (r.ok && r.rest() >= 8 && std::memcmp(&r.b[r.p], "Lvls", 4) == 0) {
    r.skip(4);
    r.u16();
    int count = r.u16();
    for (int i = 29; i < count && r.ok; i++) rows.push_back(rec());
  }
  table(o, "records", rows);
  return r.ok;
}

bool decodeCurves(Rd &r, AdjustmentInfo &o) {
  const bool isMap = r.u8() != 0;
  const int version = r.u16();
  const uint32_t countMap = r.u32();
  if (version != 1 && version != 4) return false;
  scalar(o, "version", version);
  scalar(o, "is_map", isMap ? 1 : 0);
  int count = 0;
  std::vector<double> channels;
  if (version == 1) {
    for (int bit = 0; bit < 32; bit++)
      if (countMap & (1u << bit)) { count++; channels.push_back(bit); }
  } else {
    count = (int)countMap;
    for (int i = 0; i < count; i++) channels.push_back(i);
  }
  std::vector<std::vector<double>> curves;
  for (int c = 0; c < count && r.ok; c++) {
    std::vector<double> pts;
    if (isMap) {
      for (int i = 0; i < 256; i++) pts.push_back(r.u8());
    } else {
      int n = r.u16();
      if (n < 0 || n > 64) return false;
      for (int i = 0; i < n; i++) {
        int out = r.u16(), in = r.u16();          // 格納は (出力, 入力) の順
        pts.push_back(in); pts.push_back(out);
      }
    }
    curves.push_back(pts);
  }
  // version 1 の後ろの 'Crv ' 追記部はチャンネル番号を明示している。あればそちら。
  if (version == 1 && r.ok && r.rest() >= 10 && std::memcmp(&r.b[r.p], "Crv ", 4) == 0) {
    r.skip(4);
    r.u16();
    uint32_t n = r.u32();
    std::vector<std::vector<double>> c2;
    std::vector<double> ch2;
    for (uint32_t i = 0; i < n && r.ok && i < 64; i++) {
      ch2.push_back(r.u16());
      std::vector<double> pts;
      if (isMap) {
        for (int k = 0; k < 256; k++) pts.push_back(r.u8());
      } else {
        int m = r.u16();
        if (m < 0 || m > 64) return false;
        for (int k = 0; k < m; k++) { int out = r.u16(), in = r.u16(); pts.push_back(in); pts.push_back(out); }
      }
      c2.push_back(pts);
    }
    if (r.ok) { curves.swap(c2); channels.swap(ch2); }
  }
  array(o, "channels", channels);
  table(o, isMap ? "maps" : "points", curves);
  return r.ok;
}

bool decodeHueSat(Rd &r, AdjustmentInfo &o) {
  if (r.u16() != 2) return false;
  scalar(o, "colorize", r.u8()); r.skip(1);
  array(o, "colorization", { (double)r.s16(), (double)r.s16(), (double)r.s16() });
  array(o, "master", { (double)r.s16(), (double)r.s16(), (double)r.s16() });
  std::vector<std::vector<double>> rows;
  for (int i = 0; i < 6; i++) {
    std::vector<double> row;
    for (int k = 0; k < 7; k++) row.push_back(r.s16());   // 範囲 4 + 色相 / 彩度 / 明度
    rows.push_back(row);
  }
  table(o, "ranges", rows);
  return r.ok;
}

bool decodeGradientMap(Rd &r, AdjustmentInfo &o) {
  const int version = r.u16();
  if (version != 1 && version != 3) return false;
  scalar(o, "version", version);
  scalar(o, "reversed", r.u8());
  scalar(o, "dithered", r.u8());
  if (version == 3) o.text.push_back({"method", r.fourcc()});
  o.unicode.push_back({"name", r.unicode()});
  std::vector<std::vector<double>> cs, ts;
  int n = r.u16();
  for (int i = 0; i < n && r.ok; i++) {
    std::vector<double> row;
    row.push_back(r.u32()); row.push_back(r.u32()); row.push_back(r.u16());   // 位置 / 中間点 / 種類
    for (int k = 0; k < 4; k++) row.push_back(r.u16());                      // 色 (色空間ぶん)
    r.skip(2);
    cs.push_back(row);
  }
  n = r.u16();
  for (int i = 0; i < n && r.ok; i++)
    ts.push_back({ (double)r.u32(), (double)r.u32(), (double)r.u16() });    // 位置 / 中間点 / 不透明度
  table(o, "color_stops", cs);
  table(o, "transparency_stops", ts);
  scalar(o, "expansion", r.u16());
  scalar(o, "interpolation", r.u16());
  scalar(o, "length", r.u16());
  scalar(o, "mode", r.u16());
  scalar(o, "random_seed", r.u32());
  scalar(o, "show_transparency", r.u16());
  scalar(o, "use_vector_color", r.u16());
  scalar(o, "roughness", r.u32());
  scalar(o, "color_model", r.u16());
  array(o, "minimum_color", { (double)r.u16(), (double)r.u16(), (double)r.u16(), (double)r.u16() });
  array(o, "maximum_color", { (double)r.u16(), (double)r.u16(), (double)r.u16(), (double)r.u16() });
  return r.ok;
}

bool decodeBinary(int key, Rd &r, AdjustmentInfo &o) {
  switch (key) {
  case 'levl': o.type = "levels";              return decodeLevels(r, o);
  case 'curv': o.type = "curves";              return decodeCurves(r, o);
  case 'hue2':
  case 'hue ': o.type = "hue_saturation";      return decodeHueSat(r, o);
  case 'grdm': o.type = "gradient_map";        return decodeGradientMap(r, o);
  case 'brit':
    o.type = "brightness_contrast";
    scalar(o, "brightness", r.s16());
    scalar(o, "contrast", r.s16());
    scalar(o, "mean", r.u16());
    scalar(o, "lab_only", r.u8());
    return r.ok;
  case 'blnc':
    o.type = "color_balance";
    array(o, "shadows",    { (double)r.s16(), (double)r.s16(), (double)r.s16() });
    array(o, "midtones",   { (double)r.s16(), (double)r.s16(), (double)r.s16() });
    array(o, "highlights", { (double)r.s16(), (double)r.s16(), (double)r.s16() });
    scalar(o, "preserve_luminosity", r.u8());
    return r.ok;
  case 'selc': {
    o.type = "selective_color";
    r.u16();
    scalar(o, "method", r.u16());          // 0 相対 / 1 絶対
    std::vector<std::vector<double>> rows;
    for (int i = 0; i < 10; i++)
      rows.push_back({ (double)r.s16(), (double)r.s16(), (double)r.s16(), (double)r.s16() });
    table(o, "records", rows);             // [0] は未使用、以降 レッド系 .. ブラック系
    return r.ok;
  }
  case 'thrs': o.type = "threshold"; scalar(o, "level", r.u16()); return r.ok;
  case 'post': o.type = "posterize"; scalar(o, "levels", r.u16()); return r.ok;
  case 'nvrt': o.type = "invert"; return true;
  case 'mixr': {
    o.type = "channel_mixer";
    r.u16();
    scalar(o, "monochrome", r.u16());
    std::vector<std::vector<double>> rows;
    while (r.rest() >= 10) {
      std::vector<double> row;
      for (int k = 0; k < 5; k++) row.push_back(r.s16());   // 入力チャンネルごとの % + 定数
      rows.push_back(row);
    }
    table(o, "channels", rows);
    return r.ok;
  }
  case 'phfl': {
    o.type = "photo_filter";
    const int version = r.u16();
    scalar(o, "version", version);
    if (version == 3) {
      array(o, "xyz", { (double)(int32_t)r.u32(), (double)(int32_t)r.u32(), (double)(int32_t)r.u32() });
    } else if (version == 2) {
      scalar(o, "color_space", r.u16());
      array(o, "color", { (double)r.u16(), (double)r.u16(), (double)r.u16(), (double)r.u16() });
    } else {
      return false;
    }
    scalar(o, "density", r.u32());
    scalar(o, "preserve_luminosity", r.u8());
    return r.ok;
  }
  case 'expA':
    o.type = "exposure";
    r.u16();
    scalar(o, "exposure", r.f32());
    scalar(o, "offset", r.f32());
    scalar(o, "gamma", r.f32());
    return r.ok;
  default:
    return false;
  }
}

}  // anonymous namespace

bool decodeAdjustment(const LayerInfo &layer, AdjustmentInfo &out) {
  // descriptor 形式: (キー, 種別名, descriptor の手前の読み飛ばし量)
  static const struct { int key; const char *type; int skip; } kDesc[] = {
    { 'vibA', "vibrance",            4 },
    { 'blwh', "black_white",         4 },
    { 'clrL', "color_lookup",        6 },
  };
  for (const auto &a : layer.extraData.additionalLayers) {
    std::vector<uint8_t> buf;
    for (const auto &d : kDesc) {
      if (a.key != d.key) continue;
      if (!readBlock(a, buf) || (int)buf.size() < d.skip) return false;
      out = AdjustmentInfo();
      out.key = a.key;
      out.type = d.type;
      MemoryReader mr(buf.data() + d.skip, (int)buf.size() - d.skip);
      out.descriptor = std::make_shared<Descriptor>();
      if (!out.descriptor->load(&mr)) out.descriptor.reset();
      return true;
    }
    switch (a.key) {
    case 'levl': case 'curv': case 'hue2': case 'hue ': case 'grdm': case 'brit':
    case 'blnc': case 'selc': case 'thrs': case 'post': case 'nvrt': case 'mixr':
    case 'phfl': case 'expA': {
      if (!readBlock(a, buf)) return false;
      out = AdjustmentInfo();
      out.key = a.key;
      Rd r(buf);
      out.valid = decodeBinary(a.key, r, out);
      // 新しい明るさ・コントラストは CgEd (descriptor) に実際の値を持つ
      if (a.key == 'brit') {
        for (const auto &c : layer.extraData.additionalLayers) {
          if (c.key != 'CgEd') continue;
          std::vector<uint8_t> cb;
          if (readBlock(c, cb) && cb.size() > 4) {
            MemoryReader mr(cb.data() + 4, (int)cb.size() - 4);
            out.descriptor = std::make_shared<Descriptor>();
            if (!out.descriptor->load(&mr)) out.descriptor.reset();
          }
        }
      }
      return true;
    }
    default:
      break;
    }
  }
  return false;
}

}  // namespace psd
