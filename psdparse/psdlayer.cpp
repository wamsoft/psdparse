
#include "psddata.h"
#include "psddesc.h"
#include "psdengine.h"
#include <cstring>

namespace psd {
  bool loadLayerSectionDivider(LayerInfo &layer, AdditionalLayerInfo &additional)
  {
    int type = additional.data->getInt32();
    switch (type) {
    default:
    case 0: // フォルダ関係以外の場合は他で適切なものがセットされるので無視
      break;
    case 1:
    case 2:
      layer.layerType = LAYER_TYPE_FOLDER;
      break;
    case 3:
      layer.layerType = LAYER_TYPE_HIDDEN;
      break;
    }

    if (additional.size >= 12) {
      int signature = additional.data->getInt32();      // 読み捨て
      layer.blendMode = blendKeyToMode(additional.data->getInt32());
      if (additional.size >= 16) {
        int subType = additional.data->getInt32();      // 読み捨て
      }
    }
    return true;
  }

  bool loadLayerUnicodeName(LayerInfo &layer, AdditionalLayerInfo &additional)
  {
    additional.data->getUnicodeString(layer.layerNameUnicode);
    return true;
  }

  bool loadLayerId(LayerInfo &layer, AdditionalLayerInfo &additional)
  {
    layer.layerId = additional.data->getInt32();
    return true;
  }

  bool loadLayerFillOpacity(LayerInfo &layer, AdditionalLayerInfo &additional)
  {
    layer.fill_opacity = additional.data->getCh();
    return true;
  }
  
  bool loadLayerMetadata(LayerInfo &layer, AdditionalLayerInfo &additional)
  {
    int count = additional.data->getInt32();
    for (int i = 0; i < count; i++) {
      int signature = additional.data->getInt32();
      int key = additional.data->getInt32();
      bool copyOnSheetDup = (additional.data->getCh() != 0);
      additional.data->advance(3);
      int len = additional.data->getInt32();

      // 各メタデータ項目は正確に len バイト。処理後にこの位置へ整列させないと、
      // cmls より前に別項目 (未処理) があると読み位置がずれて descriptor が壊れる。
      int dataStart = additional.data->size() - additional.data->rest();

      // レイヤーカンプ
      if (key == 'cmls') {
        int ver = additional.data->getInt32();
        if (ver == 16) {
          Descriptor &dsc = layer.layerCompDesc;
          if (dsc.load(additional.data)) {
            DescriptorList *settings = (DescriptorList*)dsc.findItem("layerSettings");
            if (settings) {
              for (int s = 0; s < (int)settings->items.size(); s++) {
                Descriptor *comp = settings->item(s);
                DescriptorList *compList = comp->item("compList");
                if (!compList || compList->items.empty()) continue;
                LayerCompInfo ci;
                memset(&ci, 0, sizeof(ci));
                ci.id = ((DescriptorInteger*)compList->items[0])->val;

                Descriptor *offset = comp->item("Ofst");
                if (offset) {
                  DescriptorInteger *h = offset->item("Hrzn");
                  DescriptorInteger *v = offset->item("Vrtc");
                  ci.offsetX = (h) ? h->val : 0;
                  ci.offsetY = (v) ? v->val : 0;
                }

                // enab が無いレイヤ (この comp で可視状態を上書きしていない) は
                // レイヤの基準可視状態を採用する。
                DescriptorBoolean *enable = comp->item("enab");
                ci.isEnabled = enable ? enable->val : layer.isVisible();

                layer.layerComps[ci.id] = ci;
              }
            }
          }
        }
      }

      // 次項目のために len バイト境界へ整列 (未処理項目や余剰も読み飛ばす)。
      int consumed = (additional.data->size() - additional.data->rest()) - dataStart;
      if (consumed < len) additional.data->advance(len - consumed);
    }

    return true;
  }

  // 'TySh' Type tool object setting (Photoshop 6.0+)。
  //   version(2) transform(double*6) textVer(2) descVer(4) <text descriptor>
  //   warpVer(2) descVer(4) <warp descriptor> left top right bottom
  // text descriptor の 'Txt ' に本文、'EngineData'(tdta) にラン単位スタイルが入る。
  bool loadLayerTypeTool(LayerInfo &layer, AdditionalLayerInfo &additional)
  {
    IteratorBase *r = additional.data;
    TextLayerData &td = layer.textData;

    int version = r->getInt16(); // = 1
    (void)version;
    for (int i = 0; i < 6; i++) {
      pun64 v;
      v.i = r->getInt64();
      td.transform[i] = v.f;
    }
    int textVer = r->getInt16(); // = 50
    (void)textVer;
    int descVer = r->getInt32(); // = 16
    (void)descVer;

    Descriptor text;
    if (!text.load(r)) {
      // 途中まで読めていれば itemMap には有効な項目が入っている
    }

    // 本文 ('Txt ' — キー末尾に空白)。EngineData が取れれば後で上書きされる。
    DescriptorString *txt = text.item("Txt ");
    if (txt) td.text = txt->val;

    // 縦横 ('Ornt' enum: Hrzn / Vrtc)
    DescriptorEnumerated *ornt = text.item("Ornt");
    td.orientation = (ornt && ornt->enumId == "Vrtc") ? "vertical" : "horizontal";

    // ラン単位スタイル (EngineData)
    DescriptorRawData *eng = text.item("EngineData");
    if (eng && !eng->bytes.empty()) {
      parseEngineData(eng->bytes.data(), eng->bytes.size(), td);
    }

    td.present = true;
    layer.layerType = LAYER_TYPE_TEXT;
    return true;
  }

} // namespace psd
