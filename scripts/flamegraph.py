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
from dataclasses import dataclass

# SVG layout constants (pixels).
_SIDE = 10       # left/right margin
_PAD_TOP = 54    # space above the frames for title/subtitle
_PAD_BOTTOM = 16 # space below the frames for the detail line

# perf-script stack frame line: leading whitespace, hex address, symbol, "(dso)".
# C++ symbols contain spaces and parentheses, so the dso is taken as the final
# parenthesized group and the symbol is everything between the address and it.
_FRAME_RE = re.compile(r"^\s+(?P<addr>[0-9a-fA-F]+)\s+(?P<rest>.*\S)\s*$")
_OFFSET_RE = re.compile(r"\+0x[0-9a-fA-F]+$")
# Trailing " (dso)" group. perf prints a space before the dso, and dso strings
# (paths or "[unknown]") never contain parens, so a non-nested match is exact and
# avoids stripping a C++ signature's own "(...)" (which has no preceding space).
_DSO_RE = re.compile(r"\s+\([^()]*\)$")


def _clean_symbol(rest: str) -> str:
    """Turn a perf-script frame body into a folded frame name.

    Drops the trailing `(dso)` and the `+0xoffset`, matching stackcollapse-perf.
    """
    rest = _DSO_RE.sub("", rest)
    rest = _OFFSET_RE.sub("", rest).strip()
    return rest if rest else "[unknown]"


class _Folder:
    """Accumulates `perf script` samples into collapsed {stack: count} pairs.

    Keeping the per-line state transitions as small methods keeps the parsing
    loop flat (one if/elif/else) instead of a deeply nested block.
    """

    def __init__(self, drop_unknown: bool = True) -> None:
        self.folded: dict[str, int] = {}
        self.dropped_unknown = 0  # count of unresolved frames folded into their caller
        self._drop_unknown = drop_unknown
        self._comm = ""
        self._stack: list[str] = []

    def start_sample(self, header: str) -> None:
        # Header line: "comm  pid  timestamp:  period event:". Finalize any prior
        # sample (perf usually separates with a blank line, but not always).
        self._flush()
        self._comm = header.split()[0]

    def add_frame(self, line: str) -> None:
        m = _FRAME_RE.match(line)
        if m:
            self._stack.append(_clean_symbol(m.group("rest")))

    def end_sample(self) -> None:
        self._flush()
        self._comm = ""

    def _flush(self) -> None:
        if self._stack:
            frames = list(reversed(self._stack))  # perf prints leaf-first
            if self._drop_unknown:
                # Frame-pointer unwinding emits a single unresolvable "[unknown]" frame at the
                # glibc allocator boundary (Fedora's libc is built without frame pointers). Fold it
                # into its caller: the sample is preserved and the real neighbours (the app frame and
                # the named libc symbol such as cfree/operator new) stay in the stack.
                kept = [f for f in frames if f != "[unknown]"]
                self.dropped_unknown += len(frames) - len(kept)
                frames = kept
            if self._comm:
                frames.insert(0, self._comm)
            if frames:
                key = ";".join(frames)
                self.folded[key] = self.folded.get(key, 0) + 1
        self._stack = []

    def result(self) -> dict[str, int]:
        self._flush()
        return self.folded


def fold_perf_script(lines, drop_unknown: bool = True) -> dict[str, int]:
    """Collapse `perf script` output into {stack_string: sample_count}."""
    folder = _Folder(drop_unknown=drop_unknown)
    for raw in lines:
        line = raw.rstrip("\n")
        if not line.strip():
            folder.end_sample()
        elif line[0].isspace():
            folder.add_frame(line)
        else:
            folder.start_sample(line)
    result = folder.result()
    if folder.dropped_unknown:
        print(
            f"flamegraph.py: folded {folder.dropped_unknown} unresolved [unknown] frame(s) "
            "into their caller",
            file=sys.stderr,
        )
    return result


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


def _layout(node: _Node, depth: int, x: int, out: list) -> None:
    """Pre-order walk assigning each node a (depth, x-offset-in-samples)."""
    out.append((node, depth, x))
    cursor = x
    for name in sorted(node.children):
        child = node.children[name]
        _layout(child, depth + 1, cursor, out)
        cursor += child.value


