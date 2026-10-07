// psdfx — パスの塗り (ベジェの平坦化と、面積累積によるアンチエイリアス付きの塗り)。
//
// 塗りは「辺が各画素にかける符号付き面積を累積し、行ごとに左から足し合わせる」
// 方式 (フォントのラスタライザで広く使われるもの)。足し合わせた値の絶対値を
// 1 で頭打ちにしたものが被覆率になる (非ゼロ巻き数の近似)。
//
// サブパスの合成は psd-tools の読み方に合わせる: 合成方法 -1 のサブパスは直前と
// ひとまとまりで塗り、まとまりどうしを 和 / 差 / 交差 / 中マド で重ねる。最初の
// まとまりが 差 / 交差 なら全面から始める。
#include "psdfx.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

struct Pt { double x, y; };

// 3 次ベジェを折れ線へ (区間の長さに応じて分割数を決める)
void flattenCubic(std::vector<Pt> &out, Pt p0, Pt c0, Pt c1, Pt p1) {
  double len = std::hypot(c0.x - p0.x, c0.y - p0.y) + std::hypot(c1.x - c0.x, c1.y - c0.y) +
               std::hypot(p1.x - c1.x, p1.y - c1.y);
  int n = (int)std::ceil(std::sqrt(len) * 1.5);
  if (n < 1) n = 1;
  if (n > 256) n = 256;
  for (int i = 1; i <= n; i++) {
    double t = (double)i / n, u = 1.0 - t;
    double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
    out.push_back({ a * p0.x + b * c0.x + c * c1.x + d * p1.x,
                    a * p0.y + b * c0.y + c * c1.y + d * p1.y });
  }
}

class Accum {
public:
  Accum(int w, int h) : w_(w), h_(h), a_((size_t)(w + 2) * h, 0.f) {}

  // 1 本の辺を足す (font-rs の draw_line と同じ考え方)
  void line(Pt p0, Pt p1) {
    if (std::fabs(p0.y - p1.y) < 1e-9) return;
    // 左右は [0, w] に寄せる (面の外側の辺も、右側への巻き数には効く)
    p0.x = std::min(std::max(p0.x, 0.0), (double)w_);
    p1.x = std::min(std::max(p1.x, 0.0), (double)w_);
    float dir = 1.f;
    if (p0.y > p1.y) { std::swap(p0, p1); dir = -1.f; }
    const double dxdy = (p1.x - p0.x) / (p1.y - p0.y);
    double x = p0.x;
    int y0 = (int)std::floor(p0.y);
    if (p0.y < 0.0) { x -= p0.y * dxdy; y0 = 0; }
    const int yEnd = std::min(h_, (int)std::ceil(p1.y));
    for (int y = std::max(0, y0); y < yEnd; y++) {
      const double dy = std::min((double)(y + 1), p1.y) - std::max((double)y, p0.y);
      const double xnext = x + dxdy * dy;
      const float d = (float)dy * dir;
      double x0 = std::min(x, xnext), x1 = std::max(x, xnext);
      float *row = &a_[(size_t)y * (w_ + 2)];
      const double x0floor = std::floor(x0);
      const int x0i = (int)x0floor;
      const double x1ceil = std::ceil(x1);
      const int x1i = (int)x1ceil;
      if (x1i <= x0i + 1) {
        const double xmf = 0.5 * (x + xnext) - x0floor;
        add(row, x0i, d - d * (float)xmf);
        add(row, x0i + 1, d * (float)xmf);
      } else {
        const double s = 1.0 / (x1 - x0);
        const double x0f = x0 - x0floor;
        const double a0 = 0.5 * s * (1.0 - x0f) * (1.0 - x0f);
        const double x1f = x1 - x1ceil + 1.0;
        const double am = 0.5 * s * x1f * x1f;
        add(row, x0i, d * (float)a0);
        if (x1i == x0i + 2) {
          add(row, x0i + 1, d * (float)(1.0 - a0 - am));
        } else {
          const double a1 = s * (1.5 - x0f);
          add(row, x0i + 1, d * (float)(a1 - a0));
          for (int xi = x0i + 2; xi < x1i - 1; xi++) add(row, xi, d * (float)s);
          const double a2 = a1 + (x1i - x0i - 3) * s;
          add(row, x1i - 1, d * (float)(1.0 - a2 - am));
        }
        add(row, x1i, d * (float)am);
      }
      x = xnext;
    }
  }

