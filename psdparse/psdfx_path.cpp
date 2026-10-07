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

const double kTolerance = 0.1;   // 塗り / 線で曲線を折れ線にするときのずれの上限 (px)
const double kPi = 3.14159265358979323846;

// 3 次ベジェを折れ線へ。分割数は 2 階差分の大きさから、ずれが tol 以下になるように
// 決める (Wang の式)。p0 は含めず、p1 までの点を足す。
// 制御点が p0-p1 の線分上にあれば (直線の区間) true
bool isStraight(Pt p0, Pt c0, Pt c1, Pt p1) {
  const double dx = p1.x - p0.x, dy = p1.y - p0.y, l2 = dx * dx + dy * dy;
  auto on = [&](Pt c) {
    if (l2 < 1e-18) return std::fabs(c.x - p0.x) < 1e-9 && std::fabs(c.y - p0.y) < 1e-9;
    const double t = ((c.x - p0.x) * dx + (c.y - p0.y) * dy) / l2;
    const double cr = (c.x - p0.x) * dy - (c.y - p0.y) * dx;
    return t >= -1e-9 && t <= 1 + 1e-9 && cr * cr / l2 < 1e-12;
  };
  return on(c0) && on(c1);
}

void flattenCubic(std::vector<Pt> &out, Pt p0, Pt c0, Pt c1, Pt p1, double tol = kTolerance) {
  if (isStraight(p0, c0, c1, p1)) { out.push_back(p1); return; }
  const double ax = p0.x - 2 * c0.x + c1.x, ay = p0.y - 2 * c0.y + c1.y;
  const double bx = c0.x - 2 * c1.x + p1.x, by = c0.y - 2 * c1.y + p1.y;
  const double m = std::max(std::hypot(ax, ay), std::hypot(bx, by));
  int n = (int)std::ceil(std::sqrt(0.75 * m / std::max(tol, 1e-3)));
  if (n < 1) n = 1;
  if (n > 1024) n = 1024;
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

// サブパスを折れ線へ。corner[i] は pts[i] がアンカー (角になりうる点) かどうか。
// 長さ 0 の区間は詰める。閉じたパスは始点を繰り返さない。
void flattenSubpath(const psdfx_subpath &sp, double ox, double oy, double tol,
                    std::vector<Pt> &pts, std::vector<char> &corner) {
  pts.clear(); corner.clear();
  if (!sp.knots || sp.count < 1) return;
  const psdfx_knot *k = sp.knots;
  auto push = [&](Pt p, bool c) {
    if (!pts.empty() && std::fabs(pts.back().x - p.x) < 1e-9 && std::fabs(pts.back().y - p.y) < 1e-9) {
      if (c) corner.back() = 1;
      return;
    }
    pts.push_back(p); corner.push_back(c ? 1 : 0);
  };
  push({ k[0].x - ox, k[0].y - oy }, true);
  const int segs = sp.closed ? sp.count : sp.count - 1;
  std::vector<Pt> tmp;
  for (int i = 0; i < segs; i++) {
    const psdfx_knot &a = k[i];
    const psdfx_knot &b = k[(i + 1) % sp.count];
    tmp.clear();
    flattenCubic(tmp, { a.x - ox, a.y - oy }, { a.out_x - ox, a.out_y - oy },
                 { b.in_x - ox, b.in_y - oy }, { b.x - ox, b.y - oy }, tol);
    for (size_t j = 0; j < tmp.size(); j++) push(tmp[j], j + 1 == tmp.size());
  }
  if (sp.closed && pts.size() > 1 && std::fabs(pts.back().x - pts[0].x) < 1e-9 &&
      std::fabs(pts.back().y - pts[0].y) < 1e-9) {
    pts.pop_back(); corner.pop_back();
  }
}

void drawSubpath(Accum &acc, const psdfx_subpath &sp, double ox, double oy) {
  if (!sp.knots || sp.count < 2) return;
  std::vector<Pt> pts;
  std::vector<char> corner;
  flattenSubpath(sp, ox, oy, kTolerance, pts, corner);
  // 塗りは常に閉じた図形として扱う (開いたパスも始点へ戻して塗る)
  for (size_t i = 0; i < pts.size(); i++) acc.line(pts[i], pts[(i + 1) % pts.size()]);
}

// --- 線 -------------------------------------------------------------------
//
// 線は「区間ごとの四角形 + 角 (join) + 端 (cap)」の多角形の集まりとして塗る。
// 多角形の向きをすべてそろえて同じ面に足すと、重なった所は 1 で頭打ちになり
// 和集合になる。

double cross(Pt a, Pt b) { return a.x * b.y - a.y * b.x; }

void addPoly(Accum &acc, const Pt *p, size_t n) {
  if (n < 3) return;
  double area = 0;
  for (size_t i = 0; i < n; i++) area += cross(p[i], p[(i + 1) % n]);
  if (area >= 0) for (size_t i = 0; i < n; i++) acc.line(p[i], p[(i + 1) % n]);
  else for (size_t i = 0; i < n; i++) acc.line(p[(i + 1) % n], p[i]);
}

void addCircle(Accum &acc, Pt c, double r) {
  if (r <= 0) return;
  const double step = 2.0 * std::acos(std::max(-1.0, 1.0 - std::min(kTolerance, r) / r));
  int n = (int)std::ceil(2.0 * kPi / std::max(step, 1e-3));
  n = std::min(256, std::max(8, n));
  std::vector<Pt> p((size_t)n);
  for (int i = 0; i < n; i++) {
    const double a = 2.0 * kPi * i / n;
    p[(size_t)i] = { c.x + r * std::cos(a), c.y + r * std::sin(a) };
  }
  addPoly(acc, p.data(), p.size());
}

struct StrokeParams { double hw; int cap, join; double miterLimit; };

void addJoin(Accum &acc, Pt v, Pt d0, Pt d1, bool isCorner, const StrokeParams &sp) {
  const double cr = cross(d0, d1), dot = d0.x * d1.x + d0.y * d1.y;
  if (std::fabs(cr) < 1e-12 && dot > 0) return;
  if (isCorner && sp.join == PSDFX_JOIN_ROUND) { addCircle(acc, v, sp.hw); return; }
  const double s = cr > 0 ? -1.0 : 1.0;   // 曲がる向きの反対側が外側
  const Pt n0 = { -d0.y * s, d0.x * s }, n1 = { -d1.y * s, d1.x * s };
  const Pt o0 = { v.x + n0.x * sp.hw, v.y + n0.y * sp.hw };
  const Pt o1 = { v.x + n1.x * sp.hw, v.y + n1.y * sp.hw };
  // 角の先端までの長さ / 線幅の半分 = 1 / sin(角の半分)
  const double sinHalf = std::sqrt(std::max(0.0, (1.0 + dot) / 2.0));
  const double ratio = sinHalf > 1e-9 ? 1.0 / sinHalf : 1e9;
  // 曲線の途中の点は角を作らない (隙間を三角形で埋めるだけ)
  if (isCorner && sp.join == PSDFX_JOIN_MITER && ratio <= sp.miterLimit) {
    Pt b = { n0.x + n1.x, n0.y + n1.y };
    const double bl = std::hypot(b.x, b.y);
    if (bl > 1e-12) {
      const Pt m = { v.x + b.x / bl * sp.hw * ratio, v.y + b.y / bl * sp.hw * ratio };
      const Pt q[4] = { v, o0, m, o1 };
      addPoly(acc, q, 4);
      return;
    }
  }
  const Pt t[3] = { v, o0, o1 };
  addPoly(acc, t, 3);
}

void addCap(Accum &acc, Pt p, Pt d, bool atStart, const StrokeParams &sp) {
  if (sp.cap == PSDFX_CAP_ROUND) { addCircle(acc, p, sp.hw); return; }
  if (sp.cap != PSDFX_CAP_SQUARE) return;
  const double k = atStart ? -1.0 : 1.0;
  const Pt n = { -d.y * sp.hw, d.x * sp.hw };
  const Pt e = { p.x + d.x * sp.hw * k, p.y + d.y * sp.hw * k };
  const Pt q[4] = { { p.x + n.x, p.y + n.y }, { e.x + n.x, e.y + n.y },
                    { e.x - n.x, e.y - n.y }, { p.x - n.x, p.y - n.y } };
  addPoly(acc, q, 4);
}

// 折れ線 1 本の線
void strokePolyline(Accum &acc, const std::vector<Pt> &pts, const std::vector<char> &corner,
                    bool closed, const StrokeParams &sp) {
  const size_t n = pts.size();
  if (n == 0 || sp.hw <= 0) return;
  if (n == 1) {   // 長さ 0: 丸 / 四角の端だけ
    if (closed) return;
    if (sp.cap == PSDFX_CAP_ROUND) addCircle(acc, pts[0], sp.hw);
    else if (sp.cap == PSDFX_CAP_SQUARE) {
      const Pt q[4] = { { pts[0].x - sp.hw, pts[0].y - sp.hw }, { pts[0].x + sp.hw, pts[0].y - sp.hw },
                        { pts[0].x + sp.hw, pts[0].y + sp.hw }, { pts[0].x - sp.hw, pts[0].y + sp.hw } };
      addPoly(acc, q, 4);
    }
    return;
  }
  const size_t segs = closed ? n : n - 1;
  std::vector<Pt> dir(segs);
  for (size_t i = 0; i < segs; i++) {
    const Pt a = pts[i], b = pts[(i + 1) % n];
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    dir[i] = len > 1e-12 ? Pt{ (b.x - a.x) / len, (b.y - a.y) / len } : Pt{ 1, 0 };
    const Pt nn = { -dir[i].y * sp.hw, dir[i].x * sp.hw };
    const Pt q[4] = { { a.x + nn.x, a.y + nn.y }, { b.x + nn.x, b.y + nn.y },
                      { b.x - nn.x, b.y - nn.y }, { a.x - nn.x, a.y - nn.y } };
    addPoly(acc, q, 4);
  }
  for (size_t i = closed ? 0 : 1; i < (closed ? n : n - 1); i++) {
    const size_t prev = (i + segs - 1) % segs;
    addJoin(acc, pts[i], dir[prev], dir[i % segs], corner[i] != 0, sp);
  }
  if (!closed) {
    addCap(acc, pts[0], dir[0], true, sp);
    addCap(acc, pts[n - 1], dir[segs - 1], false, sp);
  }
}

// 破線に切る。結果はすべて開いた折れ線。
void dashPolyline(const std::vector<Pt> &pts, const std::vector<char> &corner, bool closed,
                  const std::vector<double> &dash, double offset,
                  std::vector<std::vector<Pt>> &outPts, std::vector<std::vector<char>> &outCorner) {
  double total = 0;
  for (double d : dash) total += d;
  if (total <= 1e-9 || pts.size() < 2) return;
  size_t di = 0;
  double left = 0;
  {   // 始まりの位置
    double o = std::fmod(offset, total);
    if (o < 0) o += total;
    // 長さ 0 の線 (丸い端の点線) は、ちょうどその位置なら描く
    while (o > dash[di] || (o > 0 && o == dash[di])) { o -= dash[di]; di = (di + 1) % dash.size(); }
    left = dash[di] - o;
  }
  std::vector<Pt> cur;
  std::vector<char> curC;
  auto on = [&]() { return (di % 2) == 0; };
  if (on()) { cur.push_back(pts[0]); curC.push_back(corner[0]); }
  const size_t n = pts.size(), segs = closed ? n : n - 1;
  size_t guard = 0;
  for (size_t i = 0; i < segs; i++) {
    Pt a = pts[i];
    const Pt b = pts[(i + 1) % n];
    double len = std::hypot(b.x - a.x, b.y - a.y);
    while (len > left) {
      if (++guard > 4000000) return;   // 極端に細かい破線の歯止め
      const double t = left / len;
      const Pt m = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
      if (on()) {
        cur.push_back(m); curC.push_back(0);
        outPts.push_back(cur); outCorner.push_back(curC);
        cur.clear(); curC.clear();
      } else {
        cur.assign(1, m); curC.assign(1, 0);
      }
      a = m; len -= left;
      di = (di + 1) % dash.size();
      left = dash[di];
    }
    left -= len;
    if (on()) { cur.push_back(b); curC.push_back(corner[(i + 1) % n]); }
  }
  if (on() && cur.size() > 1) { outPts.push_back(cur); outCorner.push_back(curC); }
  // 終点ちょうどで始まる長さ 0 の線 (点線の最後の点)
  if (!on() && left <= 1e-9 && !closed) {
    const size_t next = (di + 1) % dash.size();
    if (dash[next] <= 1e-9) {
      outPts.push_back({ pts[n - 1], pts[n - 1] });
      outCorner.push_back({ 0, 0 });
    }
  }
}

}  // anonymous namespace

