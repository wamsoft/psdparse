#ifndef __psdfile_h__
#define __psdfile_h__

#include "psdbase.h"
#include "psddata.h"
#include "psdengine.h"   // RunStyleEdit (setLayerRunStyle 用)
#include <cstdint>
#include <functional>
#include <istream>
#include <memory>
#include <string>
#include <vector>

namespace psd {
  // イメージ取得モード
  enum ImageMode {
    IMAGE_MODE_IMAGE,       // マスクをくりこまないイメージデータ
    IMAGE_MODE_MASK,        // マスク情報のみのイメージデータ(グレー)
    IMAGE_MODE_MASKEDIMAGE, // マスクをアルファに繰り込んだイメージデータ
  };

  // PSD ファイルクラス
  //
  //   load(path)            : ファイルを mmap で開く (全読み込みしない)。
  //                           IteratorBase 経由のレイヤ画像取得は OS のページ
  //                           キャッシュ越しに必要なバイトだけ読まれる。
  //   loadFromMemory(p, n)  : 呼び出し元のバイト列を内部 vector にコピー保持。
  //                           ファイルアクセスを介さずロードしたいケース用。
  //   loadFromReader(reader): 汎用エントリ。任意の IteratorBase 実装を受ける。
  //                           reader が指す storage は PSDFile のライフタイム
  //                           中、呼び出し元が維持する責任を負う。
  //                           (kirikiri プラグインは iTJSBinaryStream をラップ
  //                            した自前 StreamReader をここに流し込む)
  class PSDFile : public Data {
  public:
    PSDFile();
    ~PSDFile();

    bool isLoaded;

    // filename は UTF-8。Win32 では内部で UTF-16 に変換してから OS API を叩く。
    bool load(const char *filename);
    bool loadFromMemory(const uint8_t *data, size_t size);
    bool loadFromReader(IteratorBase &reader);
    // 任意の seekable な std::istream を全領域のサイズ付きで受ける。
    // istream の所有権はとらない -- 呼び出し元が PSDFile より長く維持する責任。
    bool loadFromStream(std::istream &stream);
    // 同上の所有権ありバージョン: stream を内部に取り込んで PSDFile が
    // 解放されるまで維持する (Python など、stream を別に管理しづらい状況用)。
    bool loadFromStream(std::unique_ptr<std::istream> stream);

    // PSDFile を空に戻す。mmap を unmap し、vector を解放する。
    void clearData() override;

    // 現在ロード済みの内容を PSD ファイルとして path (UTF-8) に書き出す。
    // 失敗 (open エラー / 未ロード) で false。
    bool save(const char *filename);

    // --- 構造編集 (E1/E2) ---------------------------------------------------
    // いずれも layerList を操作するだけの軽量操作で、実バイトの再構築は
    // save() 時に遅延実行される (channel.imageData / extraData.rawBytes の参照を
    // 保持したまま並べ替え、書き出し時にそれぞれを個別に転送する)。
    //
    // 注意: 合成画像 (composite) は編集後は古いままになる。ファイルは正しく
    // 開けるが、プレビューは編集後の状態を反映せず、Photoshop が開いて再合成
    // するまで旧状態のままとなる。

    // レイヤを 1 枚削除。index は layerList のインデックス。範囲外で false。
    bool deleteLayer(int index);
    // レイヤを from から to へ移動する (to は削除後リストでの挿入位置)。
    // フォルダを渡した場合、区切りと中身は付いてこない (塊で動かすなら
    // moveLayerSibling / moveLayerRange を使う)。
    bool moveLayer(int from, int to);

    // index のレイヤを「同じ階層の隣の兄弟」と入れ替える。
    // up=true で Photoshop の表示上ひとつ上へ (layerList では後ろへ)。
    // フォルダは区切り + 中身をまとめた塊として動き、兄弟がフォルダなら
    // その塊ごと飛び越える。階層をまたぐ移動はしない。
    // 端まで来ていて動かせない場合や範囲外なら false。
    // 移動後の自分のインデックスを newIndexOut へ返す。
    bool moveLayerSibling(int index, bool up, int *newIndexOut = nullptr);

    // layerList 上の [from, from+count) を to の位置へ動かす (低水準)。
    // to は「取り除く前」のインデックスで指定する。
    bool moveLayerRange(int from, int count, int to);
    // レイヤを複製し、複製 (元の直後に挿入) の新インデックスを返す。失敗 -1。
    int  duplicateLayer(int index);
    // 別の PSD からレイヤをこのファイルへコピー挿入する。destIndex<0 で末尾。
    // コピーしたレイヤの画素/追加情報は src のストレージを参照するので、
    // src はこのファイルの save() が終わるまで生存している必要がある。
    // 色モード/ビット深度が一致している前提。新インデックスを返す。失敗 -1。
    int  copyLayerFrom(const PSDFile &src, int srcIndex, int destIndex = -1);

