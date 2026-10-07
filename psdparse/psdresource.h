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
  void loadLinkedFiles(Data &data);                            // lnk2 / lnk3 / lnkD / lnkE
  void loadPatterns(Data &data);                               // Patt / Pat2 / Pat3
  void loadAlphaChannels(Data &data);                          // 1006 / 1045 / 1077
} // namespace psd
