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



# --- レイヤー効果 / 描画モード ------------------------------------------------

def square_layer(x0, y0, x1, y1, rgb=(255, 255, 255)):
    r, g, b = rgb
    return f"""
var sq = d.artLayers.add(); sq.name = "shape";
app.foregroundColor.rgb.red = {r}; app.foregroundColor.rgb.green = {g}; app.foregroundColor.rgb.blue = {b};
d.selection.select([[{x0},{y0}],[{x1},{y0}],[{x1},{y1}],[{x0},{y1}]]);
d.selection.fill(app.foregroundColor); d.selection.deselect();
"""


def ellipse_layer(x0, y0, x1, y1, rgb=(255, 255, 255)):
    r, g, b = rgb
    return f"""
var sq = d.artLayers.add(); sq.name = "shape";
app.foregroundColor.rgb.red = {r}; app.foregroundColor.rgb.green = {g}; app.foregroundColor.rgb.blue = {b};
var sd = new ActionDescriptor(); var sr = new ActionReference(); sr.putProperty(cTID("Chnl"), cTID("fsel")); sd.putReference(cTID("null"), sr);
var el = new ActionDescriptor(); el.putUnitDouble(cTID("Top "), cTID("#Pxl"), {y0}); el.putUnitDouble(cTID("Left"), cTID("#Pxl"), {x0});
el.putUnitDouble(cTID("Btom"), cTID("#Pxl"), {y1}); el.putUnitDouble(cTID("Rght"), cTID("#Pxl"), {x1});
sd.putObject(cTID("T   "), cTID("Elps"), el); sd.putBoolean(cTID("AntA"), true);
executeAction(cTID("setd"), sd, DialogModes.NO);
d.selection.fill(app.foregroundColor); d.selection.deselect();
"""


def _color(var, rgb):
    r, g, b = rgb
    return f'var {var} = new ActionDescriptor(); {var}.putDouble(cTID("Rd  "), {r}); {var}.putDouble(cTID("Grn "), {g}); {var}.putDouble(cTID("Bl  "), {b});'


def effect(key, fields):
    """fields: JSX 文の列 (変数 e に追加)。key: OrGl / IrGl / DrSh / IrSh / ebbl / ChFX / FrFX"""
    body = "\n".join(fields)
    return f"""
var d0 = new ActionDescriptor(); var r0 = new ActionReference();
r0.putProperty(cTID("Prpr"), cTID("Lefx")); r0.putEnumerated(cTID("Lyr "), cTID("Ordn"), cTID("Trgt"));
d0.putReference(cTID("null"), r0);
var fx = new ActionDescriptor(); fx.putUnitDouble(cTID("Scl "), cTID("#Prc"), 100);
var e = new ActionDescriptor(); e.putBoolean(cTID("enab"), true);
{body}
fx.putObject(cTID("{key}"), cTID("{key}"), e);
d0.putObject(cTID("T   "), cTID("Lefx"), fx);
executeAction(cTID("setd"), d0, DialogModes.NO);
"""


def outer_glow(size, rng=50, spread=0, precise=False, rgb=(255, 0, 0), mode="Nrml", opacity=100):
    return effect("OrGl", [
        f'e.putEnumerated(cTID("Md  "), cTID("BlnM"), cTID("{mode}"));', _color("c", rgb), 'e.putObject(cTID("Clr "), cTID("RGBC"), c);',
        f'e.putUnitDouble(cTID("Opct"), cTID("#Prc"), {opacity});',
        f'e.putEnumerated(cTID("GlwT"), cTID("BETE"), cTID("{"PrBL" if precise else "SfBL"}"));',
        f'e.putUnitDouble(cTID("Ckmt"), cTID("#Pxl"), {spread}); e.putUnitDouble(cTID("blur"), cTID("#Pxl"), {size});',
        f'e.putUnitDouble(cTID("Inpr"), cTID("#Prc"), {rng}); e.putUnitDouble(cTID("ShdN"), cTID("#Prc"), 0);'])


