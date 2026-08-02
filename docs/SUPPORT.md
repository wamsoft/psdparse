# psdparse PSD 対応状況 (Support Matrix)

psdparse が PSD のどの機能を、どの水準で扱えるかの一覧です。psdparse は
**参照 (読み取り) + ラウンドトリップ保存** に重点を置いた実装で、編集保存や
高度な再合成は対象外です。

対応状況の凡例:

| 記号 | 意味 |
|:---:|---|
| ✅ | 対応済み — 構造化して取得できる |
| 🟡 | 部分対応 — 一部の値のみ / 生 descriptor 経由 |
| 📦 | 生バイトのみ保持 (ラウンドトリップ用。Python へは未公開) |
| ❌ | 未対応 — パースしていない |

最終更新: 2026-08-02 (v0.4.0)。詳細な今後の計画は [ROADMAP.md](ROADMAP.md)、
Python API の使い方は [PYTHON_API.md](PYTHON_API.md) を参照。

---

## ファイル全体

| 機能 | 状況 | 備考 |
|---|:---:|---|
| ヘッダ (幅/高さ/チャンネル/深度/モード/版) | ✅ | `PSDFile.header` |
| 解像度 (dpi, image resource 1005) | ✅ | `header.hres` / `header.vres` |
| PSB (large document, version 2) | ❌ | `version` は読めるが、PSB 特有の 8 byte 長フィールドの分岐が無く未対応 |
| ラウンドトリップ保存 (byte-identical) | ✅ | `load(a) -> save(b)` が完全一致 |
| 編集して保存 (レイヤ追加/削除/差し替え) | ❌ | ROADMAP Phase 4b–4e。RLE エンコーダ未実装 |

## 圧縮 / ビット深度

| 項目 | 状況 | 備考 |
|---|:---:|---|
| Raw / RLE(PackBits) / ZIP(±prediction) | ✅ | 展開対応 |
| ビット深度 1 / 8 / 16 / 32 | ✅ | それ以外は非対応 |

## カラーモード (ピクセル展開: `merged_image` / `layer_image`)

| モード | 状況 | 備考 |
|---|:---:|---|
| Bitmap (1bit) | ✅ | |
| Grayscale (8/16/32) | ✅ | |
| RGB (8/16/32) | ✅ | |
| Indexed (8bit) | ✅ | パレットは `PSDFile.color_table` |
| CMYK (8/16/32) | ✅ | RGB へ変換して出力 |
| Multichannel | ❌ | `getLayerImage` 未対応 |
| Duotone | ❌ | 同上 |
| Lab | ❌ | 同上 |

出力は常に **BGRA インターリーブ** (4 byte/px)。ICC を用いた色変換は行いません。

## 画像取得

| 機能 | 状況 | 備考 |
|---|:---:|---|
| 合成画像 (merged/composite) | ✅ | `merged_image()` — PSD 保存済みの合成を返す |
| レイヤ画像 (mask 込み/無し/mask のみ) | ✅ | `layer_image(i, "masked"/"image"/"mask")` |
| 効果・調整を適用した再合成 (`composite()` 相当) | ❌ | 保存済み合成の取得のみ。再描画はしない |

---

## レイヤ属性

| 属性 | 状況 | API |
|---|:---:|---|
| 位置/サイズ (bbox, width, height) | ✅ | `top/left/bottom/right`, `width`, `height` |
| ブレンドモード | ✅ | `blend_mode` (enum), `blend_mode_key` |
| 不透明度 / 塗り不透明度 | ✅ | `opacity`, `fill_opacity` |
| クリッピング | ✅ | `clipping` |
| 可視/ロック等フラグ | ✅ | `visible`, `transparency_protected`, `obsolete`, `pixel_data_irrelevant` |
| レイヤ名 (raw / Unicode) | ✅ | `name`, `name_unicode` |
| レイヤ ID | ✅ | `layer_id` |
| レイヤ種別 | ✅ | `layer_type` (NORMAL/HIDDEN/FOLDER/ADJUST/FILL/TEXT) |
| **フォルダ階層 (親子)** | ✅ | `parent_index` (v0.3.0) |
| チャンネル構成 (id/length) | ✅ | `channels[]` |
| シートカラー (レイヤパネルの色ラベル, `lclr`) | ❌ | 未デコード |

## レイヤマスク / ブレンド範囲

| 機能 | 状況 | API |
|---|:---:|---|
| マスク矩形/フラグ/既定色 | ✅ | `layer.mask` (v0.3.0) |
| real/user mask (>20byte) | ✅ | `layer.mask["real"]` |
| density / feather | 🟡 | `has_parameters` フラグのみ公開。値 (MaskParameters) は未デコード |
| ベクタマスク / パス (`vmsk`/`vsms`) | ❌ | 未対応 |
| ブレンディングレンジ ("Blend If") | ✅ | `layer.blending_ranges` (raw 32bit packed) |

