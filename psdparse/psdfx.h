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

#ifdef __cplusplus
}
#endif

#endif /* PSDFX_H */