def inner_glow(size, rng=50, spread=0, precise=False, rgb=(255, 0, 0), center=False):
    return effect("IrGl", [
        'e.putEnumerated(cTID("Md  "), cTID("BlnM"), cTID("Nrml"));', _color("c", rgb), 'e.putObject(cTID("Clr "), cTID("RGBC"), c);',
        'e.putUnitDouble(cTID("Opct"), cTID("#Prc"), 100);',
        f'e.putEnumerated(cTID("GlwT"), cTID("BETE"), cTID("{"PrBL" if precise else "SfBL"}"));',
        f'e.putUnitDouble(cTID("Ckmt"), cTID("#Pxl"), {spread}); e.putUnitDouble(cTID("blur"), cTID("#Pxl"), {size});',
        f'e.putUnitDouble(cTID("Inpr"), cTID("#Prc"), {rng}); e.putUnitDouble(cTID("ShdN"), cTID("#Prc"), 0);',
        f'e.putEnumerated(cTID("glwS"), cTID("IGSr"), cTID("{"SrcC" if center else "SrcE"}"));'])


def shadow(key, size, distance=0, spread=0, angle=90, rgb=(255, 0, 0)):
    return effect(key, [
        'e.putEnumerated(cTID("Md  "), cTID("BlnM"), cTID("Nrml"));', _color("c", rgb), 'e.putObject(cTID("Clr "), cTID("RGBC"), c);',
        'e.putUnitDouble(cTID("Opct"), cTID("#Prc"), 100); e.putBoolean(cTID("uglg"), false);',
        f'e.putUnitDouble(cTID("lagl"), cTID("#Ang"), {angle}); e.putUnitDouble(cTID("Dstn"), cTID("#Pxl"), {distance});',
        f'e.putUnitDouble(cTID("Ckmt"), cTID("#Pxl"), {spread}); e.putUnitDouble(cTID("blur"), cTID("#Pxl"), {size});',
        'e.putUnitDouble(cTID("Nose"), cTID("#Prc"), 0); e.putBoolean(cTID("AntA"), false);'])


def bevel(style="InrB", tech="SfBL", depth=100, up=True, size=10, soften=0, angle=120, altitude=30,
          hi=(255, 255, 255), sh=(0, 0, 0), hmode="Scrn", smode="Mltp", hop=75, sop=75):
    return effect("ebbl", [
        f'e.putEnumerated(cTID("hglM"), cTID("BlnM"), cTID("{hmode}"));', _color("hc", hi), 'e.putObject(cTID("hglC"), cTID("RGBC"), hc);',
        f'e.putUnitDouble(cTID("hglO"), cTID("#Prc"), {hop});',
        f'e.putEnumerated(cTID("sdwM"), cTID("BlnM"), cTID("{smode}"));', _color("sc", sh), 'e.putObject(cTID("sdwC"), cTID("RGBC"), sc);',
        f'e.putUnitDouble(cTID("sdwO"), cTID("#Prc"), {sop});',
        f'e.putEnumerated(cTID("bvlT"), cTID("bvlT"), cTID("{tech}"));',
        f'e.putEnumerated(cTID("bvlS"), cTID("BESl"), cTID("{style}"));',
        'e.putBoolean(cTID("uglg"), false);',
        f'e.putUnitDouble(cTID("lagl"), cTID("#Ang"), {angle}); e.putUnitDouble(cTID("Lald"), cTID("#Ang"), {altitude});',
        f'e.putUnitDouble(cTID("srgR"), cTID("#Prc"), {depth});',
        f'e.putUnitDouble(cTID("blur"), cTID("#Pxl"), {size});',
        f'e.putEnumerated(cTID("bvlD"), cTID("BESs"), cTID("{"In  " if up else "Out "}"));',
        'e.putBoolean(sTID("antialiasGloss"), false);',
        f'e.putUnitDouble(cTID("Sftn"), cTID("#Pxl"), {soften});',
        'e.putBoolean(sTID("useShape"), false); e.putBoolean(sTID("useTexture"), false);'])


