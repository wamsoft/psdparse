"""Photoshop を正解にした検証用 PSD を作る (Windows + Photoshop が入っている環境用)。

Photoshop を COM (Photoshop.Application) 経由で JavaScript で動かし、
「色見本の画像 + 調整レイヤ 1 枚」のような PSD を作って保存させる。保存した
PSD の合成画像は Photoshop 自身が描いたものなので、psdparse の composite() と
比べれば描画の正しさを確かめられる。

    python tools/ps_oracle.py <出力フォルダ> [ケース名の一部 ...]

作ったファイルは tests/test_ps_oracle.py が環境変数 PSDPARSE_PS_ORACLE で
指定したフォルダから読む (リポジトリには入れない。1 ファイル 200KB ほど)。

既存の PSD のレイヤの表示を切り替えて保存し直す variant() もある (調整レイヤが
何枚も重なったファイルを 1 枚ずつ確かめるのに使う)。
"""
import json
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

_PS1 = r"""
param([string]$jsx)
$ErrorActionPreference = 'Stop'
$ps = New-Object -ComObject Photoshop.Application
$r = $ps.DoJavaScriptFile($jsx)
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Write-Output $r
"""

_PRELUDE = r"""
app.displayDialogs = DialogModes.NO;
app.preferences.rulerUnits = Units.PIXELS;
app.preferences.maximizeCompatibility = QueryStateType.ALWAYS;
function jpath(p) { return new File(p); }
function savePSD(doc, path) {
  var o = new PhotoshopSaveOptions();
  o.layers = true; o.alphaChannels = true; o.embedColorProfile = false;
  doc.saveAs(new File(path), o, true, Extension.LOWERCASE);
}
function cTID(s) { return charIDToTypeID(s); }
function sTID(s) { return stringIDToTypeID(s); }
"""


def run_jsx(code, timeout=600):
    """JSX を Photoshop で実行し、最後に return した値を文字列で返す"""
    tmp = tempfile.mkdtemp(prefix="ps_oracle_")
    ps1 = os.path.join(tmp, "run.ps1")
    jsx = os.path.join(tmp, "run.jsx")
    open(ps1, "w", encoding="utf-8").write(_PS1)
    with open(jsx, "w", encoding="utf-8-sig") as f:
        f.write(_PRELUDE + "\n(function(){\n" + code + "\n})();")
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps1, jsx],
                           capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
    finally:
        for p in (ps1, jsx):
            os.remove(p)
        os.rmdir(tmp)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip() or r.stdout.strip())
    return r.stdout.strip()


def _js(p):
    return json.dumps(os.path.abspath(p).replace("\\", "/"))


# --- 元になる画像 ---------------------------------------------------------------

def cube_png(folder):
    """16 段の RGB 立方体 (4096 色) を 64x64 に並べた画像"""
    path = os.path.join(folder, "_cube.png")
    if not os.path.exists(path):
        v = np.arange(16) * 17
        r, g, b = np.meshgrid(v, v, v, indexing="ij")
        img = np.stack([r.ravel(), g.ravel(), b.ravel()], 1).reshape(64, 64, 3).astype(np.uint8)
        Image.fromarray(img, "RGB").save(path)
    return path