extern "C" int psdfx_flatten_subpath(const psdfx_subpath *subpath, double tolerance,
                                     double *xy, int max_points) {
  if (!subpath) return 0;
  std::vector<Pt> pts;
  std::vector<char> corner;
  flattenSubpath(*subpath, 0, 0, tolerance > 0 ? tolerance : kTolerance, pts, corner);
  const int n = (int)pts.size();
  if (xy && max_points >= n)
    for (int i = 0; i < n; i++) { xy[i * 2] = pts[(size_t)i].x; xy[i * 2 + 1] = pts[(size_t)i].y; }
  return n;
}

extern "C" void psdfx_stroke_path(const psdfx_subpath *subpaths, int count, int initial_fill,
                                  const psdfx_stroke_style *style,
                                  uint8_t *mask, int width, int height, int stride,
                                  double offset_x, double offset_y) {
  if (!mask || !style || width <= 0 || height <= 0) return;
  for (int y = 0; y < height; y++) std::fill(mask + (size_t)y * stride, mask + (size_t)y * stride + width, 0);
  if (style->width <= 0 || count <= 0) return;
  const bool sided = style->alignment == PSDFX_STROKE_INSIDE || style->alignment == PSDFX_STROKE_OUTSIDE;
  std::vector<double> dash;
  for (int i = 0; i < style->dash_count && style->dashes; i++) dash.push_back(std::max(0.0, style->dashes[i]));
  if (dash.size() % 2) dash.insert(dash.end(), dash.begin(), dash.end());   // 奇数個は繰り返して偶数に
  double dashTotal = 0;
  for (double d : dash) dashTotal += d;
  if (dashTotal <= 1e-9) dash.clear();
  // 内側 / 外側の線は、閉じたパスの両側に線幅ぶん描いてから形で切る
  Accum centered(width, height), twoSided(width, height);
  bool anyTwoSided = false;
  std::vector<Pt> pts;
  std::vector<char> corner;
  for (int i = 0; i < count; i++) {
    const psdfx_subpath &s = subpaths[i];
    flattenSubpath(s, offset_x, offset_y, kTolerance, pts, corner);
    const bool closed = s.closed && pts.size() > 2;
    const bool two = sided && closed;
    StrokeParams sp{ two ? style->width : style->width / 2.0, style->cap, style->join,
                     style->miter_limit > 0 ? style->miter_limit : 4.0 };
    Accum &acc = two ? twoSided : centered;
    anyTwoSided |= two;
    if (dash.empty()) {
      strokePolyline(acc, pts, corner, closed, sp);
    } else {
      std::vector<std::vector<Pt>> dp;
      std::vector<std::vector<char>> dc;
      dashPolyline(pts, corner, closed, dash, style->dash_offset, dp, dc);
      for (size_t k = 0; k < dp.size(); k++) strokePolyline(acc, dp[k], dc[k], false, sp);
    }
  }
  std::vector<float> cov, side;
  centered.resolve(cov);
  if (anyTwoSided) {
    twoSided.resolve(side);
    std::vector<uint8_t> shape((size_t)width * height);
    psdfx_fill_path(subpaths, count, initial_fill, shape.data(), width, height, width,
                    offset_x, offset_y);
    const bool inside = style->alignment == PSDFX_STROKE_INSIDE;
    for (size_t p = 0; p < cov.size(); p++) {
      const float f = shape[p] / 255.f;
      const float s = side[p] * (inside ? f : 1.f - f);
      cov[p] = cov[p] + s - cov[p] * s;
    }
  }
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++)
      mask[(size_t)y * stride + x] =
          (uint8_t)(std::min(1.f, std::max(0.f, cov[(size_t)y * width + x])) * 255.f + 0.5f);
}

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