def stroke_fx(size, pos="OutF", rgb=(255, 0, 0), opacity=100):
    return effect("FrFX", [
        f'e.putEnumerated(cTID("Styl"), cTID("FStl"), cTID("{pos}"));',
        'e.putEnumerated(cTID("PntT"), cTID("FrFl"), cTID("SClr"));',
        'e.putEnumerated(cTID("Md  "), cTID("BlnM"), cTID("Nrml"));',
        f'e.putUnitDouble(cTID("Opct"), cTID("#Prc"), {opacity});',
        f'e.putUnitDouble(cTID("Sz  "), cTID("#Pxl"), {size});',
        _color("c", rgb), 'e.putObject(cTID("Clr "), cTID("RGBC"), c);'])


# 輪郭 (効果の被覆率を写す曲線) の点。ガウス (Photoshop の既定の一覧にあるもの) とリング
GAUSS_CONTOUR = [(0, 0), (32, 7), (64, 38), (96, 101), (128, 166), (159, 209), (191, 235), (223, 248), (255, 255)]
RING_CONTOUR = [(0, 0), (128, 255), (255, 0)]


def with_contour(fx_js, points, key="TrnS"):
    lines = ['var cc = new ActionDescriptor(); cc.putString(cTID("Nm  "), "c"); var cl2 = new ActionList();']
    for h, v in points:
        lines.append(f'var cp = new ActionDescriptor(); cp.putDouble(cTID("Hrzn"), {h}); cp.putDouble(cTID("Vrtc"), {v}); '
                     'cl2.putObject(cTID("CrPt"), cp);')
    lines.append(f'cc.putList(cTID("Crv "), cl2); e.putObject(cTID("{key}"), cTID("ShpC"), cc);')
    return fx_js.replace('fx.putObject(', "\n".join(lines) + '\nfx.putObject(', 1)


def satin(size=14, distance=11, angle=19, invert=True, rgb=(255, 0, 0)):
    return effect("ChFX", [
        'e.putEnumerated(cTID("Md  "), cTID("BlnM"), cTID("Nrml"));', _color("c", rgb), 'e.putObject(cTID("Clr "), cTID("RGBC"), c);',
        'e.putUnitDouble(cTID("Opct"), cTID("#Prc"), 100);',
        f'e.putUnitDouble(cTID("lagl"), cTID("#Ang"), {angle}); e.putUnitDouble(cTID("Dstn"), cTID("#Pxl"), {distance});',
        f'e.putUnitDouble(cTID("blur"), cTID("#Pxl"), {size}); e.putBoolean(cTID("Invr"), {"true" if invert else "false"});',
        'e.putBoolean(cTID("AntA"), false);'])


def soft_square(x0, y0, x1, y1, feather, rgb=(255, 255, 255)):
    r, g, b = rgb
    return f"""
var sq = d.artLayers.add(); sq.name = "shape";
app.foregroundColor.rgb.red = {r}; app.foregroundColor.rgb.green = {g}; app.foregroundColor.rgb.blue = {b};
d.selection.select([[{x0},{y0}],[{x1},{y0}],[{x1},{y1}],[{x0},{y1}]]);
d.selection.feather({feather});
d.selection.fill(app.foregroundColor); d.selection.deselect();
"""