def ramp_png(folder):
    """256 段の灰色と色のランプ"""
    path = os.path.join(folder, "_ramp.png")
    if not os.path.exists(path):
        v = np.arange(256, dtype=np.uint8)
        rows = [np.stack([v, v, v], 1), np.stack([v, v // 2, 255 - v], 1),
                np.stack([v, 255 - v, v // 3], 1), np.stack([255 - v, v, 128 + v // 2], 1)]
        img = np.concatenate([np.repeat(r[None], 4, 0) for r in rows], 0)
        Image.fromarray(img.astype(np.uint8), "RGB").save(path)
    return path


def make(path, base_png, body_js):
    """base_png を開いて body_js (変数 d が文書) を実行し、PSD で保存する"""
    run_jsx(f"""
var d = app.open(jpath({_js(base_png)}));
if (d.mode != DocumentMode.RGB) d.changeMode(ChangeMode.RGB);
d.bitsPerChannel = BitsPerChannelType.EIGHT;
d.activeLayer.isBackgroundLayer = false;
{body_js}
savePSD(d, {_js(path)});
d.close(SaveOptions.DONOTSAVECHANGES);
return "ok";
""")
    return path


def variant(src, dst, visible):
    """既存の PSD のレイヤの表示を {名前: bool} で切り替えて保存し直す"""
    run_jsx(f"""
var d = app.open(jpath({_js(src)}));
var want = {json.dumps(visible, ensure_ascii=False)};
function walk(layers) {{
  for (var i = 0; i < layers.length; i++) {{
    var l = layers[i];
    if (want.hasOwnProperty(l.name)) l.visible = want[l.name];
    if (l.typename == "LayerSet") walk(l.layers);
  }}
}}
walk(d.layers);
savePSD(d, {_js(dst)});
d.close(SaveOptions.DONOTSAVECHANGES);
return "ok";
""")
    return dst


# --- 調整レイヤ -----------------------------------------------------------------

def adjustment_layer(code, cls):
    """code は調整の descriptor を変数 adj に作る JSX、cls はその種類の ID"""
    return f"""
var desc = new ActionDescriptor();
var ref = new ActionReference(); ref.putClass(sTID("adjustmentLayer"));
desc.putReference(cTID("null"), ref);
var ld = new ActionDescriptor();
{code}
ld.putObject(cTID("Type"), {cls}, adj);
desc.putObject(cTID("Usng"), sTID("adjustmentLayer"), ld);
executeAction(cTID("Mk  "), desc, DialogModes.NO);
"""


def hue_sat(hue=0, sat=0, light=0, colorize=False, ranges=()):
    """ranges: [(範囲 1..6, (境界 4 つ), 色相, 彩度, 明度)]"""
    lines = ["var adj = new ActionDescriptor();",
             'adj.putEnumerated(sTID("presetKind"), sTID("presetKindType"), sTID("presetKindCustom"));',
             f'adj.putBoolean(cTID("Clrz"), {"true" if colorize else "false"});',
             "var list = new ActionList(); var m = new ActionDescriptor();",
             f'm.putInteger(cTID("H   "), {hue}); m.putInteger(cTID("Strt"), {sat}); m.putInteger(cTID("Lght"), {light});',
             'list.putObject(cTID("Hst2"), m);']
    for idx, (b1, b2, e1, e2), h, s, l in ranges:
        lines += ["var r = new ActionDescriptor();",
                  f'r.putInteger(cTID("LclR"), {idx}); r.putInteger(cTID("BgnR"), {b1}); r.putInteger(cTID("BgnS"), {b2});',
                  f'r.putInteger(cTID("EndS"), {e1}); r.putInteger(cTID("EndR"), {e2});',
                  f'r.putInteger(cTID("H   "), {h}); r.putInteger(cTID("Strt"), {s}); r.putInteger(cTID("Lght"), {l});',
                  'list.putObject(cTID("Hst2"), r);']
    lines.append('adj.putList(cTID("Adjs"), list);')
    return adjustment_layer("\n".join(lines), 'cTID("HStr")')


def levels(master=(0, 255, 0, 255, 1.0), red=None, green=None, blue=None):
    """各 (入力黒, 入力白, 出力黒, 出力白, ガンマ)"""
    lines = ["var adj = new ActionDescriptor();",
             'adj.putEnumerated(sTID("presetKind"), sTID("presetKindType"), sTID("presetKindCustom"));',
             "var list = new ActionList();"]
    for ch, v in (("Cmps", master), ("Rd  ", red), ("Grn ", green), ("Bl  ", blue)):
        if v is None:
            continue
        ib, iw, ob, ow, g = v
        lines += ["var c = new ActionDescriptor(); var r = new ActionReference();",
                  f'r.putEnumerated(cTID("Chnl"), cTID("Chnl"), cTID("{ch}")); c.putReference(cTID("Chnl"), r);',
                  f'var li = new ActionList(); li.putInteger({ib}); li.putInteger({iw}); c.putList(cTID("Inpt"), li);',
                  f'var lo = new ActionList(); lo.putInteger({ob}); lo.putInteger({ow}); c.putList(cTID("Otpt"), lo);',
                  f'c.putDouble(cTID("Gmm "), {g});',
                  'list.putObject(cTID("LvlA"), c);']
    lines.append('adj.putList(cTID("Adjs"), list);')
    return adjustment_layer("\n".join(lines), 'cTID("Lvls")')


def brightness_contrast(b, c, legacy=False):
    return adjustment_layer(f"""var adj = new ActionDescriptor();
adj.putInteger(cTID("Brgh"), {b}); adj.putInteger(cTID("Cntr"), {c});
adj.putBoolean(sTID("useLegacy"), {"true" if legacy else "false"});""", 'cTID("BrgC")')


def color_balance(shadows, midtones, highlights, preserve):
    def lst(name, v):
        return (f'var l_{name} = new ActionList(); l_{name}.putInteger({v[0]}); l_{name}.putInteger({v[1]}); '
                f'l_{name}.putInteger({v[2]}); adj.putList(cTID("{name}"), l_{name});')
    body = "\n".join(["var adj = new ActionDescriptor();", lst("ShdL", shadows), lst("MdtL", midtones),
                      lst("HghL", highlights),
                      'adj.putBoolean(cTID("PrsL"), %s);' % ("true" if preserve else "false")])
    return adjustment_layer(body, 'cTID("ClrB")')


def photo_filter(rgb, density, preserve):
    r, g, b = rgb
    return adjustment_layer(f"""var adj = new ActionDescriptor();
var c = new ActionDescriptor(); c.putDouble(cTID("Rd  "), {r}); c.putDouble(cTID("Grn "), {g}); c.putDouble(cTID("Bl  "), {b});
adj.putObject(cTID("Clr "), cTID("RGBC"), c);
adj.putInteger(cTID("Dnst"), {density});
adj.putBoolean(cTID("PrsL"), {"true" if preserve else "false"});""", 'sTID("photoFilter")')


def masked_black(feather):
    """白の上の黒いレイヤに、横 64..128 を見せるマスク (ぼかし feather) を付ける"""
    return f"""
var bl = d.artLayers.add(); bl.name = "black";
app.foregroundColor.rgb.red = 0; app.foregroundColor.rgb.green = 0; app.foregroundColor.rgb.blue = 0;
d.selection.selectAll(); d.selection.fill(app.foregroundColor); d.selection.deselect();
d.selection.select([[64,0],[128,0],[128,64],[64,64]]);
var m = new ActionDescriptor(); m.putClass(cTID("Nw  "), cTID("Chnl"));
var rr = new ActionReference(); rr.putEnumerated(cTID("Chnl"), cTID("Chnl"), cTID("Msk ")); m.putReference(cTID("At  "), rr);
m.putEnumerated(cTID("Usng"), cTID("UsrM"), cTID("RvlS"));
executeAction(cTID("Mk  "), m, DialogModes.NO);
d.selection.deselect();
var s = new ActionDescriptor(); var r = new ActionReference();
r.putEnumerated(cTID("Lyr "), cTID("Ordn"), cTID("Trgt")); s.putReference(cTID("null"), r);
var l = new ActionDescriptor(); l.putUnitDouble(sTID("userMaskFeather"), cTID("#Pxl"), {feather});
s.putObject(cTID("T   "), cTID("Lyr "), l);
executeAction(cTID("setd"), s, DialogModes.NO);
"""


def white_png(folder):
    path = os.path.join(folder, "_white.png")
    if not os.path.exists(path):
        Image.fromarray(np.full((64, 192, 3), 255, np.uint8), "RGB").save(path)
    return path


# ケース名 -> (元画像, JSX, 許す最大誤差 (0..255)、平均誤差の上限)
def cases():
    return {
        "hs_master_light": ("cube", hue_sat(light=50), 3, 1.0),
        "hs_master_sat_up": ("cube", hue_sat(sat=60), 4, 1.0),
        "hs_master_sat_down": ("cube", hue_sat(sat=-60), 2, 1.0),
        "hs_master_hue": ("cube", hue_sat(hue=90), 3, 1.0),
        "hs_colorize": ("cube", hue_sat(hue=200, sat=40, light=10, colorize=True), 4, 1.0),
        "hs_reds_light": ("cube", hue_sat(ranges=[(1, (315, 345, 15, 45), 0, 0, 100)]), 10, 1.0),
        "hs_reds_hue": ("cube", hue_sat(ranges=[(1, (315, 345, 15, 45), 40, 0, 0)]), 8, 1.0),
        "lv_input": ("ramp", levels(master=(40, 210, 0, 255, 1.0)), 1, 0.5),
        "lv_gamma": ("ramp", levels(master=(0, 255, 0, 255, 2.0)), 3, 0.5),
        "lv_channels": ("ramp", levels(master=(20, 230, 10, 245, 1.4), red=(30, 255, 0, 255, 0.7),
                                       blue=(0, 200, 40, 255, 1.0)), 3, 0.5),
        "bc_legacy_up": ("ramp", brightness_contrast(30, 40, True), 2, 0.5),
        "bc_legacy_down": ("ramp", brightness_contrast(-40, -30, True), 2, 0.5),
        "bc_modern": ("ramp", brightness_contrast(40, 30), 2, 0.5),
        "bc_modern_strong": ("ramp", brightness_contrast(100, 80), 2, 0.5),
        "cb_mixed": ("cube", color_balance((30, -20, 40), (-40, 25, 10), (20, 50, -60), False), 3, 0.5),
        "cb_mixed_preserve": ("cube", color_balance((30, -20, 40), (-40, 25, 10), (20, 50, -60), True), 4, 0.5),
        "pf_warm": ("cube", photo_filter((236, 138, 0), 25, False), 2, 0.5),
        "pf_blue_preserve": ("cube", photo_filter((0, 90, 255), 60, True), 8, 0.5),
        "mask_feather_2": ("white", masked_black(2), 4, 0.5),
        "mask_feather_15": ("white", masked_black(15), 3, 0.5),
    }


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    out = argv[0]
    os.makedirs(out, exist_ok=True)
    bases = {"cube": cube_png(out), "ramp": ramp_png(out), "white": white_png(out)}
    limits = {}
    for name, (base, js, mx, mean) in cases().items():
        limits[name] = {"max": mx, "mean": mean}
        if len(argv) > 1 and not any(k in name for k in argv[1:]):
            continue
        print(name, flush=True)
        make(os.path.join(out, name + ".psd"), bases[base], js)
    with open(os.path.join(out, "limits.json"), "w", encoding="utf-8") as f:
        json.dump(limits, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
