"""Photoshop が描いた合成画像との比較 (tools/ps_oracle.py で作ったファイル)。

環境変数 PSDPARSE_PS_ORACLE に tools/ps_oracle.py の出力フォルダを指定すると、
その中の各 PSD で composite() と保存済み合成画像 (Photoshop の描画) を比べる。
許す誤差は同じフォルダの limits.json (ケースごとの最大 / 平均)。
"""
import json
import os

import numpy as np
import pytest

import psdparse

ROOT = os.environ.get("PSDPARSE_PS_ORACLE")


def _cases():
    if not ROOT or not os.path.isdir(ROOT):
        return []
    limits = {}
    path = os.path.join(ROOT, "limits.json")
    if os.path.exists(path):
        limits = json.load(open(path, encoding="utf-8"))
    out = []
    for name in sorted(os.listdir(ROOT)):
        if name.endswith(".psd"):
            out.append((name, limits.get(name[:-4], {"max": 8, "mean": 1.0})))
    return out


@pytest.mark.skipif(not _cases(), reason="set PSDPARSE_PS_ORACLE to a folder made by tools/ps_oracle.py")
@pytest.mark.parametrize("name,limit", _cases() or [("none", {})])
def test_matches_photoshop(name, limit):
    p = psdparse.PSDFile()
    assert p.load(os.path.join(ROOT, name))
    h = p.header
    ref = np.frombuffer(p.merged_image(), np.uint8).reshape(h.height, h.width, 4)[:, :, :3].astype(int)
    out, stats = p.composite()
    got = np.frombuffer(out, np.uint8).reshape(h.height, h.width, 4)[:, :, :3].astype(int)
    d = np.abs(got - ref)
    assert stats["skipped_adjustments"] == 0
    assert d.max() <= limit["max"], (name, int(d.max()))
    assert d.mean() <= limit["mean"], (name, float(d.mean()))
