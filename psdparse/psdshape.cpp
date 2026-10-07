// シェイプレイヤの情報: 塗り ('vscg')、線 ('vstk')、ライブシェイプの元の形 ('vogk')。
//
// どれも descriptor で、'vstk' は版 4 バイト、'vscg' は塗りの種類 4 バイト + 版
// 4 バイト、'vogk' は版 4 バイト + データ版 4 バイトのあとに続く。パスそのものは
// ベクタマスク ('vsms' / 'vmsk', LayerInfo::vectorMask) にある。
#include "psdparse.h"

#include <cstring>
#include <vector>

namespace psd {

namespace {

std::shared_ptr<Descriptor> loadBlock(const LayerInfo &l, int key, int skip, int *head = nullptr) {
  for (const auto &a : l.extraData.additionalLayers) {
    if (a.key != key || !a.data) continue;
    IteratorBase *r = a.data->clone();
    r->init();
    const int n = r->size();
    std::vector<uint8_t> buf(n > 0 ? (size_t)n : 0);
    const int got = n > 0 ? r->getData(buf.data(), n) : 0;
    delete r;
    if (got != n || n <= skip) return nullptr;
    if (head && n >= 4) *head = (int)((uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3]);
    MemoryReader mr(buf.data() + skip, n - skip);
    auto d = std::make_shared<Descriptor>();
    if (!d->load(&mr)) return nullptr;
    return d;
  }
  return nullptr;
}

double num(Descriptor *d, const char *k, double def = 0.0, int *unit = nullptr) {
  DescriptorItem *it = d ? d->item(k).find() : nullptr;
  if (auto *v = dynamic_cast<DescriptorDouble*>(it)) return v->val;
  if (auto *v = dynamic_cast<DescriptorUnitFloat*>(it)) { if (unit) *unit = v->unit; return v->val; }
  if (auto *v = dynamic_cast<DescriptorInteger*>(it)) return v->val;
  return def;
}

bool flag(Descriptor *d, const char *k, bool def) {
  auto *b = d ? dynamic_cast<DescriptorBoolean*>(d->item(k).find()) : nullptr;
  return b ? b->val : def;
}

std::string enumOf(Descriptor *d, const char *k) {
  auto *e = d ? dynamic_cast<DescriptorEnumerated*>(d->item(k).find()) : nullptr;
  return e ? e->enumId : std::string();
}

// 'unitValueQuadVersion' 形式の矩形 / 角丸の半径
void quad(Descriptor *d, const char *a, const char *b, const char *c, const char *e, double out[4]) {
  out[0] = num(d, a); out[1] = num(d, b); out[2] = num(d, c); out[3] = num(d, e);
}

// 塗りの descriptor の種類 (持っているキーで決める)
int contentKind(Descriptor *d) {
  if (!d) return 0;
  if (d->item("Grad").find()) return 'GdFl';
  if (d->item("Ptrn").find()) return 'PtFl';
  if (d->item("Clr ").find()) return 'SoCo';
  return 0;
}

}  // anonymous namespace

bool decodeShape(const LayerInfo &layer, ShapeInfo &out) {
  out = ShapeInfo();
  int head = 0;
  if (auto d = loadBlock(layer, 'vscg', 8, &head)) {
    out.hasFill = true;
    out.fillKind = head;
    out.fill = d;
  }
  if (auto d = loadBlock(layer, 'vstk', 4)) {
    ShapeStroke &s = out.stroke;
    out.hasStroke = true;
    s.descriptor = d;
    s.strokeEnabled = flag(d.get(), "strokeEnabled", true);
    s.fillEnabled = flag(d.get(), "fillEnabled", true);
    int unit = 0;
    s.width = num(d.get(), "strokeStyleLineWidth", 1.0, &unit);
    s.widthInPoints = unit == UNIT_POINTS;
    s.resolution = num(d.get(), "strokeStyleResolution", 72.0);
    const std::string al = enumOf(d.get(), "strokeStyleLineAlignment");
    s.alignment = al == "strokeStyleAlignOutside" ? 0 : al == "strokeStyleAlignInside" ? 1 : 2;
    const std::string cap = enumOf(d.get(), "strokeStyleLineCapType");
    s.cap = cap == "strokeStyleRoundCap" ? 1 : cap == "strokeStyleSquareCap" ? 2 : 0;
    const std::string join = enumOf(d.get(), "strokeStyleLineJoinType");
    s.join = join == "strokeStyleRoundJoin" ? 1 : join == "strokeStyleBevelJoin" ? 2 : 0;
    s.miterLimit = num(d.get(), "strokeStyleMiterLimit", 100.0);
    if (auto *ds = dynamic_cast<DescriptorList*>(d->item("strokeStyleLineDashSet").find())) {
      for (auto *it : ds->items) {
        if (auto *v = dynamic_cast<DescriptorUnitFloat*>(it)) s.dashes.push_back(v->val);
        else if (auto *v = dynamic_cast<DescriptorDouble*>(it)) s.dashes.push_back(v->val);
      }
    }
    s.dashOffset = num(d.get(), "strokeStyleLineDashOffset");
    s.opacity = num(d.get(), "strokeStyleOpacity", 100.0) / 100.0;
    s.blendMode = enumOf(d.get(), "strokeStyleBlendMode");
    if (auto *c = dynamic_cast<Descriptor*>(d->item("strokeStyleContent").find())) {
      s.content = std::shared_ptr<Descriptor>(d, c);   // vstk 全体と寿命を共にする
      s.contentKind = contentKind(c);
    }
  }
  if (auto d = loadBlock(layer, 'vogk', 8)) {
    out.originDescriptor = d;
    if (auto *list = dynamic_cast<DescriptorList*>(d->item("keyDescriptorList").find())) {
      for (auto *it : list->items) {
        auto *o = dynamic_cast<Descriptor*>(it);
        if (!o) continue;
        ShapeOrigin so;
        so.type = (int)num(o, "keyOriginType", 0);
        so.index = (int)num(o, "keyOriginIndex", 0);
        so.resolution = num(o, "keyOriginResolution", 0);
        if (auto *b = dynamic_cast<Descriptor*>(o->item("keyOriginShapeBBox").find())) {
          so.hasBox = true;
          quad(b, "Left", "Top ", "Rght", "Btom", so.box);
        }
        if (auto *r = dynamic_cast<Descriptor*>(o->item("keyOriginRRectRadii").find())) {
          so.hasRadii = true;
          quad(r, "topLeft", "topRight", "bottomRight", "bottomLeft", so.radii);
        }
        auto *ls = dynamic_cast<Descriptor*>(o->item("keyOriginLineStart").find());
        auto *le = dynamic_cast<Descriptor*>(o->item("keyOriginLineEnd").find());
        if (ls && le) {
          so.hasLine = true;
          so.line[0] = num(ls, "Hrzn"); so.line[1] = num(ls, "Vrtc");
          so.line[2] = num(le, "Hrzn"); so.line[3] = num(le, "Vrtc");
          so.lineWeight = num(o, "keyOriginLineWeight", 1.0);
        }
        so.invalidated = flag(o, "keyShapeInvalidated", false);
        out.origins.push_back(so);
      }
    }
  }
  out.present = out.hasFill || out.hasStroke || out.originDescriptor != nullptr;
  return out.present;
}

}  // namespace psd
