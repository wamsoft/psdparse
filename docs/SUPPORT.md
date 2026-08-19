# psdparse PSD 対応状況 (Support Matrix)

psdparse が PSD のどの機能を、どの水準で扱えるかの一覧です。psdparse は
**参照 (読み取り) + ラウンドトリップ保存** を土台に、**構造編集 (レイヤの
追加/削除/並べ替え/複製、画素・マスク・パラメータ・効果・テキストの編集、
ゼロからの新規作成、レイヤグループの作成)** まで対応します (v0.12.0)。未編集のファイルは byte-identical
に保存され、編集した部分だけがフィールドから再構築されます。効果込みの再合成
(composite の再描画) は対象外です。

対応状況の凡例:

| 記号 | 意味 |
|:---:|---|
| ✅ | 対応済み — 構造化して取得 / 編集できる |
| 🟡 | 部分対応 — 一部の値のみ / 生 descriptor 経由 |
| 📦 | 生バイトのみ保持 (ラウンドトリップ用。Python へは未公開) |
| ❌ | 未対応 — パースしていない |

API 欄は Python 名です。C++ 側の対応するメソッドは `psdparse/psdfile.h` /
`psdparse/psdengine.h` を参照 (Python API は C++ の公開編集面を一通り覆っています)。

最終更新: 2026-08-18 (v0.12.0)。詳細な今後の計画は [ROADMAP.md](ROADMAP.md)、
Python API の使い方は [PYTHON_API.md](PYTHON_API.md) を参照。

---

## ファイル全体

| 機能 | 状況 | 備考 |
|---|:---:|---|
| ヘッダ (幅/高さ/チャンネル/深度/モード/版) | ✅ | `PSDFile.header` |
| 解像度 (dpi, image resource 1005) | ✅ | `header.hres` / `header.vres` |
| PSB (large document, version 2) | ❌ | `version` は読めるが、PSB 特有の 8 byte 長フィールドの分岐が無く未対応 |
| ラウンドトリップ保存 (byte-identical) | ✅ | `load(a) -> save(b)` が完全一致 (未編集時) |

## 編集して保存 (edit & save, v0.7.0–v0.10.0)

版の表記は Python API が使えるようになった版です (C++ 側は 0.9.0 で先行、
Python バインディングが 0.10.0)。

編集は「参照のみ・保存時に再構築」モデル。未編集のレイヤは生バイトをそのまま
書き出すので byte-identical を維持し、編集した部分だけがフィールドから再構築
される。画素系は **8bit RGB 文書のみ**。

