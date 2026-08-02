#ifndef __psdengine_h__
#define __psdengine_h__

#include "psddata.h"

#include <cstddef>

namespace psd {

  // Adobe "EngineData" (テキストエンジンのシリアライズ表現) を解析し、
  // TextLayerData の text / runs / justification / orientation を埋める。
  //
  // EngineData は Descriptor とは別物の独自ミニ言語で、次の要素からなる:
  //   <<  >>        辞書 ( /key value の並び )
  //   [  ]          配列
  //   ( ... )       文字列 (UTF-16BE, BOM 付き。( ) \ は \ でエスケープ)
  //   /name         キー
  //   123 / -1.5    数値
  //   true / false  真偽
  //
  // 抽出するのは:
  //   EngineDict/Editor/Text                     … 本文
  //   ResourceDict/FontSet[]/Name                … フォント名テーブル
  //   EngineDict/StyleRun/RunArray[]             … ラン単位スタイル
  //     StyleSheet/StyleSheetData/{Font,FontSize,FillColor}
  //   EngineDict/StyleRun/RunLengthArray         … 各ランの文字数
  //   EngineDict/ParagraphRun/RunArray[0]/…/Justification … 行揃え
  //
  // 本文 (Editor/Text) が取得できたら true を返す。
  bool parseEngineData(const char *data, size_t len, TextLayerData &out);

  // EngineData をパースして psd-tools 準拠の形式で再直列化する。 out に結果。
  // (未編集なら元とバイト一致するのが目標 — 直列化器の検証用)。
  bool reserializeEngineData(const char *data, size_t len, std::string &out);

  // EngineData の本文を newText に差し替えて再直列化する。 スタイルは先頭ランに
  // 畳まれる。 成功で true。
  bool editEngineDataText(const char *data, size_t len, const u16str &newText,
                          std::string &out);

  // ラン単位スタイル編集の内容。 has* が true のフィールドだけ上書きする。
  struct RunStyleEdit {
    bool   hasSize = false;      double size = 0;         // FontSize (px)
    bool   hasColor = false;     float  color[4] = {0,0,0,1}; // RGBA 0..1
    bool   hasTracking = false;  int    tracking = 0;
    bool   hasKerning = false;   int    kerning = 0;
    bool   hasBold = false;      bool   bold = false;     // FauxBold
    bool   hasItalic = false;    bool   italic = false;   // FauxItalic
    bool   hasUnderline = false; bool   underline = false;// Underline
  };

  // EngineDict/StyleRun/RunArray[runIndex] のスタイル値を編集して再直列化する。
  // 継承でキーが無い場合は追加する。 runIndex が範囲外なら false。
  bool editEngineDataRunStyle(const char *data, size_t len, int runIndex,
                              const RunStyleEdit &edit, std::string &out);

} // namespace psd

#endif // __psdengine_h__
