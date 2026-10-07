/*
 * psdfx — Photoshop 互換の合成とレイヤー効果の画像処理 (C API)
 *
 * psdparse に依存しない、BGRA 8bit (ストレートアルファ) の面を扱う関数群。
 * 他のプログラム (プラグイン等) からも、PSD を読まずにこれだけで使える。
 *
 *   - 面の合成: Photoshop のブレンドモード一式 (4 文字キー 'norm' / 'mul ' など)
 *   - 基本処理: アルファのぼかし・距離変換など (効果の部品)
 *   - レイヤー効果: 影 / 光彩 / 線 / オーバーレイ / ベベル / サテン
 *
 * 面はすべて「左上の画素から、1 行 stride バイト、1 画素 B,G,R,A の 4 バイト」。
 * 色はストレートアルファ (乗算済みではない)。
 */
#ifndef PSDFX_H
#define PSDFX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* BGRA 8bit の面 (所有しない) */
typedef struct psdfx_surface {
  uint8_t *pixels;
  int width;
  int height;
  int stride;      /* 1 行のバイト数 (通常 width * 4) */
} psdfx_surface;

/* ブレンドモードの 4 文字キーを整数にしたもの (例 PSDFX_KEY('m','u','l',' '))。
 * PSD のレイヤレコード / 効果の descriptor に入っている値そのまま。 */