    // --- 画素編集 (E4) ------------------------------------------------------
    // 8bit RGB 文書のみ対応。入力は BGRA インターリーブ (layer_image と同じ並び、
    // width*height*4 バイト)。チャンネルは PackBits(RLE) で符号化して保持し、
    // save() 時にレイヤ毎の再構築経路で書き出される。

    // 既存レイヤ index の画素を差し替える。bbox は left/top を保ち width/height を
    // 更新する。マスク等の extra data は変更しない (サイズ不一致に注意)。失敗 false。
    bool setLayerPixels(int index, const uint8_t *bgra, int width, int height);

    // レイヤのマスク画素 (グレースケール, width*height バイト) を差し替える。
    // マスク矩形も (top,left,w,h) に設定する (幾何編集を兼ねる)。マスクが無ければ
    // 新規作成 (既定色 0)。8bit のみ。失敗で false。カラーチャンネルは保持。
    bool setLayerMaskPixels(int index, const uint8_t *gray,
                            int top, int left, int width, int height);

    // 新規画像レイヤを (left,top) に追加する。destIndex<0 で末尾に挿入。
    // 新しいレイヤのインデックスを返す。失敗で -1。
    int  addLayer(const char *nameUtf8, int left, int top,
                  const uint8_t *bgra, int width, int height,
                  int blendModeKey = 'norm', int opacity = 255, int destIndex = -1);

    // --- extra data 項目編集 (E3) -------------------------------------------
    // レイヤ index を改名する。pascal 名 (UTF-8 バイト) と luni (Unicode) の
    // 両方を更新し、save() 時に extra data をフィールドから再構築する
    // (mask/blending ranges は生バイトを保持するので不変)。失敗で false。
    bool setLayerName(int index, const char *nameUtf8);

    // マスク値の編集 (マスクを持つレイヤのみ、失敗で false)。矩形/画素は変えない。
    bool setMaskDisabled(int index, bool disabled);
    bool setMaskDensity(int index, int density);       // ユーザーマスク濃度 0..255
    bool setMaskFeather(int index, double feather);     // ユーザーマスクぼかし(px)
    bool setMaskDefaultColor(int index, int color);     // 0..255

    // 塗り不透明度 (iOpa) の編集。0..255。失敗で false。
    bool setFillOpacity(int index, int opacity);

    // 追加レイヤ情報ブロック (key) の生バイトを差し替える (無ければ末尾に追加)。
    // 効果 (lfx2) の再直列化バイトの流し込み等に使う。extra data は save 時に
    // フィールドから再構築される (useRawBytes=false)。失敗で false。
    bool setAdditionalInfoBytes(int index, int key, const uint8_t *data, int size);

    // --- テキストレイヤ編集 (E6) --------------------------------------------
    // テキストレイヤの 'TySh' ブロックは
    //   prefix (56B: version + transform + textVer + descVer)
    //   + descriptor ('Txt ' と EngineData を含む)
    //   + suffix (warp + bounds)
    // という構造で、本文の実体は descriptor 内の EngineData (Adobe 独自の
    // ミニ言語) 側にある。以下の API は EngineData を psdengine の
    // editEngineData* で書き換えてから TySh を再直列化し、
    // setAdditionalInfoBytes で差し替える。触っていないレイヤのバイトは
    // そのまま保たれる。
    //
    // 失敗時は errorOut (非 null なら) に理由を入れて false を返す。

    // 本文を newText へ差し替える (段落区切りは \r)。スタイルは先頭ランに
    // 畳まれ、descriptor の 'Txt ' も同じ内容へ更新される。
    bool setLayerText(int index, const u16str &newText,
                      std::string *errorOut = nullptr);
    // UTF-8 版。内部で UTF-16 へ変換する。
    bool setLayerTextUtf8(int index, const char *utf8,
                          std::string *errorOut = nullptr);

    // 既存ラン runIndex のスタイル値を編集する (本文と長さは変えない)。
    // RunStyleEdit の has* が true のフィールドだけ上書きされる。
    bool setLayerRunStyle(int index, int runIndex, const RunStyleEdit &edit,
                          std::string *errorOut = nullptr);

    // 本文とラン構成 / 段落構成をまとめて差し替える (書式付きテキストの編集)。
    // setLayerText はランを 1 つに畳んでしまうので、部分ごとに書式を変えたい
    // 場合はこちらを使う。詳細は psdengine.h の editEngineDataRichText 参照。
    bool setLayerRichText(int index, const u16str &newText,
                          const std::vector<TextRunSpec> &runs,
                          const std::vector<TextParagraphSpec> &paragraphs,
                          std::string *errorOut = nullptr);

