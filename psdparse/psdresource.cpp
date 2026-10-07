
#include "psddata.h"
#include "psdresource.h"
#include "psddesc.h"

namespace psd {
  // スライス情報をロード
  bool loadResourceSlice(Data &data, ImageResourceInfo &res)
  {
    int version = res.data->getInt32();
    if (version == 6) {
      data.slice.boundingLeft   = res.data->getInt32();
      data.slice.boundingTop    = res.data->getInt32();
      data.slice.boundingRight  = res.data->getInt32();
      data.slice.boundingBottom = res.data->getInt32();
      res.data->getUnicodeString(data.slice.groupName);
      int sliceNum = res.data->getInt32();
      // 件数は壊れていることがある。データが尽きたら止める (尽きた後も件数ぶん
      // 空の項目を足し続けると、巨大な件数でメモリを使い切る)。
      for (int i = 0; i < sliceNum && res.data->rest() > 0; i++) {
        data.slice.slices.push_back(SliceItem());
        SliceItem &item = data.slice.slices.back();

        item.id = res.data->getInt32();
        item.groupId = res.data->getInt32();
        item.origin = res.data->getInt32();
        if (item.origin == 1) {
          item.associatedLayerId = res.data->getInt32();
        } else {
          item.associatedLayerId = -1; // 多分０でいいはずだけどドキュメントないので…
        }
        res.data->getUnicodeString(item.name);
        item.type   = res.data->getInt32();
        item.left   = res.data->getInt32();
        item.top    = res.data->getInt32();
        item.right  = res.data->getInt32();
        item.bottom = res.data->getInt32();
        res.data->getUnicodeString(item.url);
        res.data->getUnicodeString(item.target);
        res.data->getUnicodeString(item.message);
        res.data->getUnicodeString(item.altTag);
        item.isCellTextHtml = (res.data->getCh() != 0);
        res.data->getUnicodeString(item.cellText);
        item.horizontalAlign = res.data->getInt32();
        item.verticalAlign   = res.data->getInt32();
        item.colorA = res.data->getCh();
        item.colorR = res.data->getCh();
        item.colorG = res.data->getCh();
        item.colorB = res.data->getCh();
      }
      data.slice.isEnabled = true;
      data.slice.version = version;

      // additional descriptor resource
      Descriptor dsc;
      if (!res.data->eoi() &&
          16 == res.data->getInt32() &&
          dsc.load(res.data)) {
        // 層ベースのスライスの外側の余白などが入っている (未使用)
      }
    } else if (version == 7 || version == 8) {
      // v7/v8 はディスクリプタ形式で格納されている
      // baseName / bounds / slices[] を v6 と同じ SliceResource へ写す。
      int ver = res.data->getInt32();
      if (ver == 16) {
        Descriptor dsc;
        dsc.load(res.data);   // 途中まででも読めた項目は使う
        auto str = [](Descriptor *d, const char *k) -> u16str {
          auto *s = d ? dynamic_cast<DescriptorString*>(d->item(k).find()) : 0;
          if (!s) return u16str();
          u16str v = s->val;
          while (!v.empty() && v.back() == 0) v.pop_back();
          return v;
        };
        auto num = [](Descriptor *d, const char *k, int def) -> int {
          DescriptorItem *it = d ? d->item(k).find() : 0;
          if (auto *n = dynamic_cast<DescriptorInteger*>(it)) return n->val;
          if (auto *f = dynamic_cast<DescriptorDouble*>(it)) return (int)f->val;
          if (auto *u = dynamic_cast<DescriptorUnitFloat*>(it)) return (int)u->val;
          return def;
        };
        auto en = [](Descriptor *d, const char *k) -> std::string {
          auto *e = d ? dynamic_cast<DescriptorEnumerated*>(d->item(k).find()) : 0;
          return e ? e->enumId : std::string();
        };
        auto rect = [&](Descriptor *b, int &l, int &t, int &r, int &bt) {
          l = num(b, "Left", 0); t = num(b, "Top ", 0); r = num(b, "Rght", 0); bt = num(b, "Btom", 0);
        };
        SliceResource &s = data.slice;
        s.version = version;
        s.groupName = str(&dsc, "baseName");
        rect(dynamic_cast<Descriptor*>(dsc.item("bounds").find()),
             s.boundingLeft, s.boundingTop, s.boundingRight, s.boundingBottom);
        if (auto *list = dynamic_cast<DescriptorList*>(dsc.item("slices").find())) {
          for (auto *it : list->items) {
            auto *d = dynamic_cast<Descriptor*>(it);
            if (!d) continue;
            SliceItem item = SliceItem();
            item.id      = num(d, "sliceID", 0);
            item.groupId = num(d, "groupID", 0);
            const std::string origin = en(d, "origin");
            item.origin = origin == "layerGenerated" ? 1 : origin == "userGenerated" ? 2 : 0;
            item.associatedLayerId = num(d, "layerID", -1);
            item.name = str(d, "Nm  ");
            const std::string type = en(d, "Type");
            item.type = type == "noImage" ? 0 : type == "Tbl " ? 2 : 1;
            rect(dynamic_cast<Descriptor*>(d->item("bounds").find()),
                 item.left, item.top, item.right, item.bottom);
            item.url      = str(d, "url");
            item.target   = str(d, "null");
            item.message  = str(d, "Msge");
            item.altTag   = str(d, "altTag");
            auto *html = dynamic_cast<DescriptorBoolean*>(d->item("cellTextIsHTML").find());
            item.isCellTextHtml = html ? html->val : true;
            item.cellText = str(d, "cellText");
            const std::string ha = en(d, "horzAlign"), va = en(d, "vertAlign");
            item.horizontalAlign = ha == "Left" ? 1 : ha == "Cntr" ? 2 : ha == "Rght" ? 3 : 0;
            item.verticalAlign   = va == "Top " ? 1 : va == "Cntr" ? 2 : va == "Bsln" ? 3 : va == "Btom" ? 4 : 0;
            item.colorA = item.colorR = item.colorG = item.colorB = 0;
            if (auto *c = dynamic_cast<Descriptor*>(d->item("bgColor").find())) {
              item.colorA = (uint8_t)num(c, "alpha", 0);
              item.colorR = (uint8_t)num(c, "Rd  ", 0);
              item.colorG = (uint8_t)num(c, "Grn ", 0);
              item.colorB = (uint8_t)num(c, "Bl  ", 0);
            }
            s.slices.push_back(item);
          }
        }
        s.isEnabled = true;
      }
    }

    return true;
  }

