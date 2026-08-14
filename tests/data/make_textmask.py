"""マスク付きテキストレイヤのサンプル PSD を作る。

textboxsample.psd をもとに、2 枚のテキストレイヤへレイヤマスクを足す。
「言語ごとにテキストレイヤを複製し、書き出し範囲をマスクで切る」という
実作業に近い形にしてある (複製でマスクが引き継がれるかの確認用)。

  layer 1 (横書き) : 右側を隠す縦のグラデーション。レイヤ矩形より小さい
                     マスク矩形にして、幾何が保たれるかも見えるようにする
  layer 2 (縦書き) : 下半分を隠す単純な帯

  python make_textmask.py <出力先.psd>
"""
import sys
from pathlib import Path

import psdparse

SRC = str(Path(__file__).with_name("textboxsample.psd"))


def gradient_mask(w, h):
    """左は見える (255)、右へ向かって隠れる (0) グラデーション"""
    out = bytearray(w * h)
    for y in range(h):
        row = y * w
        for x in range(w):
            v = 255 - int(255 * x / max(1, w - 1))
            out[row + x] = v
    return bytes(out)


def band_mask(w, h):
    """上半分だけ見える帯"""
    out = bytearray(w * h)
    for y in range(h):
        v = 255 if y < h // 2 else 0
        out[y * w:(y + 1) * w] = bytes([v]) * w
    return bytes(out)


def main(dst):
    p = psdparse.PSDFile()
    if not p.load(SRC):
        raise SystemExit("could not load " + SRC)

    # --- 横書きのテキストレイヤ (index 1) ---
    l = p.layers[1]
    # レイヤ矩形より一回り小さいマスク矩形にする (幾何が保たれるかの確認用)
    left, top = l.left + 20, l.top + 10
    w, h = (l.right - l.left) - 40, (l.bottom - l.top) - 20
    p.set_layer_mask_pixels(1, gradient_mask(w, h), top, left, w, h)

    # --- 縦書きのテキストレイヤ (index 2) ---
    l2 = p.layers[2]
    w2, h2 = l2.right - l2.left, l2.bottom - l2.top
    p.set_layer_mask_pixels(2, band_mask(w2, h2), l2.top, l2.left, w2, h2)

    if not p.save(dst):
        raise SystemExit("could not save " + dst)

    # 保存したものを読み直して確認する
    q = psdparse.PSDFile()
    q.load(dst)
    for i in (1, 2):
        m = q.layers[i].mask
        assert m, "layer %d lost its mask" % i
        print("layer %d: text=%s mask rect=(%d,%d)-(%d,%d)" % (
            i, bool(q.layers[i].text), m["left"], m["top"], m["right"], m["bottom"]))
    print("written:", dst)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "textmask.psd")