def grad_layer(style, angle=30, smooth=4096, scale=100, reverse=False, stops=((0, (255, 0, 0)), (4096, (0, 0, 255))), mids=50,
               method=None):
    lines = ['var d0 = new ActionDescriptor(); var r0 = new ActionReference(); r0.putClass(sTID("contentLayer")); d0.putReference(cTID("null"), r0);',
             'var ld = new ActionDescriptor(); var gl = new ActionDescriptor();',
             f'gl.putEnumerated(cTID("Type"), cTID("GrdT"), cTID("{style}")); gl.putUnitDouble(cTID("Angl"), cTID("#Ang"), {angle});',
             f'gl.putUnitDouble(cTID("Scl "), cTID("#Prc"), {scale}); gl.putBoolean(cTID("Rvrs"), {"true" if reverse else "false"});',
             'var g = new ActionDescriptor(); g.putString(cTID("Nm  "), "c"); g.putEnumerated(cTID("GrdF"), cTID("GrdF"), cTID("CstS"));',
             f'g.putDouble(cTID("Intr"), {smooth});', 'var cl = new ActionList();']
    for loc, (r, gg, b) in stops:
        lines += ['var s = new ActionDescriptor(); var c = new ActionDescriptor();',
                  f'c.putDouble(cTID("Rd  "), {r}); c.putDouble(cTID("Grn "), {gg}); c.putDouble(cTID("Bl  "), {b});',
                  's.putObject(cTID("Clr "), cTID("RGBC"), c); s.putEnumerated(cTID("Type"), cTID("Clry"), cTID("UsrS"));',
                  f's.putInteger(cTID("Lctn"), {loc}); s.putInteger(cTID("Mdpn"), {mids}); cl.putObject(cTID("Clrt"), s);']
    lines += ['g.putList(cTID("Clrs"), cl); var tl = new ActionList();',
              'var t = new ActionDescriptor(); t.putUnitDouble(cTID("Opct"), cTID("#Prc"), 100); t.putInteger(cTID("Lctn"), 0); t.putInteger(cTID("Mdpn"), 50); tl.putObject(cTID("TrnS"), t);',
              'var t2 = new ActionDescriptor(); t2.putUnitDouble(cTID("Opct"), cTID("#Prc"), 100); t2.putInteger(cTID("Lctn"), 4096); t2.putInteger(cTID("Mdpn"), 50); tl.putObject(cTID("TrnS"), t2);',
              'g.putList(cTID("Trns"), tl); gl.putObject(cTID("Grad"), cTID("Grdn"), g);',
              (f'gl.putEnumerated(sTID("gradientsInterpolationMethod"), sTID("gradientInterpolationMethodType"), cTID("{method}"));'
               if method else ''),
              'ld.putObject(cTID("Type"), sTID("gradientLayer"), gl); d0.putObject(cTID("Usng"), sTID("contentLayer"), ld);',
              'executeAction(cTID("Mk  "), d0, DialogModes.NO);']
    return "\n".join(lines)


def solid_layer(rgb, mode, opacity=100, fill=100):
    """全面を単色で塗ったレイヤ (描画モード / 不透明度 / 塗り)"""
    r, g, b = rgb
    return f"""
var sl = d.artLayers.add(); sl.name = "solid";
app.foregroundColor.rgb.red = {r}; app.foregroundColor.rgb.green = {g}; app.foregroundColor.rgb.blue = {b};
d.selection.selectAll(); d.selection.fill(app.foregroundColor); d.selection.deselect();
sl.blendMode = BlendMode.{mode}; sl.opacity = {opacity}; sl.fillOpacity = {fill};
"""