  // グリッド/ガイド情報をロード
  bool loadResourceGridAndGuide(Data &data, ImageResourceInfo &res)
  {
    // 読み捨て
    int version = res.data->getInt32();

    data.gridGuide.horizontalGrid = res.data->getInt32();
    data.gridGuide.verticalGrid   = res.data->getInt32();
    int guideNum = res.data->getInt32();
    for (int i = 0; i < guideNum && res.data->rest() >= 5; i++) {   // 1 件 5 バイト
      data.gridGuide.guides.push_back(GuideItem());
      GuideItem &item = data.gridGuide.guides.back();

      item.location  = res.data->getInt32();
      item.direction = (GuideDirection)res.data->getCh();
    }
    data.gridGuide.isEnabled = true;

    return true;
  }

  // インデックスカラーテーブルカウント
  bool loadResourceColorTableCount(Data &data, ImageResourceInfo &res)
  {
    data.colorTable.validCount = res.data->getInt16();
    return true;
  }

  // 透明インデックス
  bool loadResourceTransparencyIndex(Data &data, ImageResourceInfo &res)
  {
    data.colorTable.transparencyIndex = res.data->getInt16();
    return true;
  }

  // レイヤーカンプ
  bool loadResourceLayerComps(Data &data, ImageResourceInfo &res)
  {
    int ver = res.data->getInt32();
    if (ver == 16) {
      Descriptor dsc;
      if (dsc.load(res.data)) {
        // dsc.dump();
        DescriptorInteger *lastApplied = dsc.item("lastAppliedComp");
        data.lastAppliedCompId = lastApplied ? lastApplied->val : -1;

        DescriptorList *compList = dsc.item("list");
        if (compList) {
          for (int i = 0; i < (int)compList->itemCount(); i++) {
            Descriptor *comp = compList->item(i);
            if (comp) {
              DescriptorInteger *compId = comp->item("compID");
              if (compId) {
                LayerComp lc;
                lc.id = compId->val;

                DescriptorInteger *capturedInfo = comp->item("capturedInfo");
                if (capturedInfo) {
                  lc.isRecordVisibility = ((capturedInfo->val & (1 << 0)) != 0);
                  lc.isRecordPosition   = ((capturedInfo->val & (1 << 1)) != 0);
                  lc.isRecordAppearance = ((capturedInfo->val & (1 << 2)) != 0);
                } else {
                  lc.isRecordVisibility = false;
                  lc.isRecordPosition   = false;
                  lc.isRecordAppearance = false;
                }

                DescriptorString *name = comp->item("Nm  ");
                if (name) {
                  lc.name = name->val;
                }

                DescriptorString *comment = comp->item("comment");
                if (comment) {
                  lc.comment = comment->val;
                }

                data.layerComps.push_back(lc);
              }
            }
          }
        }
        return true;
      }
    }
    // assert(false);
    return false;
  }

} // namespace psd