@dataclass
class FlameOptions:
    """Styling/labelling knobs for an SVG render."""

    title: str = "QSL Flame Graph"
    subtitle: str = ""
    countname: str = "samples"
    width: int = 1200
    frame_height: int = 16
    min_px: float = 0.1


@dataclass
class _Canvas:
    """Derived geometry passed to per-frame rendering."""

    total: int
    max_depth: int
    height: int
    plot_width: int
    frame_height: int
    min_px: float
    countname: str


def _append_chrome(parts: list, opts: FlameOptions, height: int) -> None:
    """Append the static page furniture: SVG root, style, title, controls."""
    width = opts.width
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
        f'font-size="17" font-weight="bold">{html.escape(opts.title)}</text>'
    )
    parts.append(
        f'<text x="{width // 2}" y="40" text-anchor="middle" fill="#555">'
        f'{html.escape(opts.subtitle)}</text>'
    )
    parts.append(
        f'<text id="qsl-search" x="{width - _SIDE}" y="24" text-anchor="end" '
        f'fill="#990000" onclick="qslSearch()" style="cursor:pointer">Search</text>'
    )
    parts.append(
        f'<text id="qsl-detail" x="{_SIDE}" y="{height - 4}" fill="#333"> </text>'
    )


def _truncate(label: str, width_px: float) -> str:
    """Fit a label into a frame, ~7px/char with 6px padding (else nothing)."""
    maxchars = int((width_px - 6) / 7)
    if maxchars < 3:
        return ""
    return label if len(label) <= maxchars else label[: maxchars - 2] + ".."


def _frame_svg(c: _Canvas, node: _Node, depth: int, x: int) -> str:
    """Render one frame's <g> group, or "" when narrower than the cutoff."""
    w = node.value / c.total * c.plot_width
    if w < c.min_px:
        return ""
    x_px = _SIDE + x / c.total * c.plot_width
    y = _PAD_TOP + (c.max_depth - depth) * c.frame_height
    pct = node.value / c.total * 100.0
    tip = f"{node.name} ({node.value} {c.countname}, {pct:.2f}%)"
    out = [
        f'<g class="func" data-name="{html.escape(node.name)}">',
        f"<title>{html.escape(tip)}</title>",
        f'<rect class="frame" x="{x_px:.1f}" y="{y}" width="{w:.1f}" '
        f'height="{c.frame_height - 1}" fill="{_color(node.name)}" rx="2" ry="2"/>',
    ]
    text = _truncate(node.name, w)
    if text:
        out.append(
            f'<text x="{x_px + 3:.1f}" y="{y + c.frame_height - 4}" '
            f'fill="#000">{html.escape(text)}</text>'
        )
    out.append("</g>")
    return "".join(out)


def render_svg(root: _Node, opts: FlameOptions | None = None) -> str:
    opts = opts or FlameOptions()
    total = root.value or 1
    placed: list = []
    _layout(root, 0, 0, placed)
    max_depth = max((d for _, d, _ in placed), default=0)
    height = _PAD_TOP + (max_depth + 1) * opts.frame_height + _PAD_BOTTOM
    canvas = _Canvas(
        total=total,
        max_depth=max_depth,
        height=height,
        plot_width=opts.width - 2 * _SIDE,
        frame_height=opts.frame_height,
        min_px=opts.min_px,
        countname=opts.countname,
    )

    parts: list[str] = []
    _append_chrome(parts, opts, height)
    for node, depth, x in placed:
        parts.append(_frame_svg(canvas, node, depth, x))
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
    ap.add_argument("--keep-unknown", action="store_true",
                    help="keep unresolved [unknown] frames instead of folding them into the caller")
    args = ap.parse_args(argv)

    if args.from_collapsed:
        folded = parse_collapsed(sys.stdin)
    else:
        folded = fold_perf_script(sys.stdin, drop_unknown=not args.keep_unknown)

    if args.collapse_only:
        for stack in sorted(folded):
            sys.stdout.write(f"{stack} {folded[stack]}\n")
        return 0

    if not folded:
        sys.stderr.write("flamegraph.py: no stacks parsed from input\n")
        return 1

    root = build_tree(folded, args.root_name)
    opts = FlameOptions(
        title=args.title,
        subtitle=args.subtitle,
        countname=args.countname,
        width=args.width,
    )
    sys.stdout.write(render_svg(root, opts))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