def black_png(folder):
    path = os.path.join(folder, "_black.png")
    if not os.path.exists(path):
        Image.fromarray(np.zeros((160, 160, 3), np.uint8), "RGB").save(path)
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
        "hs_master_with_range": ("cube", hue_sat(sat=-97, ranges=[(6, (82, 133, 151, 171), 0, 100, 0)]), 16, 1.0),
        "hs_overlapping_ranges": ("cube", hue_sat(ranges=[(1, (315, 345, 15, 45), 0, 60, 0),
                                                          (2, (0, 30, 60, 90), 0, -60, 0)]), 10, 1.0),
        "hs_sat100_dark": ("cube", hue_sat(hue=122, sat=100, light=-37), 8, 1.0),
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
        "mode_linear_burn_fill": ("cube", solid_layer((40, 120, 200), "LINEARBURN", 100, 50), 2, 0.5),
        "mode_hard_mix_fill": ("cube", solid_layer((40, 120, 200), "HARDMIX", 100, 50), 3, 0.5),
        "mode_hard_mix_opacity": ("cube", solid_layer((40, 120, 200), "HARDMIX", 50, 100), 2, 0.5),
        "mode_vivid_light_fill": ("cube", solid_layer((40, 120, 200), "VIVIDLIGHT", 100, 50), 2, 0.5),
        "grad_radial": ("white", grad_layer("Rdl "), 2, 0.5),
        "grad_diamond": ("white", grad_layer("Dmnd"), 6, 0.8),
        "grad_angle": ("white", grad_layer("Angl"), 255, 0.5),
        "grad_linear_smooth": ("white", grad_layer("Lnr ", angle=-60, stops=((0, (255, 0, 0)), (2048, (0, 255, 0)), (4096, (0, 0, 255))), mids=30), 3, 0.5),
        # 補間方法: 知覚的 (Oklab) / 滑らか、角度 0 (保存時に角度が省かれる)
        "grad_perceptual_gray": ("white", grad_layer("Lnr ", angle=0, stops=((0, (0, 0, 0)), (4096, (255, 255, 255))),
                                                     method="Perc"), 2, 0.5),
        "grad_perceptual_rgb": ("white", grad_layer("Lnr ", angle=0, stops=((0, (255, 0, 0)), (1400, (0, 255, 0)),
                                                                            (4096, (0, 0, 255))), method="Perc"), 16, 3.0),
        "grad_smooth_gray": ("white", grad_layer("Lnr ", angle=0, stops=((0, (0, 0, 0)), (2048, (128, 128, 128)),
                                                                         (4096, (255, 255, 255))), method="Smoo"), 2, 0.5),
        "fx_outer_glow_soft": ("black", square_layer(60, 60, 100, 100) + outer_glow(20), 12, 0.5),
        "fx_outer_glow_precise": ("black", square_layer(60, 60, 100, 100) + outer_glow(20, 25, precise=True), 15, 0.5),
        "fx_inner_glow": ("black", square_layer(60, 60, 100, 100, (0, 0, 255)) + inner_glow(10), 20, 0.5),
        "fx_inner_shadow": ("black", square_layer(60, 60, 100, 100, (0, 0, 255)) + shadow("IrSh", 10, distance=8), 12, 0.5),
        "fx_drop_shadow": ("black", square_layer(60, 60, 100, 100) + shadow("DrSh", 20, spread=50), 12, 0.5),
        "fx_bevel_inner": ("black", square_layer(50, 50, 110, 110, (128, 128, 128)) + bevel(), 18, 0.5),
        "fx_bevel_emboss": ("black", square_layer(50, 50, 110, 110, (128, 128, 128)) + bevel("Embs"), 25, 0.6),
        "fx_bevel_chisel": ("black", square_layer(50, 50, 110, 110, (128, 128, 128)) + bevel(tech="PrBL"), 36, 0.5),
        "fx_drop_shadow_gauss_contour": ("black", square_layer(50, 50, 110, 110, (0, 0, 255)) +
                                         with_contour(shadow("DrSh", 20), GAUSS_CONTOUR), 8, 0.5),
        "fx_outer_glow_ring_contour": ("black", square_layer(50, 50, 110, 110, (0, 0, 255)) +
                                       with_contour(outer_glow(30, rng=75), RING_CONTOUR), 16, 1.2),
        "fx_satin": ("black", square_layer(50, 50, 110, 110, (0, 0, 255)) + satin(), 6, 0.5),
        "fx_satin_gauss_contour": ("black", square_layer(50, 50, 110, 110, (0, 0, 255)) +
                                   with_contour(satin(size=30, distance=25, angle=45), GAUSS_CONTOUR, "MpgS"), 8, 0.5),
        "fx_stroke_ellipse_outside": ("black", ellipse_layer(40, 40, 120, 120, (0, 0, 255)) + stroke_fx(3, "OutF"), 20, 0.5),
        "fx_stroke_ellipse_inside": ("black", ellipse_layer(40, 40, 120, 120, (0, 0, 255)) + stroke_fx(3, "InsF"), 20, 0.5),
        "fx_stroke_soft_center": ("black", soft_square(50, 50, 110, 110, 6, (0, 0, 255)) + stroke_fx(10, "CtrF"), 32, 0.5),
        "fx_bevel_linear_burn": ("black", ellipse_layer(17, 20, 140, 143, (249, 237, 52)) +
                                 bevel(size=60, hi=(251, 250, 137), sh=(244, 210, 21), hmode="Scrn",
                                       smode="linearBurn", hop=0, sop=70).replace('cTID("linearBurn")', 'sTID("linearBurn")'),
                                 15, 0.5),
    }


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    out = argv[0]
    os.makedirs(out, exist_ok=True)
    bases = {"cube": cube_png(out), "ramp": ramp_png(out), "white": white_png(out), "black": black_png(out)}
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
