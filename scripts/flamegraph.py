#!/usr/bin/env python3
"""Self-contained flamegraph generator for QSL perf profiles.

Reads `perf script` output on stdin, folds it into collapsed stacks
(stackcollapse), and renders a deterministic SVG flamegraph on stdout.

This is intentionally dependency-free (Python standard library only) so the
profiling artifact is reproducible from the repository alone, without vendoring
Brendan Gregg's Perl FlameGraph toolkit. The data model is identical: a
"collapsed stack" is `root;...;leaf<TAB-or-space>count`, and the flamegraph is a
proportional, sorted, recursive layout of those stacks.

Modes:
  flamegraph.py            perf script (stdin) -> SVG (stdout)
  flamegraph.py --collapse-only   perf script (stdin) -> collapsed stacks (stdout)
  flamegraph.py --from-collapsed  collapsed stacks (stdin) -> SVG (stdout)

The rendering is deterministic: frames are sorted by name, and colors are a pure
function of the frame name (no RNG, no timestamps in the drawn body). The driver
script (scripts/flamegraph.sh) records run provenance separately so the SVG stays
reproducible for a given input.
"""

from __future__ import annotations

import argparse
import html
import re
import sys
import zlib

# perf-script stack frame line: leading whitespace, hex address, symbol, "(dso)".
# C++ symbols contain spaces and parentheses, so the dso is taken as the final
# parenthesized group and the symbol is everything between the address and it.
_FRAME_RE = re.compile(r"^\s+(?P<addr>[0-9a-fA-F]+)\s+(?P<rest>.*\S)\s*$")
_OFFSET_RE = re.compile(r"\+0x[0-9a-fA-F]+$")


def _clean_symbol(rest: str) -> str:
    """Turn a perf-script frame body into a folded frame name.

    Drops the trailing `(dso)` and the `+0xoffset`, matching stackcollapse-perf.
    """
    # Strip the final "(...)" dso group if present (balanced at end of line).
    if rest.endswith(")"):
        depth = 0
        for i in range(len(rest) - 1, -1, -1):
            if rest[i] == ")":
                depth += 1
            elif rest[i] == "(":
                depth -= 1
                if depth == 0:
                    rest = rest[:i].rstrip()
                    break
    rest = _OFFSET_RE.sub("", rest).strip()
    if not rest:
        return "[unknown]"
    return rest


def fold_perf_script(lines) -> dict[str, int]:
    """Collapse `perf script` output into {stack_string: sample_count}."""
    folded: dict[str, int] = {}
    comm = ""
    stack: list[str] = []

    def flush() -> None:
        nonlocal stack, comm
        if stack:
            frames = list(reversed(stack))
            if comm:
                frames.insert(0, comm)
            key = ";".join(frames)
            folded[key] = folded.get(key, 0) + 1
        stack = []

    for raw in lines:
        line = raw.rstrip("\n")
        if not line.strip():
            flush()
            comm = ""
            continue
        if line[0].isspace():
            m = _FRAME_RE.match(line)
            if m:
                stack.append(_clean_symbol(m.group("rest")))
            continue
        # Header line: "comm  pid  timestamp:  period event:" -> capture comm.
        flush()
        comm = line.split()[0]
    flush()
    return folded


def parse_collapsed(lines) -> dict[str, int]:
    """Parse pre-collapsed `stack<sep>count` lines.

    The canonical folded separator is a space, but a tab is tolerated. Tab is
    preferred when present so a stack containing spaces (C++ signatures) still
    splits on the trailing count rather than on an interior space. Non-positive
    counts are ignored.
    """
    folded: dict[str, int] = {}
    for raw in lines:
        line = raw.rstrip("\n")
        if not line.strip():
            continue
        sep = "\t" if "\t" in line else " "
        stack, found, count = line.rpartition(sep)
        if not found:
            continue
        try:
            n = int(count)
        except ValueError:
            continue
        if n <= 0:
            continue
        folded[stack] = folded.get(stack, 0) + n
    return folded


class _Node:
    __slots__ = ("name", "value", "children")

    def __init__(self, name: str) -> None:
        self.name = name
        self.value = 0
        self.children: dict[str, _Node] = {}


def build_tree(folded: dict[str, int], root_name: str) -> _Node:
    root = _Node(root_name)
    for stack, count in folded.items():
        root.value += count
        node = root
        for frame in stack.split(";"):
            if not frame:
                continue
            child = node.children.get(frame)
            if child is None:
                child = _Node(frame)
                node.children[frame] = child
            child.value += count
            node = child
    return root


def _color(name: str) -> str:
    """Deterministic warm 'hot' palette derived purely from the frame name."""
    h = zlib.crc32(name.encode("utf-8")) & 0xFFFFFFFF
    r = 205 + (h % 51)
    g = (h >> 8) % 231
    b = (h >> 16) % 56
    return f"rgb({r},{g},{b})"


def _layout(node: _Node, depth: int, x: int, total: int, out: list) -> None:
    out.append((node, depth, x))
    cursor = x
    for name in sorted(node.children):
        child = node.children[name]
        _layout(child, depth + 1, cursor, total, out)
        cursor += child.value


