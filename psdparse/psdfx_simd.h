// psdfx 内部: 合成の行処理の SIMD 版 (使える CPU なら実行時に選ぶ)
#pragma once

#include <cstdint>

namespace psdfx_internal {

// 1 行の先頭から 8 画素単位で合成し、処理した画素数を返す (残りは呼び出し側の
// スカラー処理で)。スカラー版と同じ式・同じ演算順で、結果はバイト単位で一致する。
// 対応しないモードなら 0 を返す。
//   d   下地 (BGRA8、ストレートアルファ)、書き換える
//   s   重ねる面 (BGRA8)
//   m   マスク (NULL 可、1 画素 1 バイト)
//   key ブレンドモード、op 不透明度 (0..1)、atop は source-atop (下地のアルファを保つ)
int compositeRowSimd(uint8_t *d, const uint8_t *s, const uint8_t *m, int n, uint32_t key, float op,
                     bool atop);

// この CPU で SIMD 版が使えるか (AVX2)
bool simdAvailable();

}  // namespace psdfx_internal
