"""Generate character (tachie) variations from grouped layers.

Game/VTuber character PSDs put mutually-exclusive parts (eyes, mouth, outfit,
expression) into layer **folders** — each folder is a "slot" where one option is
shown at a time. This example enumerates option combinations and composites each
with Pillow (see composite.py).

    python variations.py character.psd out_dir/            # sweep the first slot
    python variations.py character.psd out_dir/ --group 表情 --group 目

As a library:

    from variations import option_groups, composite_choices
    groups = option_groups(psd)                 # {folder_index: [option_index, ...]}
    img = composite_choices(psd, {folder_i: option_i})   # PIL.Image

Note: this treats each top-level folder as a simple "pick one of its direct
pixel-layer children" slot. Real PSDs sometimes nest (a body folder with an
always-on base plus arm options); adapt `option_groups` for those.
"""
from __future__ import annotations

import argparse
import itertools
import os

import psdparse

from composite import composite, _PIXEL_TYPES


def _label(psd: psdparse.PSDFile, index: int) -> str:
    try:
        return psd.layers[index].name_unicode
    except UnicodeDecodeError:
        return f"layer{index}"


def option_groups(psd: psdparse.PSDFile) -> dict[int, list[int]]:
    """Top-level folders → their direct pixel-layer children (the options)."""
    groups: dict[int, list[int]] = {}
    for i, layer in enumerate(psd.layers):
        if layer.layer_type == psdparse.LayerType.FOLDER and layer.parent_index == -1:
            options = [j for j, c in enumerate(psd.layers)
                       if c.parent_index == i and c.layer_type in _PIXEL_TYPES]
            if options:
                groups[i] = options
    return groups


def composite_choices(psd: psdparse.PSDFile, choices: dict[int, int]):
    """Composite with one option chosen per group.

    `choices`: {folder_index: option_index}. For each chosen group the picked
    option is shown and its siblings hidden; groups not listed keep their
    currently-visible option; layers outside any group follow their visibility.
    """
    show = {i for i, l in enumerate(psd.layers)
            if l.visible and l.layer_type in _PIXEL_TYPES}
    groups = option_groups(psd)
    for folder_index, chosen in choices.items():
        for opt in groups.get(folder_index, []):
            show.discard(opt)
        show.add(chosen)
    return composite(psd, show)


def find_group(psd: psdparse.PSDFile, name: str) -> int | None:
    for folder_index in option_groups(psd):
        if _label(psd, folder_index) == name:
            return folder_index
    return None


def composite_comp(psd: psdparse.PSDFile, comp_id: int):
    """Composite a document **layer comp** (`comp_id` from psd.layer_comps).

    Each layer's `comp_states[comp_id].enabled` says whether it shows in that
    comp; layers the comp doesn't mention keep their current visibility. (Comp
    position/appearance overrides are not applied — visibility only.)
    """
    show = set()
    for i, layer in enumerate(psd.layers):
        if layer.layer_type not in _PIXEL_TYPES:
            continue
        st = layer.comp_states
        visible = st[comp_id]["enabled"] if comp_id in st else layer.visible
        if visible:
            show.add(i)
    return composite(psd, show)


def main() -> None:
    ap = argparse.ArgumentParser(description="Composite tachie layer-group combinations.")
    ap.add_argument("input")
    ap.add_argument("out_dir")
    ap.add_argument("--group", action="append", default=[],
                    help="folder name to sweep (repeatable). Default: the first group.")
    ap.add_argument("--comps", action="store_true",
                    help="instead of sweeping groups, render each document layer comp")
    ap.add_argument("--limit", type=int, default=24, help="max images to write")
    args = ap.parse_args()

    psd = psdparse.PSDFile()
    if not psd.load(args.input):
        raise SystemExit(f"failed to load {args.input}")

    if args.comps:
        comps = psd.layer_comps
        if not comps:
            raise SystemExit("this PSD has no document layer comps")
        os.makedirs(args.out_dir, exist_ok=True)
        for n, c in enumerate(comps):
            img = composite_comp(psd, c["id"])
            safe = "".join(ch if ch.isalnum() else "_" for ch in c["name"]).strip("_")
            img.save(os.path.join(args.out_dir, f"comp{n:02d}_{safe or c['id']}.png"))
        print(f"wrote {len(comps)} layer comps to {args.out_dir}")
        return

    groups = option_groups(psd)
    if not groups:
        raise SystemExit("no option groups (top-level folders) found")

    print("option groups:")
    for folder_index, opts in groups.items():
        print(f"  {_label(psd, folder_index)!r}: {[_label(psd, o) for o in opts]}")

    # which groups to sweep
    if args.group:
        sweep = [find_group(psd, n) for n in args.group]
        if None in sweep:
            raise SystemExit(f"group not found among {[_label(psd, g) for g in groups]}")
    else:
        sweep = [next(iter(groups))]

    os.makedirs(args.out_dir, exist_ok=True)
    combos = itertools.product(*[groups[g] for g in sweep])
    n = 0
    for combo in combos:
        if n >= args.limit:
            print(f"(stopped at --limit {args.limit})")
            break
        choices = dict(zip(sweep, combo))
        img = composite_choices(psd, choices)
        tag = "_".join(_label(psd, o).split()[0] for o in combo) or f"v{n}"
        path = os.path.join(args.out_dir, f"{n:03d}_{tag}.png")
        img.save(path)
        n += 1
    print(f"wrote {n} images to {args.out_dir}")


if __name__ == "__main__":
    main()
