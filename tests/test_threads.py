"""並列処理 (set_threads): スレッド数を変えても合成の結果が同じこと。"""
import os

import pytest

import psdparse

DATA = os.path.join(os.path.dirname(__file__), "data")


@pytest.fixture
def restore_threads():
    yield
    psdparse.set_threads(0)


def test_get_threads_follows_setting(restore_threads):
    psdparse.set_threads(3)
    assert psdparse.get_threads() == 3
    psdparse.set_threads(1)
    assert psdparse.get_threads() == 1
    psdparse.set_threads(0)
    assert psdparse.get_threads() >= 1


@pytest.mark.parametrize("name", ["config.psd", "system.psd"])
def test_composite_same_for_any_thread_count(name, restore_threads):
    p = psdparse.PSDFile()
    assert p.load(os.path.join(DATA, name))
    results = []
    for n in (1, 2, 5):
        psdparse.set_threads(n)
        for effects in (True, False):
            img, st = p.composite(effects=effects)
            results.append((n, effects, img, st))
    for effects in (True, False):
        imgs = [r for r in results if r[1] == effects]
        assert all(r[2] == imgs[0][2] for r in imgs), name
        assert all(r[3] == imgs[0][3] for r in imgs), name


def test_render_layer_same_for_any_thread_count(restore_threads):
    p = psdparse.PSDFile()
    assert p.load(os.path.join(DATA, "system.psd"))
    idx = [i for i, l in enumerate(p.layers) if l.width * l.height > 40000][:4]
    assert idx
    for i in idx:
        psdparse.set_threads(1)
        a = p.render_layer(i)
        psdparse.set_threads(4)
        b = p.render_layer(i)
        assert a == b


def test_simd_matches_scalar():
    """SIMD 版 (AVX2) を切った子プロセスの合成と、この中の合成がバイト一致する"""
    import hashlib
    import subprocess
    import sys
    path = os.path.join(DATA, "system.psd")
    code = ("import hashlib, psdparse; p = psdparse.PSDFile(); p.load(r'%s'); "
            "print(hashlib.md5(p.composite()[0]).hexdigest())" % path)
    env = dict(os.environ, PSDFX_SIMD="0")
    env["PYTHONPATH"] = os.pathsep.join(sys.path)
    scalar = subprocess.run([sys.executable, "-c", code], env=env, capture_output=True, text=True,
                            check=True).stdout.strip()
    p = psdparse.PSDFile()
    assert p.load(path)
    assert hashlib.md5(p.composite()[0]).hexdigest() == scalar
