
#include "psddesc.h"

#include <algorithm>

namespace psd {

  // --------------------------------------------------------------------------
  // ユーティリティ
  // --------------------------------------------------------------------------

  // 4バイトキー or stringタイプのID取得用ユーティリティ
  // 戻り値: 4 文字の ID が「長さ 4 を明示」で書かれていたか (書き戻しで再現する。
  // DescriptorItem::idLongMask を参照)
  static bool readId(IteratorBase *data, std::string &id)
  {
    int idSize = data->getInt32();
    const bool explicitFour = (idSize == 4);
    if (idSize == 0) {
      idSize = 4;
    }
    // 壊れた長さ (負 / 残りより長い) は読めないので、残りを捨てて空 ID にする。
    // 以降の読み取りは型 0 になり、呼び出し側が「解釈不能」として打ち切る。
    if (idSize < 0 || idSize > data->rest()) {
      data->advance(data->rest());
      id.clear();
      return false;
    }
    std::vector<char> buf(idSize+1);
    data->getData(&buf[0], idSize);
    buf[idSize] = '\0';
    id.assign(&buf[0]);
    return explicitFour;
  }

  // タイプにしたがってアイテム取得をディスパッチ
  static DescriptorItem *readItem(IteratorBase *data)
  {
    DescriptorItem *item = 0;
    DescriptorType type = (DescriptorType)data->getInt32();
    switch (type) {
    case 'obj ': item = new DescriptorReference();   break;
    case 'GlbO':
    case 'Objc': item = new Descriptor(type);        break;
    case 'VlLs': item = new DescriptorList();        break;
    case 'doub': item = new DescriptorDouble();      break;
    case 'UntF': item = new DescriptorUnitFloat();   break;
    case 'TEXT': item = new DescriptorString();      break;
    case 'enum': item = new DescriptorEnumerated();  break;
    case 'long': item = new DescriptorInteger();     break;
    case 'bool': item = new DescriptorBoolean();     break;
    case 'type':
    case 'GlbC': item = new DescriptorClass(type);   break;
    case 'alis': item = new DescriptorAlias();       break;
    case 'tdta': item = new DescriptorRawData();     break;
    case 'comp': item = new DescriptorLargeInteger(); break;
    case 'UnFl': item = new DescriptorUnitFloats();  break;
    case 'ObAr': item = new DescriptorObjectArray(); break;
    default:
      break;
    }
    if (item) {
      item->load(data);
    }
    return item;
  }

  // --------------------------------------------------------------------------
  // ローダ
  // --------------------------------------------------------------------------
  bool
  Descriptor::load(IteratorBase *data)
  {
    data->getUnicodeString(name);
    if (readId(data, classId)) idLongMask |= 1;

    int itemCount = data->getInt32();
    for (int i = 0; i < itemCount; i++) {
      std::string key;
      const bool longKey = readId(data, key);
      DescriptorItem *item = readItem(data);
      if (longKey) longKeys.insert(key);
      if (item && item->isValid) {
        if (itemMap.find(key) == itemMap.end()) keyOrder.push_back(key);
        itemMap[key] = item;
      } else {
        // 解釈不能が出てきたら構造上スキップできないのでここで終了する
        if (item) {
          delete item;
        }
        isValid = false;
        break;
      }
    }
    return isValid;
  }

  bool
  DescriptorReference::load(IteratorBase *data)
  {
    int itemCount = data->getInt32();
    for (int i = 0; i < itemCount; i++) {
      ReferenceItem *item = 0;
      type = (ReferenceType)data->getInt32();
      switch (type) {
      case 'prop': item = new ReferenceProperty();   break;
      case 'Clss': item = new ReferenceClass();      break;
      case 'Enmr': item = new ReferenceEnumRef();    break;
      case 'rele': item = new ReferenceOffset();     break;
      case 'Idnt': item = new ReferenceIdentifier(); break;
      case 'indx': item = new ReferenceIndex();      break;
      case 'name': item = new ReferenceName();       break;
      default:
        break;
      }
      if (item) {
        if (item->load(data)) {
          items.push_back(item);
        } else {
          delete item;
        }
      }
    }
    return isValid;
  }

