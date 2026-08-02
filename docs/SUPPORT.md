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

最終更新: 2026-08-02 (v0.6.0)。詳細な今後の計画は [ROADMAP.md](ROADMAP.md)、
Python API の使い方は [PYTHON_API.md](PYTHON_API.md) を参照。

---

## ファイル全体

| 機能 | 状況 | 備考 |
|---|:---:|---|
| ヘッダ (幅/高さ/チャンネル/深度/モード/版) | ✅ | `PSDFile.header` |
| 解像度 (dpi, image resource 1005) | ✅ | `header.hres` / `header.vres` |
| PSB (large document, version 2) | ❌ | `version` は読めるが、PSB 特有の 8 byte 長フィールドの分岐が無く未対応 |
| ラウンドトリップ保存 (byte-identical) | ✅ | `load(a) -> save(b)` が完全一致 (未編集時) |
| 編集保存: パラメータ変更 | ✅ | `layer.opacity`/`visible`/`set_blend_mode()` (v0.7.0) |
| 編集保存: レイヤ改名 | ✅ | `layer.name_unicode = ...` / `set_layer_name()` — mask/blend は生バイト保持 (v0.7.0) |
| 編集保存: マスク値編集 (disabled/density/feather/default色) | ✅ | `set_layer_mask(i, ...)` — 矩形/画素は不変 (v0.7.0) |
| 編集保存: 塗り不透明度 (fill opacity) | ✅ | `layer.fill_opacity = ...` (iOpa, v0.7.0) |
| 編集保存: 効果 (lfx2) の値編集 | ❌ | Descriptor シリアライザ未実装 (ROADMAP E3 残 / E6 と共通) |
| 編集保存: レイヤ削除/並べ替え/複製 | ✅ | `delete_layer`/`move_layer`/`duplicate_layer` (v0.7.0) |
| 編集保存: 別 PSD からレイヤコピー | ✅ | `copy_layer_from(src, i)` — src を save まで生存させる (v0.7.0) |
| 編集保存: 画素差し替え | ✅ | `set_layer_pixels(i, bgra, w, h)` — 8bit RGB のみ (v0.7.0) |
| 編集保存: 画像レイヤ新規追加 | ✅ | `add_layer(name, l, t, bgra, w, h)` — 8bit RGB のみ、名前は luni で Unicode 対応 (v0.7.0) |
| 編集保存: 完全新規 PSD 作成 | ✅ | `create_blank(w, h)` → `add_layer` → `save` — 8bit RGB のみ (v0.7.0) |
| 編集保存: テキストレイヤ編集 | ❌ | Descriptor/EngineData シリアライザ未実装 (ROADMAP Phase E6) |
| 編集後の合成画像 (composite) 再生成 | ❌ | 編集後は旧合成のまま (開いた Photoshop が再合成) |

## 圧縮 / ビット深度

| 項目 | 状況 | 備考 |
|---|:---:|---|
| Raw / RLE(PackBits) / ZIP(±prediction) | ✅ | 展開対応。PackBits は符号化 (save 時のレイヤ画素書き出し) も対応 |
| ビット深度 1 / 8 / 16 / 32 | ✅ | それ以外は非対応 |

## カラーモード (ピクセル展開: `merged_image` / `layer_image`)

| モード | 状況 | 備考 |
|---|:---:|---|
| Bitmap (1bit) | ✅ | |
| Grayscale (8/16/32) | ✅ | |
| RGB (8/16/32) | ✅ | |
| Indexed (8bit) | ✅ | パレットは `PSDFile.color_table` |
| CMYK (8/16/32) | ✅ | RGB へ変換して出力 |
| Multichannel | ❌ | RGB への正準的変換が無く未対応 |
| Duotone (8/16/32) | ✅ | grayscale として展開 (Adobe 仕様: duotone データは gray) |
| Lab (8/16) | ✅ | 標準 D65 CIELAB→sRGB 近似で変換 (Photoshop は D50 のため彩度の高い色は差あり)。32bit Lab は非存在 |

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
| シートカラー (レイヤパネルの色ラベル, `lclr`) | ✅ | `layer.sheet_color` (`{index,name}`, v0.6.0) |

## レイヤマスク / ブレンド範囲

| 機能 | 状況 | API |
|---|:---:|---|
| マスク矩形/フラグ/既定色 | ✅ | `layer.mask` (v0.3.0) |
| real/user mask (size>=36) | ✅ | `layer.mask["real"]` (v0.6.0 でオフセット 1 byte ずれを修正) |
| density / feather | ✅ | `layer.mask` の `user_density`/`user_feather`/`vector_density`/`vector_feather` (v0.6.0) |
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
| スマートオブジェクト変換 (`SoLd`) | 🟡 | `descriptor("SoLd")` で取得可 (既定 skip=12 を設定済。実サンプル未検証) |
| スマートオブジェクト埋め込みデータ抽出 (`lnkD`) | ❌ | 未対応 |
| ベクタストローク/シェイプ (`vstk`/`vscg`/`vogk`) | 🟡 | `descriptor()` 経由で取得可 (既定 skip 設定済、実サンプル未検証) |
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
| ICC プロファイル (1039) | ✅ | `PSDFile.icc_profile` (生バイト, v0.5.0) |
| EXIF (1058) | ✅ | `PSDFile.exif` (生バイト, v0.5.0) |
| XMP メタデータ (1060) | ✅ | `PSDFile.xmp` (UTF-8 str, v0.5.0) |
| サムネイル (1036/1033) | ✅ | `PSDFile.thumbnail` (JPEG bytes + 寸法, v0.5.0) |
| 任意リソースの生バイト | ✅ | `PSDFile.image_resource(id)` / `image_resource_ids` (v0.5.0) |
| 上記の内容デコード (EXIF タグ解析等) | ❌ | 生バイトを返すのみ。解析は利用側 (Pillow 等) で |
| バージョン情報 / アルファチャンネル名 / その他 | 📦→✅ | `image_resource(id)` で生バイト取得可 |
| Global layer mask info | ✅ | `PSDFile.global_layer_mask` (overlay 色/opacity/kind, v0.6.0) |

---

## まとめ (一言で)

- **得意**: レイヤ列挙 + 属性 + 階層 + 色ラベル、RGB/CMYK/Gray/Indexed/Lab/Duotone
  のピクセル取得、テキストのラン単位スタイル、マスクの density/feather、レイヤー
  効果/塗りの descriptor、文書メタデータ (ガイド/スライス/カンプ/global layer
  mask) と image resource の生バイト (ICC/EXIF/XMP/サムネイル)、そして
  **byte-identical なラウンドトリップ**。
- **未対応/限定的**: Multichannel ピクセル、調整レイヤの数値パラメータ、ベクタ
  パス、スマートオブジェクト実体 (`lnkD`)、効果込みの再合成、そして編集して保存。
- Lab は標準 D65 CIELAB→sRGB 近似 (Photoshop の D50 とは彩度の高い色でわずかに差)。
- image resource の**中身の解釈** (EXIF タグ, サムネイル描画等) は行わず生バイトを
  返すのみ。
