# psdfx — PSD の描画処理 (C API)

`psdparse/psdfx.h` は、PSD の合成で使う画像処理を **素の C 関数** にまとめた
ものです。`PSDFile::compositeImage` / `renderLayer` (Python では `composite()` /
`render_layer()`) はこれを使って描いています。PSD の解析とは切り離してあり、
psdfx の `.cpp` (`psdfx.cpp` / `psdfx_path.cpp` / `psdfx_paint.cpp` /
`psdfx_effects.cpp`) は標準ライブラリ以外に依存しません。組み込み先で
自前の画像に同じ効果を掛けたいときは、この API を直接呼べます。

未リリース。関数や構造体は今後変わることがあります。

## 約束ごと

- 面は `psdfx_surface` (BGRA 8bit、**ストレートアルファ**、所有しない)。
  `stride` は 1 行のバイト数。
- 描画モードは PSD に入っている 4 文字キーをそのまま整数にしたもの
  (`PSDFX_KEY('m','u','l',' ')`)。`psdfx_blend_supported()` で対応を確かめられる。
- 長さはピクセル、角度は度 (0 = 右、90 = 上、反時計回り)、不透明度は 0..1。
- 効果の値は descriptor の値を **そのまま** 入れればよいように作ってある
  (効果全体の拡大率 `Scl ` は保存値に既に反映済みなので掛けない、など)。
  descriptor から構造体への詰め替えは `psdcomposite.cpp` の `layerEffects()` が手本。

## 関数

| 関数 | 内容 |
|---|---|
| `psdfx_composite` | 面を描画モード・不透明度・マスク付きで重ねる (source-over) |
| `psdfx_composite_layer` | レイヤとして重ねる。不透明度と塗りの不透明度を分けて受け取る (リニアバーンなど 8 つのモードでは塗りが特別に効く) |
| `psdfx_composite_atop` | dst のアルファを変えずに重ねる (source-atop) |
| `psdfx_composite_layer_atop` | `psdfx_composite_layer` の source-atop 版 (クリッピング用。下地が半透明でもアルファを変えない) |
| `psdfx_lerp` | 2 枚の面の線形補間 (通過グループの不透明度 / マスク) |
| `psdfx_blur_plane` | 1 チャンネルのガウスぼかし (箱ぼかし 3 回) |
| `psdfx_gradient_color` / `psdfx_draw_gradient` | グラデーション (線形 / 円形 / 角度 / 反射 / 菱形、中間点、滑らかさ) |
| `psdfx_draw_pattern` | パターンの繰り返し |
| `psdfx_fill_path` | ベジェのサブパス群をアンチエイリアス付きで塗る (パスの合成方法、初期塗りつぶし) |
| `psdfx_stroke_path` | サブパス群の線 (線幅、内側 / 中央 / 外側、端、角、マイターの上限、破線) |
| `psdfx_flatten_subpath` | サブパス 1 本を折れ線にする (曲線からのずれの上限を指定) |
| `psdfx_apply_lut` と `psdfx_*_lut` | 表で掛ける調整 (レベル補正 / トーンカーブ / 明るさ・コントラスト / 露光量 / ポスタリゼーション) |
| `psdfx_hue_saturation` ほか | 色相・彩度 / 自然な彩度 / カラーバランス / 特定色域の選択 / チャンネルミキサー / レンズフィルター / 白黒 / グラデーションマップ / 2 階調化 |
| `psdfx_apply_adjusted` | 調整済みの画像を元の画像へブレンドモード・不透明度・マスクで重ねる (アルファは元のまま) |
| `psdfx_effects_margin` | 効果がレイヤの外へはみ出す量 |
| `psdfx_composite_with_effects` | レイヤを効果込みで重ねる |

効果は `psdfx_layer_effects` にまとめて渡します: ドロップシャドウ / シャドウ
(内側) / 光彩 (外側・内側) / 境界線 / カラー・グラデーション・パターン
オーバーレイ / サテン / ベベルとエンボス。同じ種類の 2 つ目以降は `more_*` に
並べます。ベベルとサテンは近似です。

## 例: 画像に境界線とドロップシャドウを付ける

```c
#include "psdfx.h"

psdfx_layer_effects fx = {0};
fx.stroke.enabled = 1;
fx.stroke.blend = PSDFX_KEY('n','o','r','m');
fx.stroke.opacity = 1.0f;
fx.stroke.size = 3;
fx.stroke.position = PSDFX_STROKE_OUTSIDE;
fx.stroke.fill.kind = PSDFX_FILL_SOLID;
fx.stroke.fill.color[0] = 255;            /* R */

fx.drop_shadow.enabled = 1;
fx.drop_shadow.blend = PSDFX_KEY('m','u','l',' ');
fx.drop_shadow.opacity = 0.75f;
fx.drop_shadow.angle = 120;
fx.drop_shadow.distance = 5;
fx.drop_shadow.size = 5;
fx.drop_shadow.knocks_out = 1;

/* layer を dst の (left, top) へ。doc_box はグラデーション / パターンの基準 */
const double doc_box[4] = { 0, 0, dst.width, dst.height };
psdfx_composite_with_effects(&dst, &layer, left, top, PSDFX_KEY('n','o','r','m'),
                             1.0f, 1.0f, &fx, doc_box, NULL, 0);
```

効果のはみ出しを含めて描くには、`psdfx_effects_margin(&fx)` だけ広げた面を
用意してください。

## 例: パスの線を描く

```c
psdfx_knot k[4] = {
  { 10, 10, 10, 10, 10, 10 }, { 50, 10, 50, 10, 50, 10 },   /* in, anchor, out */
  { 50, 50, 50, 50, 50, 50 }, { 10, 50, 10, 50, 10, 50 } };
psdfx_subpath sp = { k, 4, 1 /* closed */, -1 };
psdfx_stroke_style st = {0};
st.width = 4;
st.alignment = PSDFX_STROKE_OUTSIDE;
st.join = PSDFX_JOIN_ROUND;
uint8_t mask[64 * 64];
psdfx_stroke_path(&sp, 1, 0, &st, mask, 64, 64, 64, 0, 0);   /* 被覆率 0..255 */
```

PSD のレイヤからは `PSDFile::shapeMask` (C++) がベクタマスクとシェイプの線
(`decodeShape` で読んだ `vstk`) からこれを呼んで被覆率を返します。
