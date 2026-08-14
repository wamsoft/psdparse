#ifndef __psdengine_h__
#define __psdengine_h__

#include "psddata.h"

#include <cstddef>
#include <string>
#include <vector>

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
    // フォント名 (UTF-8)。 ResourceDict/FontSet に無ければ追記し、その
    // インデックスを StyleSheetData/Font に設定する。
    bool   hasFont = false;      std::string font;
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

  // --------------------------------------------------------------------------
  // リッチテキスト差し替え
  //
  // editEngineDataText は本文長の辻褄を合わせるためにランを 1 つへ畳んでしまう
  // ので、「本文を変えつつ部分ごとに書式を変える」ことができない。こちらは
  // 本文とラン構成 / 段落構成をまとめて指定して差し替える。
  // --------------------------------------------------------------------------

  // ラン 1 つぶん (本文中の連続する length 文字に同じスタイルが乗る)
  struct TextRunSpec {
    int          length = 0;   // UTF-16 コードユニット数
    RunStyleEdit style;        // 指定したフィールドだけ雛形から変更する
  };

  // 段落 1 つぶん
  struct TextParagraphSpec {
    int  length = 0;                // UTF-16 コードユニット数
    bool hasJustification = false;
    int  justification = 0;         // 0=左 1=右 2=中央
  };

  // 本文とラン構成 / 段落構成をまとめて差し替える。
  //
  // - runs が空なら単一ランへ畳む (editEngineDataText と同じ挙動)
  // - runs の length 合計が本文長と食い違う場合は末尾ランで吸収する
  //   (長すぎれば切り詰め、短ければ末尾を伸ばす)。長さ 0 のランは捨てる
  // - 各ランのスタイルは「元の先頭ランを雛形にして、指定されたフィールドだけ
  //   上書き」する。未指定の書式は雛形のまま (= 元の見た目) を保つ
  // - paragraphs も同様。空なら単一段落へ畳む
  // - 末尾に改行 (\r) が無ければ補う (Photoshop の慣習)
  bool editEngineDataRichText(const char *data, size_t len, const u16str &newText,
                              const std::vector<TextRunSpec> &runs,
                              const std::vector<TextParagraphSpec> &paragraphs,
                              std::string &out);

  // 段落 paraIndex の行揃えだけ変える (本文もラン構成も変えない)。
  // paraIndex < 0 で全段落。範囲外なら false。
  bool editEngineDataJustification(const char *data, size_t len, int paraIndex,
                                   int justification, std::string &out);

  // ResourceDict/FontSet に載っているフォント名を列挙する (UI の候補用)。
  bool listEngineDataFonts(const char *data, size_t len,
                           std::vector<std::string> &outUtf8Names);

} // namespace psd

#endif // __psdengine_h__
