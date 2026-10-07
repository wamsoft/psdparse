#include "psddata.h"

#include <string>

namespace psd {
  // リソースローダ
  bool loadResourceSlice(Data &data, ImageResourceInfo &res);
  bool loadResourceGridAndGuide(Data &data, ImageResourceInfo &res);
  bool loadResourceColorTableCount(Data &data, ImageResourceInfo &res);
  bool loadResourceTransparencyIndex(Data &data, ImageResourceInfo &res);
  bool loadResourceLayerComps(Data &data, ImageResourceInfo &res);
  bool loadResourcePath(Data &data, ImageResourceInfo &res);   // 1025 / 2000〜2997
  void loadUnicodePathNames(Data &data);                       // 'pths' (保存パスの Unicode 名)
} // namespace psd