## テキストレイヤ

| 機能 | 状況 | 備考 |
|---|:---:|---|
| 本文 / 縦横 / 変換行列 | ✅ | `layer.text` (`text`/`orientation`/`transform`) |
| ラン単位スタイル (font/size/color/tracking/kerning) | ✅ | `text["runs"]` |
| 段落別の行揃え | ✅ | `text["paragraphs"]` (v0.2.2) |
| ワープ (warp) | ❌ | TySh warp descriptor 未処理 |
| 非 RGB の FillColor | ❌ | `/Type 1` (RGB) のみ |
| leading / 疑似ボールド / 下線 等 | 🟡 | EngineData にキーはあるが既定値サンプルのみで未検証 |

## Descriptor ブロック (v0.4.0)

Photoshop の汎用ディスクリプタで格納されるブロックを dict 化して取得。

| 機能 | 状況 | API |
|---|:---:|---|
| レイヤー効果 (`lfx2`, object-based) | ✅ | `layer.effects` (nested descriptor dict) |
| 旧レイヤー効果 (`lrFX`, binary) | ❌ | descriptor でないため未対応 |
| 塗りつぶしレイヤ (`SoCo`/`GdFl`/`PtFl`) | ✅ | `layer.fill` (`{type, data}`) |
| 任意キーの descriptor 取得 | ✅ | `layer.descriptor(key, skip)` / `layer.info_keys` |

`effects` / `fill` は **生 descriptor を辞書化したもの** で、効果ごとの型付き
アクセサ (drop shadow の angle/distance を名前で、等) はまだありません。値の
マッピング規則は [PYTHON_API.md](PYTHON_API.md#descriptor-blocks) を参照。

## 調整レイヤ / 塗りつぶし / スマートオブジェクト / シェイプ

| 機能 | 状況 | 備考 |
|---|:---:|---|
| 調整レイヤの種別判定 | ✅ | `layer_type == ADJUST` |
| 調整レイヤのパラメータ (levels/curves 等, binary) | ❌ | 未デコード。descriptor 形式のもの (`CgEd` 等) は `descriptor()` で取得可 |
| スマートオブジェクト変換 (`SoLd`/`PlLd`) | 🟡 | `descriptor()` で試行可 (未検証、既定 skip 要調整) |
| スマートオブジェクト埋め込みデータ抽出 (`lnkD`) | ❌ | 未対応 |
| ベクタストローク/シェイプ (`vstk`/`vscg`) | 🟡 | `descriptor()` 経由で取得可 (未検証) |
| ライブシェイプ情報 (origination) | ❌ | 未対応 |

---

## Image Resources / 文書メタデータ

| リソース | 状況 | API |
|---|:---:|---|
| 解像度 (1005) | ✅ | `header.hres/vres` |
| グリッド & ガイド (1032) | ✅ | `PSDFile.guides` (v0.3.0) |
| スライス (1050 v6) | ✅ | `PSDFile.slices` (v0.3.0) |
| スライス (1050 v7/v8, descriptor) | ❌ | 未格納 |
| レイヤーカンプ (1065) | ✅ | `PSDFile.layer_comps` (v0.3.0) |
| インデックスカラーパレット (色/count/透明index) | ✅ | `PSDFile.color_table` (v0.3.0) |
| ICC プロファイル (1039) | 📦 | 生バイト保持のみ・未公開 |
| EXIF (1058/1059) | 📦 | 同上 |
| XMP メタデータ (1060) | 📦 | 同上 |
| サムネイル (1033/1036) | 📦 | 同上 |
| バージョン情報 / アルファチャンネル名 / その他 | 📦 | 同上 |
| Global layer mask info | 📦 | 構造体フィールドは未充填、生バイトのみ |

---

## まとめ (一言で)

- **得意**: レイヤ列挙 + 属性 + 階層、RGB/CMYK/Gray/Indexed のピクセル取得、
  テキストのラン単位スタイル、レイヤー効果/塗りの descriptor、主要な文書
  メタデータ (ガイド/スライス/カンプ)、そして **byte-identical なラウンドトリップ**。
- **未対応/限定的**: Lab/Duotone/Multichannel ピクセル、調整レイヤの数値
  パラメータ、ベクタパス、スマートオブジェクト実体、効果込みの再合成、
  そして編集して保存。
- 生バイトは全ブロックが保持されているので、📦 の項目は「公開するだけ」で
  対応できるものが多い (ROADMAP 参照)。