| 機能 | 状況 | API |
|---|:---:|---|
| パラメータ変更 (不透明度/可視/クリップ/ブレンド) | ✅ | `layer.opacity`/`visible`/`clipping`/`set_blend_mode()` |
| 塗り不透明度 (fill opacity) | ✅ | `layer.fill_opacity = ...` (iOpa) |
| レイヤ改名 | ✅ | `layer.name_unicode = ...` / `set_layer_name()` (Pascal + luni) |
| レイヤ削除 / 並べ替え / 複製 | ✅ | `delete_layer` / `move_layer` / `duplicate_layer` (`move_layer` は 1 枚単位。フォルダは塊で付いてこない) |
| フォルダを塊で移動 (区切り + 中身) | ✅ | `move_layer_sibling(i, up)` / `group_span(i)` / `move_layer_range(from, count, to)` (v0.10.0) |
| 構造編集後の親子関係 (`parent_index`) 貼り直し | ✅ | 削除/移動/複製/コピー/追加の後で自動 (`relinkGroups`, v0.10.0) |
| 別 PSD からレイヤコピー | ✅ | `copy_layer_from(src, i)` — src を save まで生存させる |
| 画素差し替え | ✅ | `set_layer_pixels(i, bgra, w, h)` (RLE 符号化) |
| 画像レイヤ新規追加 | ✅ | `add_layer(name, l, t, bgra, w, h)` (名前は luni で Unicode) |
| 完全新規 PSD 作成 | ✅ | `create_blank(w, h)` → `add_layer` → `save` |
| マスク値編集 (disabled/density/feather/default色) | ✅ | `set_layer_mask(i, ...)` — 矩形/画素は不変 |
| マスク画素 / 矩形 (幾何) の設定 | ✅ | `set_layer_mask_pixels(i, gray, top, left, w, h)` |
| 効果 (lfx2) の値編集 | ✅ | `set_effects(i, changes)` — descriptor シリアライザ byte-exact |
| 任意 descriptor (塗り SoCo/GdFl/PtFl 等) の値編集 | ✅ | `set_layer_descriptor(i, key, changes)` |
| テキスト本文の編集 | ✅ | `set_text(i, str)` — EngineData シリアライザ byte-exact |
| テキストのラン単位スタイル編集 | ✅ | `set_run_style(i, run, font=/size_px=/color=/tracking=/kerning=/bold=/italic=/underline=)` (v0.7.0) |
| テキストのフォント変更 (FontSet へ追記) | ✅ | `set_run_style(i, run, font="Arial")` — FontSet に無ければ追記 (v0.10.0) |
| 本文 + ラン / 段落構成のまとめ差し替え (リッチテキスト) | ✅ | `set_rich_text(i, text, runs, paragraphs)` (v0.10.0)。未指定の書式は元の先頭ランを踏襲 |
| 段落の行揃え編集 | ✅ | `set_justification(i, j, para_index=-1)` (v0.10.0)。既定 -1 で全段落 |
| フォント候補の列挙 (FontSet) | ✅ | `text_fonts(i)` (v0.10.0) |
| テキストの位置 (TySh 変換行列) 編集 | ✅ | `move_text_layer(i, dx, dy)` / `text_transform(i)` / `set_text_transform(i, m)` (v0.10.0)。move はレイヤ矩形とマスク矩形も同時にずらす |
| テキストの流し込み枠 (descriptor `bounds`) 編集 | ✅ | `text_bounds(i)` / `set_text_bounds(i, l,t,r,b)` (v0.10.0)。実際に流し込みが変わるのは段落 (box) テキストのみ |
| **Txt2 (文書ぜんたいの Text Engine Data) の追随 / 削除** | ✅ | `set_text_engine_policy(0=SYNC/1=REMOVE/2=KEEP)` / `drop_text_engine_data()` (v0.12.0)。**Photoshop は Txt2 を TySh より優先して読む**ので、これが無いと編集が Photoshop に届かない |
| マスク幾何の単独編集 (画素なし) | 🟡 | `set_layer_mask_pixels` で画素とセットのみ |
| 合成済み画像 (composite) の入れ替え | ✅ | `set_merged_image(bgra)` — Python で合成した結果を書き戻せる (v0.7.x) |
| 合成済み画像を単色プレビューにする | ✅ | `set_merged_image_solid(r,g,b)` (v0.12.0) — RLE 圧縮なので巨大キャンバスでも小さい。Photoshop が「PSD 互換を優先」を切ったときと同じ形 |
| 効果込みの合成 (composite) の自動再生成 | ❌ | psdparse 自身は再描画しない。合成は Python (Pillow 等, `examples/`) で |

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
| 疑似ボールド / 疑似イタリック / 下線 | ✅ | `text["runs"]` の `bold`/`italic`/`underline` (FauxBold/FauxItalic/Underline, v0.9.0) |
| 段落別の行揃え | ✅ | `text["paragraphs"]` (v0.2.2) |
| 配置 (変換行列) / 流し込み枠 | ✅ | `text["transform"]` / `text_transform(i)` / `text_bounds(i)` |
| **本文の編集 (save)** | ✅ | `set_text(i, str)` — EngineData を byte-exact に再直列化 (v0.7.0)。スタイルは先頭ランに畳まれる |
| **ラン単位スタイルの編集** | ✅ | `set_run_style(i, run, size_px=/color=/tracking=/kerning=/bold=/italic=/underline=)` (v0.7.0)、`font=` (v0.10.0) |
| リッチテキスト差し替え / 行揃え / フォント変更 / 配置 / 枠 | ✅ | 上の「編集して保存」表を参照 (v0.10.0) |
| Txt2 の本文 / ラン長の追随 | ✅ | 既定で `set_text` / `set_rich_text` が Txt2 も書き換える (v0.12.0)。`text_engine_texts()` で中身を確認できる |
| Txt2 の書式 (スタイルシート) の追随 | ❌ | 数値エイリアスのシートを作り直す必要があるため。書式が変わる編集では Txt2 を削除して TySh へ倒す |
| テキストレイヤのラスタ再生成 | ❌ | psdparse も **Photoshop も開いただけでは描き直さない**。Photoshop 側で各テキストレイヤを小突く必要がある (psdtext の `tools/update-text-layers.jsx` 参照) |
| ワープ (warp) | ❌ | TySh warp descriptor 未処理 (編集時はバイト列のまま保存される) |
| 非 RGB の FillColor | ❌ | `/Type 1` (RGB) のみ |
| leading / 段落インデント / 段落前後アキ | ❌ | EngineData にキーはあるが未抽出・未編集 |

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
| レイヤーカンプ (1065, 文書レベル) | ✅ | `PSDFile.layer_comps` (id/name/comment/record_*, v0.3.0) |
| レイヤーカンプの各レイヤ状態 (可視) | ✅ | `layer.comp_states` = `{comp_id: {enabled, offset_x, offset_y}}` (v0.7.x)。位置/効果の上書きは未適用 |
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

- **得意 (参照)**: レイヤ列挙 + 属性 + 階層 + 色ラベル、RGB/CMYK/Gray/Indexed/Lab/
  Duotone のピクセル取得、テキストのラン単位スタイル、マスクの density/feather、
  レイヤー効果/塗りの descriptor、文書メタデータ (ガイド/スライス/カンプ/global
  layer mask) と image resource の生バイト (ICC/EXIF/XMP/サムネイル)、そして
  **byte-identical なラウンドトリップ**。
- **得意 (編集, v0.10.0)**: レイヤの追加/削除/並べ替え (フォルダは塊のまま移動)
  /複製/他ファイルコピー、画素・マスク画素・パラメータ・改名・fill opacity の
  編集、効果 (lfx2) の byte-exact な編集、テキスト (本文 / ラン単位スタイル /
  リッチテキスト / 行揃え / フォント / 配置 / 流し込み枠) の編集、ゼロからの
  新規作成。未編集部分は byte-identical を維持。編集 API は C++ / Python の
  どちらからも同じことができます。
- **未対応/限定的**: Multichannel ピクセル、調整レイヤの数値パラメータ、ベクタ
  パス、スマートオブジェクト実体 (`lnkD`)、効果込みの再合成 (composite 再描画)、
  テキストの warp / leading / 段落インデント。画素編集は 8bit RGB のみ。
- Lab は標準 D65 CIELAB→sRGB 近似 (Photoshop の D50 とは彩度の高い色でわずかに差)。
- image resource の**中身の解釈** (EXIF タグ, サムネイル描画等) は行わず生バイトを
  返すのみ。