def render_svg(
    root: _Node,
    *,
    title: str,
    subtitle: str,
    width: int = 1200,
    frame_height: int = 16,
    min_px: float = 0.1,
    countname: str = "samples",
) -> str:
    total = root.value or 1
    placed: list = []
    _layout(root, 0, 0, total, placed)
    max_depth = max((d for _, d, _ in placed), default=0)

    pad_top = 54
    pad_bottom = 16
    side = 10
    plot_width = width - 2 * side
    height = pad_top + (max_depth + 1) * frame_height + pad_bottom

    def px(samples: int) -> float:
        return samples / total * plot_width

    parts: list[str] = []
    parts.append(
        f'<?xml version="1.0" encoding="UTF-8" standalone="no"?>\n'
        f'<svg version="1.1" width="{width}" height="{height}" '
        f'xmlns="http://www.w3.org/2000/svg" '
        f'xmlns:xlink="http://www.w3.org/1999/xlink" '
        f'viewBox="0 0 {width} {height}" font-family="Verdana,Helvetica,sans-serif" '
        f'font-size="12">'
    )
    parts.append(
        '<style>.frame:hover{stroke:#000;stroke-width:0.5}'
        ' .hl{stroke:#000;stroke-width:1}</style>'
    )
    parts.append(_SEARCH_JS)
    parts.append(f'<rect width="{width}" height="{height}" fill="#f8f8f8"/>')
    parts.append(
        f'<text x="{width // 2}" y="24" text-anchor="middle" '
        f'font-size="17" font-weight="bold">{html.escape(title)}</text>'
    )
    parts.append(
        f'<text x="{width // 2}" y="40" text-anchor="middle" fill="#555">'
        f'{html.escape(subtitle)}</text>'
    )
    parts.append(
        f'<text id="qsl-search" x="{width - side}" y="24" text-anchor="end" '
        f'fill="#990000" onclick="qslSearch()" style="cursor:pointer">Search</text>'
    )
    parts.append(
        f'<text id="qsl-detail" x="{side}" y="{height - 4}" fill="#333"> </text>'
    )

    for node, depth, x in placed:
        w = px(node.value)
        if w < min_px:
            continue
        x_px = side + px(x)
        y = pad_top + (max_depth - depth) * frame_height
        pct = node.value / total * 100.0
        label = node.name
        # Approx 7px per char at this font; reserve 6px padding.
        maxchars = int((w - 6) / 7)
        text = ""
        if maxchars >= 3:
            text = label if len(label) <= maxchars else label[: maxchars - 2] + ".."
        tip = f"{label} ({node.value} {countname}, {pct:.2f}%)"
        parts.append(f'<g class="func" data-name="{html.escape(label)}">')
        parts.append(f"<title>{html.escape(tip)}</title>")
        parts.append(
            f'<rect class="frame" x="{x_px:.1f}" y="{y}" width="{w:.1f}" '
            f'height="{frame_height - 1}" fill="{_color(node.name)}" rx="2" ry="2"/>'
        )
        if text:
            parts.append(
                f'<text x="{x_px + 3:.1f}" y="{y + frame_height - 4}" '
                f'fill="#000">{html.escape(text)}</text>'
            )
        parts.append("</g>")

    parts.append("</svg>\n")
    return "".join(parts)


# Minimal, self-contained search affordance (highlight matches, report % of
# matched samples). No external assets; deterministic; no zoom to keep the
# artifact robust across renderers.
_SEARCH_JS = (
    "<script type=\"text/ecmascript\"><![CDATA[\n"
    "function qslSearch(){\n"
    "  var term=prompt('Search frame (regex):','');\n"
    "  var detail=document.getElementById('qsl-detail');\n"
    "  var gs=document.getElementsByClassName('func');\n"
    "  var i;\n"
    "  if(!term){for(i=0;i<gs.length;i++){gs[i].getElementsByTagName('rect')[0]"
    ".classList.remove('hl');}if(detail)detail.textContent=' ';return;}\n"
    "  var re;try{re=new RegExp(term);}catch(e){return;}\n"
    "  for(i=0;i<gs.length;i++){var r=gs[i].getElementsByTagName('rect')[0];\n"
    "    if(re.test(gs[i].getAttribute('data-name'))){r.classList.add('hl');}\n"
    "    else{r.classList.remove('hl');}}\n"
    "  if(detail)detail.textContent='Search: '+term;\n"
    "}\n"
    "]]></script>"
)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--collapse-only", action="store_true",
                    help="emit collapsed stacks instead of SVG")
    ap.add_argument("--from-collapsed", action="store_true",
                    help="read collapsed stacks instead of perf script output")
    ap.add_argument("--title", default="QSL Flame Graph")
    ap.add_argument("--subtitle", default="")
    ap.add_argument("--countname", default="samples")
    ap.add_argument("--root-name", default="all")
    ap.add_argument("--width", type=int, default=1200)
    args = ap.parse_args(argv)

    if args.from_collapsed:
        folded = parse_collapsed(sys.stdin)
    else:
        folded = fold_perf_script(sys.stdin)

    if args.collapse_only:
        for stack in sorted(folded):
            sys.stdout.write(f"{stack} {folded[stack]}\n")
        return 0

    if not folded:
        sys.stderr.write("flamegraph.py: no stacks parsed from input\n")
        return 1

    root = build_tree(folded, args.root_name)
    sys.stdout.write(
        render_svg(
            root,
            title=args.title,
            subtitle=args.subtitle,
            width=args.width,
            countname=args.countname,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
