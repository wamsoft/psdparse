// psdfx 内部: 行などの範囲を複数のスレッドで分けて処理する
#pragma once

#include <functional>

namespace psdfx_internal {

// [begin, end) を区切って fn(lo, hi) を並列に呼ぶ。work は処理量の目安 (画素数など)。
// 少ないとき・スレッド数 1 のとき・並列処理の中から呼ばれたときは呼び出し元で
// そのまま fn(begin, end) を呼ぶ。fn どうしは同じ場所へ書かないこと。
void parallelFor(int begin, int end, long long work, const std::function<void(int, int)> &fn);

// これより少ない処理量は分けない (スレッドに渡す手間のほうが大きい)
constexpr long long kParallelMinWork = 1 << 15;

}  // namespace psdfx_internal
