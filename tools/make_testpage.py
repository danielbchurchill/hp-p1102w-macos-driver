#!/usr/bin/env python3
"""Write a one-page driver test PDF (stdlib only).

usage: make_testpage.py OUT.pdf [letter|a4]
"""
import sys

SIZES = {"letter": (612, 792), "a4": (595, 842)}


def page_ops(w, h):
    ops = []
    add = ops.append
    # Border 0.25in inside the edge (should be fully visible).
    add("0.5 w 18 18 %d %d re S" % (w - 36, h - 36))

    def text(x, y, size, s, font="F1"):
        s = s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
        add("BT /%s %g Tf %g %g Td (%s) Tj ET" % (font, size, x, y, s))

    top = h - 60
    text(54, top, 20, "HP LaserJet P1102w - macOS native driver", "F2")
    text(54, top - 22, 10, "Pipeline: cgpdftoraster -> rastertozjs -> foo2zjs (no Ghostscript)")

    # Text sizes.
    y = top - 56
    for sz in (6, 8, 10, 12, 16, 24):
        s = "The quick brown fox jumps over the lazy dog" + (" 0123456789" if sz <= 12 else "")
        text(54, y, sz, "%dpt  %s" % (sz, s) if sz < 24 else "24pt  Quick brown fox")
        y -= sz + 8

    # Gray ramp 0..100% in 10% steps.
    y -= 60
    text(54, y + 40, 10, "Gray ramp 0% - 100%", "F2")
    bw = (w - 108) / 11.0
    for i in range(11):
        g = 1.0 - i / 10.0
        add("%g g %g %g %g 30 re f" % (g, 54 + i * bw, y, bw))
        add("0 g")
        text(54 + i * bw + 4, y - 12, 7, "%d%%" % (i * 10))

    # Hairlines.
    y -= 50
    text(54, y, 10, "Lines: 0.25pt  0.5pt  1pt  2pt", "F2")
    y -= 14
    for i, lw in enumerate((0.25, 0.5, 1, 2)):
        add("%g w 54 %g m %g %g l S" % (lw, y - i * 10, w - 54, y - i * 10))
    for i, lw in enumerate((0.25, 0.5, 1, 2)):
        x = 54 + i * 14
        add("%g w %g %g m %g %g l S" % (lw, x, y - 50, x, y - 120))

    # Aspect-ratio check: exact 2in x 2in square (144pt).
    sq_y = 80
    add("1 w %g %d 144 144 re S" % (w - 54 - 144, sq_y))
    add("%g %d m %g %d l S" % (w - 54 - 144, sq_y, w - 54, sq_y + 144))
    text(w - 54 - 144, sq_y + 152, 9, "Must measure 2.00 x 2.00 in", "F2")

    # Corner marks at the 12pt imageable edge.
    add("1 w")
    for cx, cy, dx, dy in ((12, 12, 1, 1), (w - 12, 12, -1, 1), (12, h - 12, 1, -1), (w - 12, h - 12, -1, -1)):
        add("%g %g m %g %g l %g %g l S" % (cx + 30 * dx, cy, cx, cy, cx, cy + 30 * dy))
    text(54, 40, 8, "Corner marks sit on the 12pt printable edge; they may be partly clipped by the engine.")
    return "\n".join(ops).encode("latin-1")


def build(w, h):
    content = page_ops(w, h)
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] /Contents 4 0 R "
         "/Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> >>" % (w, h)).encode(),
        b"<< /Length %d >>\nstream\n" % len(content) + content + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>",
    ]
    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for i, o in enumerate(objs, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for off in offsets:
        out += b"%010d 00000 n \n" % off
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
    return bytes(out)


if __name__ == "__main__":
    size = sys.argv[2].lower() if len(sys.argv) > 2 else "letter"
    w, h = SIZES[size]
    with open(sys.argv[1], "wb") as f:
        f.write(build(w, h))