  bool
  DescriptorList::load(IteratorBase *data)
  {
    int itemCount = data->getInt32();
    // 1 要素は最低 4 バイト (型) あるので、残りから見て多すぎる件数は確保しない
    if (itemCount > 0) items.reserve((size_t)std::min(itemCount, data->rest() / 4));
    for (int i = 0; i < itemCount; i++) {
      DescriptorItem *item = readItem(data);
      if (item && item->isValid) {
        items.push_back(item);
      } else {
        // 解釈不能が出てきたら構造上スキップできないのでここで終了する
        delete item;
        isValid = false;
        break;
      }
    }
    return isValid;
  }

  bool
  DescriptorDouble::load(IteratorBase *data)
  {
    pun64 v;
    v.i = data->getInt64();
    val = v.f;
    return true;
  }

  bool
  DescriptorUnitFloat::load(IteratorBase *data)
  {
    unit = (DescriptorUnit)data->getInt32();
    pun64 v;
    v.i = data->getInt64();
    val = v.f;
    return true;
  }

  bool
  DescriptorString::load(IteratorBase *data)
  {
    data->getUnicodeString(val);
    return true;
  }

  bool
  DescriptorEnumerated::load(IteratorBase *data)
  {
    if (readId(data, typeId)) idLongMask |= 1;
    if (readId(data, enumId)) idLongMask |= 2;
    return true;
  }

  bool
  DescriptorInteger::load(IteratorBase *data)
  {
    val = data->getInt32();
    return true;
  }

  bool
  DescriptorBoolean::load(IteratorBase *data)
  {
    val = (data->getCh() != 0);
    return true;
  }

  bool
  DescriptorClass::load(IteratorBase *data)
  {
    data->getUnicodeString(name);
    if (readId(data, classId)) idLongMask |= 1;
    return true;
  }

  bool
  DescriptorLargeInteger::load(IteratorBase *data)
  {
    if (data->rest() < 8) { isValid = false; return false; }
    val = data->getInt64();
    return true;
  }

  bool
  DescriptorUnitFloats::load(IteratorBase *data)
  {
    unit = (DescriptorUnit)data->getInt32();
    int count = data->getInt32();
    if (count < 0 || count > data->rest() / 8) { isValid = false; return false; }
    values.resize((size_t)count);
    for (int i = 0; i < count; i++) {
      pun64 v;
      v.i = (uint64_t)data->getInt64();
      values[(size_t)i] = v.f;
    }
    return true;
  }

  bool
  DescriptorObjectArray::load(IteratorBase *data)
  {
    itemsCount = (uint32_t)data->getInt32();
    return Descriptor::load(data);
  }

  bool
  DescriptorRawData::load(IteratorBase *data)
  {
    int size = data->getInt32();
    if (size < 0 || size > data->rest()) {
      isValid = false;
      return false;
    }
    bytes.resize((size_t)size);
    if (size > 0) {
      data->getData(&bytes[0], size);
    }
    return true;
  }

  bool
  DescriptorAlias::load(IteratorBase *data)
  {
    int size = data->getInt32();
    if (size < 0 || size > data->rest()) {
      isValid = false;
      return false;
    }
    std::vector<char> buf(size+1);
    data->getData(&buf[0], size);
    buf[size] = '\0';
    alias.assign(&buf[0]);
    return true;
  }

  // --------------------------------------------------------------------------
  // リファレンス
  // --------------------------------------------------------------------------

  bool
  ReferenceProperty::load(IteratorBase *data)
  {
    data->getUnicodeString(name);
    if (readId(data, classId)) idLongMask |= 1;
    if (readId(data, keyId)) idLongMask |= 2;
    return true;
  }

  bool
  ReferenceClass::load(IteratorBase *data)
  {
    data->getUnicodeString(name);
    if (readId(data, classId)) idLongMask |= 1;
    return true;
  }

  bool
  ReferenceEnumRef::load(IteratorBase *data)
  {
    data->getUnicodeString(name);
    if (readId(data, classId)) idLongMask |= 1;
    if (readId(data, typeId)) idLongMask |= 2;
    if (readId(data, enumId)) idLongMask |= 4;
    return true;
  }

  bool
  ReferenceOffset::load(IteratorBase *data)
  {
    data->getUnicodeString(name);
    if (readId(data, classId)) idLongMask |= 1;
    offset = data->getInt32();
    return true;
  }

  bool
  ReferenceIdentifier::load(IteratorBase *data)
  {
    identifier = data->getInt32();
    return true;
  }

  bool
  ReferenceIndex::load(IteratorBase *data)
  {
    index = data->getInt32();
    return true;
  }

  bool
  ReferenceName::load(IteratorBase *data)
  {
    data->getUnicodeString(name);
    return true;
  }

} // namespace psd