#define PSDFX_KEY(a, b, c, d) \
  (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | \
   ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

/* 対応していれば 1。'pass' (通過) は面の合成としては 'norm' と同じに扱う。 */
int psdfx_blend_supported(uint32_t blend_key);

/*
 * src を dst の (dx, dy) の位置へ、ブレンドモード blend_key・不透明度 opacity
 * (0..1) で重ねる (source-over)。dst からはみ出る部分は捨てる。
 * mask が NULL でなければ、src と同じ大きさの 8bit 面 (1 行 mask_stride バイト)
 * で src のアルファをさらに掛ける。
 */
void psdfx_composite(psdfx_surface *dst, const psdfx_surface *src, int dx, int dy,
                     uint32_t blend_key, float opacity,
                     const uint8_t *mask, int mask_stride);

/*
 * クリッピング用: src を dst へ重ねるが、dst のアルファは変えない
 * (dst が不透明な所にだけ描く。source-atop)。
 */
void psdfx_composite_atop(psdfx_surface *dst, const psdfx_surface *src, int dx, int dy,
                          uint32_t blend_key, float opacity);

/* dst と src (同じ大きさ) を t (0..1) で線形補間する: dst = dst + (src - dst) * t。
 * mask (dst と同じ大きさの 8bit 面、NULL 可) があれば画素ごとに t へ掛ける。
 * 通過グループに不透明度やマスクがあるときに使う。 */
void psdfx_lerp(psdfx_surface *dst, const psdfx_surface *src, float t,
                const uint8_t *mask, int mask_stride);

/* ------------------------------------------------------------------------
 * 基本処理 (8bit の 1 チャンネル面)
 * ------------------------------------------------------------------------ */

/* ガウスぼかし (箱ぼかし 3 回で近似)。sigma はピクセル。その場で書き換える。 */
void psdfx_blur_plane(uint8_t *plane, int width, int height, int stride, double sigma);

/* ------------------------------------------------------------------------
 * 塗り (グラデーション / パターン)
 * ------------------------------------------------------------------------ */

typedef struct psdfx_color_stop {
  double location;        /* 0..1 */
  double midpoint;        /* 0..1 (次の分岐点との中間点。既定 0.5) */
  uint8_t r, g, b;
} psdfx_color_stop;

typedef struct psdfx_alpha_stop {
  double location;        /* 0..1 */
  double midpoint;        /* 0..1 */
  double opacity;         /* 0..1 */
} psdfx_alpha_stop;

typedef struct psdfx_gradient {
  const psdfx_color_stop *colors; int color_count;
  const psdfx_alpha_stop *alphas; int alpha_count;
  double smoothness;      /* 0..1 (Photoshop の滑らかさ。既定 1.0) */
} psdfx_gradient;

enum {
  PSDFX_GRADIENT_LINEAR = 0,
  PSDFX_GRADIENT_RADIAL = 1,
  PSDFX_GRADIENT_ANGLE = 2,
  PSDFX_GRADIENT_REFLECTED = 3,
  PSDFX_GRADIENT_DIAMOND = 4
};

/* 位置 t (0..1) のグラデーションの色 (r, g, b, a: 0..255) */
void psdfx_gradient_color(const psdfx_gradient *g, double t, uint8_t rgba[4]);

/*
 * dst (左上が文書の (dst_left, dst_top)) をグラデーションで塗る (上書き)。
 * 基準の矩形 box (文書座標の left, top, right, bottom) の中心を
 * (offset_x, offset_y) % (矩形の幅 / 高さに対する割合) だけずらした点が中心。
 * angle は度 (反時計回り、0 = 右向き)、scale は 1.0 = 100%。
 */
void psdfx_draw_gradient(psdfx_surface *dst, int dst_left, int dst_top,
                         const psdfx_gradient *g, int style, double angle, double scale,
                         int reverse, const double box[4], double offset_x, double offset_y);

/*
 * dst (左上が文書の (dst_left, dst_top)) をパターン tile の繰り返しで塗る
 * (上書き)。タイルの原点は文書の (origin_x, origin_y)、scale は 1.0 = 100%。
 */
void psdfx_draw_pattern(psdfx_surface *dst, int dst_left, int dst_top,
                        const psdfx_surface *tile, double scale, double origin_x, double origin_y);

/* ------------------------------------------------------------------------
 * パスの塗り (ベクタマスク / シェイプ)
 * ------------------------------------------------------------------------ */

/* 3 次ベジェの knot (アンカーと前後の制御点。座標は面のピクセル) */
typedef struct psdfx_knot {
  double in_x, in_y;      /* 前の区間の制御点 (アンカーへ入ってくる側) */
  double x, y;            /* アンカー */
  double out_x, out_y;    /* 次の区間の制御点 (アンカーから出ていく側) */
} psdfx_knot;

/* サブパス 1 本。operation は Photoshop のパスの合成方法:
 *   -1 = 直前のサブパスとひとまとまり、1 = 和 (結合)、2 = 差 (前面の型抜き)、
 *   3 = 交差、0 = 中マド (除外) */
typedef struct psdfx_subpath {
  const psdfx_knot *knots;
  int count;
  int closed;
  int operation;
} psdfx_subpath;

/*
 * サブパス群を塗り、被覆率 (0..255、アンチエイリアス付き) を mask へ書く
 * (mask は width x height、1 行 stride バイト)。座標は (offset_x, offset_y)
 * だけずらしてから使う (面の左上が文書のどこかを指定する)。
 * initial_fill が 0 以外なら全面が塗られた状態から始める (PSD のパスの
 * 「初期塗りつぶしルール」)。塗り規則は非ゼロ巻き数。
 */
void psdfx_fill_path(const psdfx_subpath *subpaths, int count, int initial_fill,
                     uint8_t *mask, int width, int height, int stride,
                     double offset_x, double offset_y);

/* ------------------------------------------------------------------------
 * レイヤー効果
 *
 * 長さはすべてピクセル (PSD の効果全体の拡大率は呼び出し側で掛けておく)。
 * 角度は度 (光源の向き。0 = 右、90 = 上)。不透明度は 0..1。
 * ------------------------------------------------------------------------ */

/* 塗りの元 (線・光彩・オーバーレイで共通) */
enum { PSDFX_FILL_SOLID = 0, PSDFX_FILL_GRADIENT = 1, PSDFX_FILL_PATTERN = 2 };

typedef struct psdfx_fill_source {
  int kind;                       /* PSDFX_FILL_* */
  uint8_t color[3];               /* 単色 (R, G, B) */
  psdfx_gradient gradient;        /* グラデーション */
  int gradient_style;             /* PSDFX_GRADIENT_* */
  double angle;                   /* グラデーションの角度 (度) */
  double scale;                   /* グラデーション / パターンの比率 (1.0 = 100%) */
  int reverse;
  int align_with_layer;           /* 1: レイヤの範囲を基準にする / 0: 文書 */
  double offset_x, offset_y;      /* グラデーションの中心のずらし (%) */
  const psdfx_surface *pattern;   /* パターンのタイル */
  double phase_x, phase_y;        /* パターンの位相 (px) */
} psdfx_fill_source;

typedef struct psdfx_shadow {     /* ドロップシャドウ / シャドウ (内側) */
  int enabled;
  uint32_t blend;                 /* PSDFX_KEY('m','u','l',' ') など */
  float opacity;
  uint8_t color[3];
  double angle, distance;
  double spread;                  /* 0..1 (内側ではチョーク) */
  double size;
  int knocks_out;                 /* ドロップシャドウ: レイヤの下の影を抜く */
} psdfx_shadow;

typedef struct psdfx_glow {       /* 光彩 (外側 / 内側) */
  int enabled;
  uint32_t blend;
  float opacity;
  psdfx_fill_source fill;         /* 単色 or グラデーション (中心 → 外へ) */
  double spread;                  /* 0..1 (内側ではチョーク) */
  double size;
  int precise;                    /* 0: さらにソフト / 1: 精細 */
  int source_center;              /* 内側: 1 = 中央から / 0 = エッジから */
} psdfx_glow;

enum { PSDFX_STROKE_OUTSIDE = 0, PSDFX_STROKE_INSIDE = 1, PSDFX_STROKE_CENTER = 2 };

typedef struct psdfx_stroke {     /* 境界線 */
  int enabled;
  uint32_t blend;
  float opacity;
  double size;
  int position;                   /* PSDFX_STROKE_* */
  psdfx_fill_source fill;
} psdfx_stroke;

typedef struct psdfx_overlay {    /* カラー / グラデーション / パターンオーバーレイ */
  int enabled;
  uint32_t blend;
  float opacity;
  psdfx_fill_source fill;
} psdfx_overlay;

typedef struct psdfx_satin {      /* サテン */
  int enabled;
  uint32_t blend;
  float opacity;
  uint8_t color[3];
  double angle, distance, size;
  int invert;
} psdfx_satin;

enum { PSDFX_BEVEL_OUTER = 0, PSDFX_BEVEL_INNER = 1, PSDFX_BEVEL_EMBOSS = 2,
       PSDFX_BEVEL_PILLOW = 3 };

typedef struct psdfx_bevel {      /* ベベルとエンボス */
  int enabled;
  int style;                      /* PSDFX_BEVEL_* */
  int up;                         /* 1: 上へ / 0: 下へ */
  double depth;                   /* 0..10 (1.0 = 100%) */
  double size, soften;
  double angle, altitude;         /* 光源 */
  uint32_t highlight_blend, shadow_blend;
  float highlight_opacity, shadow_opacity;
  uint8_t highlight_color[3], shadow_color[3];
} psdfx_bevel;

typedef struct psdfx_layer_effects {
  psdfx_shadow drop_shadow, inner_shadow;
  psdfx_glow outer_glow, inner_glow;
  psdfx_stroke stroke;
  psdfx_overlay color_overlay, gradient_overlay, pattern_overlay;
  psdfx_satin satin;
  psdfx_bevel bevel;
} psdfx_layer_effects;

/* 効果がレイヤの外へはみ出す量 (px)。描画範囲を広げるのに使う。 */
int psdfx_effects_margin(const psdfx_layer_effects *fx);

/*
 * レイヤ layer (左上が dst 上の (left, top)) を、効果込みで dst へ重ねる。
 *   - ドロップシャドウ / 光彩 (外側) は下地へそれぞれのブレンドで重ねる
 *   - 内側の効果 (オーバーレイ / シャドウ・光彩 (内側) / サテン / ベベル) と
 *     境界線はレイヤの形の中 (境界線は外側も) で重ね、それをレイヤの
 *     ブレンドモード blend・不透明度 opacity で下地へ重ねる
 *   - fill_opacity (塗りの不透明度) はレイヤの画素だけに掛かり、効果には掛からない
 * doc_box は文書の矩形 (left, top, right, bottom、dst と同じ座標系)。
 * 「レイヤに整列」しないグラデーションやパターンの基準に使う。
 */
void psdfx_composite_with_effects(psdfx_surface *dst, const psdfx_surface *layer,
                                  int left, int top, uint32_t blend, float opacity,
                                  float fill_opacity, const psdfx_layer_effects *fx,
                                  const double doc_box[4]);

#ifdef __cplusplus
}
#endif

#endif /* PSDFX_H */