  // 被覆率 (0..1) を out (w x h) へ
  void resolve(std::vector<float> &out) const {
    out.assign((size_t)w_ * h_, 0.f);
    for (int y = 0; y < h_; y++) {
      const float *row = &a_[(size_t)y * (w_ + 2)];
      float acc = 0.f;
      for (int x = 0; x < w_; x++) {
        acc += row[x];
        out[(size_t)y * w_ + x] = std::min(1.f, std::fabs(acc));
      }
    }
  }

private:
  int w_, h_;
  std::vector<float> a_;
  void add(float *row, int x, float v) {
    if (x < 0) x = 0;
    if (x > w_ + 1) x = w_ + 1;
    row[x] += v;
  }
};

void drawSubpath(Accum &acc, const psdfx_subpath &sp, double ox, double oy) {
  if (!sp.knots || sp.count < 2) return;
  std::vector<Pt> pts;
  const psdfx_knot *k = sp.knots;
  pts.push_back({ k[0].x - ox, k[0].y - oy });
  const int segs = sp.closed ? sp.count : sp.count - 1;
  for (int i = 0; i < segs; i++) {
    const psdfx_knot &a = k[i];
    const psdfx_knot &b = k[(i + 1) % sp.count];
    flattenCubic(pts, { a.x - ox, a.y - oy }, { a.out_x - ox, a.out_y - oy },
                 { b.in_x - ox, b.in_y - oy }, { b.x - ox, b.y - oy });
  }
  // 塗りは常に閉じた図形として扱う (開いたパスも始点へ戻して塗る)
  for (size_t i = 0; i < pts.size(); i++) acc.line(pts[i], pts[(i + 1) % pts.size()]);
}

}  // anonymous namespace

extern "C" void psdfx_fill_path(const psdfx_subpath *subpaths, int count, int initial_fill,
                                uint8_t *mask, int width, int height, int stride,
                                double offset_x, double offset_y) {
  if (!mask || width <= 0 || height <= 0) return;
  const size_t n = (size_t)width * height;
  std::vector<float> cov(n, initial_fill && count == 0 ? 1.f : 0.f);
  std::vector<float> plane;
  bool first = true;
  for (int i = 0; i < count; ) {
    // 合成方法 -1 が続くサブパスはひとまとまりで塗る
    int j = i + 1;
    while (j < count && subpaths[j].operation == -1) j++;
    Accum acc(width, height);
    for (int k = i; k < j; k++) drawSubpath(acc, subpaths[k], offset_x, offset_y);
    acc.resolve(plane);
    const int op = subpaths[i].operation;
    if (first && (op == 2 || op == 3)) for (float &c : cov) c = 1.f - c;
    for (size_t p = 0; p < n; p++) {
      float m = cov[p], s = plane[p];
      switch (op) {
      case 0:  m = m + s - 2.f * m * s; break;          // 中マド
      case 2:  m = std::max(0.f, m - s); break;          // 差
      case 3:  m = m * s; break;                         // 交差
      default: m = m + s - m * s; break;                 // 和 (1 / -1)
      }
      cov[p] = m;
    }
    first = false;
    i = j;
  }
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++)
      mask[(size_t)y * stride + x] =
          (uint8_t)(std::min(1.f, std::max(0.f, cov[(size_t)y * width + x])) * 255.f + 0.5f);
}
