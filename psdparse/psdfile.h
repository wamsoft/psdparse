#ifndef __psdfile_h__
#define __psdfile_h__

#include "psdbase.h"
#include "psddata.h"
#include <cstdint>
#include <istream>
#include <memory>
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
    bool moveLayer(int from, int to);
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