    // 段落の行揃えだけ変える (paraIndex < 0 で全段落)。0=左 1=右 2=中央。
    bool setLayerJustification(int index, int paraIndex, int justification,
                               std::string *errorOut = nullptr);

    // このテキストレイヤの EngineData が持つフォント名 (ResourceDict/FontSet)。
    // UI のフォント候補に使う。テキストレイヤでなければ false。
    bool getLayerFonts(int index, std::vector<std::string> &outUtf8Names,
                       std::string *errorOut = nullptr) const;

    // --- テキストの配置と流し込み枠 -----------------------------------------
    // 位置は TySh prefix のアフィン変換 (xx, xy, yx, yy, tx, ty) が持ち、
    // 流し込み枠は descriptor の 'bounds' が「変換のローカル座標」で持つ。
    // 文書上の見た目の位置は tx/ty + bounds になる。

    // 変換行列を取り出す / 差し替える。
    bool getLayerTextTransform(int index, double m[6],
                               std::string *errorOut = nullptr) const;
    bool setLayerTextTransform(int index, const double m[6],
                               std::string *errorOut = nullptr);

    // テキストレイヤを平行移動する。変換の tx/ty と、レイヤ矩形 (とマスク矩形)
    // を同じだけずらすので、PSD 内蔵のラスタも一緒に動く。
    // Photoshop で開き直せば新しい位置で描き直される。
    bool moveTextLayer(int index, double dx, double dy,
                       std::string *errorOut = nullptr);

    // 流し込み枠 (descriptor の 'bounds')。変換のローカル座標。
    // 枠を変えて実際に流し込みが変わるのは段落テキスト (box text) のみで、
    // ポイントテキストでは Photoshop 側が字形から枠を作り直す。
    bool getLayerTextBounds(int index, double &l, double &t, double &r, double &b,
                            std::string *errorOut = nullptr) const;
    bool setLayerTextBounds(int index, double l, double t, double r, double b,
                            std::string *errorOut = nullptr);

    // TySh ブロック全体 (prefix + descriptor) を触る低水準口。
    // 上記の配置 / 枠の API はこれを土台にしている。
    bool editTyShBlock(int index,
                       const std::function<bool(std::vector<uint8_t> &prefix,
                                                Descriptor &desc)> &fn,
                       std::string *errorOut = nullptr);

    // 上記 2 つの共通実体。EngineData のバイト列を editEngine で変換する。
    // 独自の EngineData 変換を差し込みたいとき用。newTxt が非 null なら
    // descriptor の 'Txt ' もその値へ更新する。
    bool editTextLayer(int index,
                       const std::function<bool(const std::string &in,
                                                std::string &out)> &editEngine,
                       const u16str *newTxt = nullptr,
                       std::string *errorOut = nullptr);

    // 合成済み画像 (merged/composite セクション) を差し替える。入力は BGRA
    // インターリーブ (width*height*4)、canvas サイズ一致が必須。header.channels に
    // 応じて RGB(3) か RGBA(4) の raw プレーンで書き出す。8bit RGB のみ。
    // Python 側で合成した結果を PSD のプレビューに反映するのに使う。失敗で false。
    bool setMergedImage(const uint8_t *bgra, int width, int height);

    // --- 新規作成 (E5) ------------------------------------------------------
    // この PSDFile を空の 8bit RGB 文書 (幅×高さ, 白の合成画像) として初期化する。
    // 以後 addLayer(...) でレイヤを足して save() できる。成功で true。
    // (合成画像は白のまま。レイヤ追加後も再合成はされない — Photoshop が開いて
    //  再合成するまで白。)
    bool createBlank(int width, int height, int mode = COLOR_MODE_RGB);

    // 画像データ取得インタフェース (バッファピッチが０の場合は full fill)
    bool getMergedImage(void *buf, const ColorFormat &format, int bufPitchByte);
    bool getLayerImage(const LayerInfo &layer, void *buf, const ColorFormat &format,
                       int bufPitchByte, ImageMode mode);
    bool getLayerImageById(int layerId, void *buf, const ColorFormat &format,
                           int bufPitchByte, ImageMode mode);

  private:
    // OS マップ領域 (path から load した場合)。pimpl で windows.h 等の漏出を防ぐ。
    struct Mapping;
    std::unique_ptr<Mapping> mapping_;
    // ユーザー渡しバイト列の保持 (loadFromMemory 用)。
    std::vector<uint8_t> ownedBuffer_;
    // 所有権版 loadFromStream で取り込んだ istream を維持する。
    std::unique_ptr<std::istream> ownedStream_;

    // clearData/state 初期化は呼ばず、与えられた stream をパースするだけ。
    // 両 loadFromStream overload からの共有実体。
    bool parseFromStream_(std::istream &stream);
  };

} // namespace psd

#endif
