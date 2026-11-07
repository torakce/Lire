#!/usr/bin/env python3
"""Génère des PDF de test lourds imitant des plans mécaniques (traits, cercles, hachures, cotes)."""
import random
import sys
import zlib


def drawing(w, h, nseg, ntext, seed):
    r = random.Random(seed)
    out = []
    a = out.append
    a("1 J 1 j 0.25 w 0 G")
    # cadre et cartouche
    m = 20
    a(f"2 w {m} {m} {w-2*m} {h-2*m} re S 0.25 w")
    for i in range(1, 12):
        x = m + i * (w - 2 * m) / 12
        a(f"{x:.1f} {m} m {x:.1f} {m+10} l {x:.1f} {h-m} m {x:.1f} {h-m-10} l S")
    a(f"1 w {w-m-700} {m} 700 220 re S 0.25 w")
    segs = 0
    while segs < nseg:
        kind = r.random()
        cx, cy = r.uniform(m + 50, w - m - 50), r.uniform(m + 50, h - m - 50)
        if kind < 0.35:  # polyligne
            n = r.randint(5, 40)
            pts = [f"{cx:.2f} {cy:.2f} m"]
            for _ in range(n):
                cx = min(max(cx + r.uniform(-60, 60), m), w - m)
                cy = min(max(cy + r.uniform(-60, 60), m), h - m)
                pts.append(f"{cx:.2f} {cy:.2f} l")
            a(" ".join(pts) + " S")
            segs += n
        elif kind < 0.6:  # cercle (4 béziers) + axes
            rad = r.uniform(2, 120)
            k = rad * 0.5523
            a(f"{cx+rad:.2f} {cy:.2f} m {cx+rad:.2f} {cy+k:.2f} {cx+k:.2f} {cy+rad:.2f} {cx:.2f} {cy+rad:.2f} c "
              f"{cx-k:.2f} {cy+rad:.2f} {cx-rad:.2f} {cy+k:.2f} {cx-rad:.2f} {cy:.2f} c "
              f"{cx-rad:.2f} {cy-k:.2f} {cx-k:.2f} {cy-rad:.2f} {cx:.2f} {cy-rad:.2f} c "
              f"{cx+k:.2f} {cy-rad:.2f} {cx+rad:.2f} {cy-k:.2f} {cx+rad:.2f} {cy:.2f} c S")
            a(f"[6 2 1 2] 0 d {cx-rad-8:.2f} {cy:.2f} m {cx+rad+8:.2f} {cy:.2f} l {cx:.2f} {cy-rad-8:.2f} m {cx:.2f} {cy+rad+8:.2f} l S [] 0 d")
            segs += 6
        elif kind < 0.8:  # zone hachurée
            bw, bh = r.uniform(30, 200), r.uniform(30, 200)
            a(f"q {cx:.2f} {cy:.2f} {bw:.2f} {bh:.2f} re W n")
            step = r.uniform(2.0, 5.0)
            t = -bh
            lines = []
            while t < bw:
                lines.append(f"{cx+t:.2f} {cy:.2f} m {cx+t+bh:.2f} {cy+bh:.2f} l")
                t += step
                segs += 1
            a(" ".join(lines) + " S Q")
            a(f"0.5 w {cx:.2f} {cy:.2f} {bw:.2f} {bh:.2f} re S 0.25 w")
        else:  # cote avec texte
            L = r.uniform(20, 300)
            a(f"{cx:.2f} {cy:.2f} m {cx+L:.2f} {cy:.2f} l S")
            a(f"{cx:.2f} {cy:.2f} m {cx+6:.2f} {cy+2:.2f} l {cx+6:.2f} {cy-2:.2f} l f")
            a(f"{cx+L:.2f} {cy:.2f} m {cx+L-6:.2f} {cy+2:.2f} l {cx+L-6:.2f} {cy-2:.2f} l f")
            segs += 3
    for _ in range(ntext):
        x, y = r.uniform(m + 20, w - m - 100), r.uniform(m + 20, h - m - 20)
        s = r.choice(["Ø%.1f H7" % r.uniform(2, 80), "R%.1f" % r.uniform(1, 40), "%.2f ±0.05" % r.uniform(5, 500),
                      "A-A", "DÉTAIL B (2:1)", "Ra 1.6", "M%d x %.2f" % (r.randint(3, 24), r.choice([0.5, 0.75, 1.0, 1.25, 1.5]))])
        size = r.choice([3.5, 5, 7, 10])
        s = s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
        s = s.encode("latin-1", "replace")
        a(f"BT /F1 {size} Tf {x:.2f} {y:.2f} Td (".encode().decode() + s.decode("latin-1") + ") Tj ET")
    a(f"BT /F1 28 Tf {w-m-680} {m+150} Td (PLAN DE TEST - Lire) Tj ET")
    a(f"BT /F1 14 Tf {w-m-680} {m+110} Td (Format {w:.0f} x {h:.0f} pt, {nseg} segments, {ntext} textes) Tj ET")
    return "\n".join(out).encode("latin-1")


def text_page(w, h, seed, n):
    r = random.Random(seed)
    words = "lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore et dolore magna aliqua pièce tolérance cotation usinage".split()
    out = [f"BT /F1 20 Tf 60 {h-80} Td (Page {n}) Tj ET", "BT /F1 10 Tf 60 %d Td 13 TL" % (h - 120)]
    for _ in range(int((h - 180) / 13)):
        line = " ".join(r.choice(words) for _ in range(14))
        out.append("(" + line + ") '")
    out.append("ET")
    return "\n".join(out).encode("latin-1")


def write_pdf(path, pages):
    """pages : liste de (largeur, hauteur, contenu_bytes)"""
    objs = []

    def add(b):
        objs.append(b)
        return len(objs)

    font = add(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")
    pages_id = len(objs) + 1 + 0  # réservé
    objs.append(None)
    kids = []
    for (w, h, content) in pages:
        comp = zlib.compress(content, 6)
        cid = add(b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(comp) + comp + b"\nendstream")
        pid = add(b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %.2f %.2f] /Resources << /Font << /F1 %d 0 R >> >> /Contents %d 0 R >>"
                  % (pages_id, w, h, font, cid))
        kids.append(pid)
    objs[pages_id - 1] = b"<< /Type /Pages /Kids [" + b" ".join(b"%d 0 R" % k for k in kids) + b"] /Count %d >>" % len(kids)
    cat = add(b"<< /Type /Catalog /Pages %d 0 R >>" % pages_id)
    with open(path, "wb") as f:
        f.write(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
        offs = []
        for i, o in enumerate(objs):
            offs.append(f.tell())
            f.write(b"%d 0 obj\n" % (i + 1) + o + b"\nendobj\n")
        x = f.tell()
        f.write(b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1))
        for o in offs:
            f.write(b"%010d 00000 n \n" % o)
        f.write(b"trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, cat, x))


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    A0 = (3370.4, 2383.9)  # A0 paysage
    # 1) plan géant : 4 x A0 (4A0 paysage, 2378 x 1682 mm) très chargé
    write_pdf(f"{out}/plan_4A0_lourd.pdf", [(6741, 4768, drawing(6741, 4768, 1_200_000, 40_000, 1))])
    # 2) plan très allongé : 6 x A0 en longueur (A0 x 6)
    write_pdf(f"{out}/plan_6xA0_long.pdf", [(3370.4 * 6, 2383.9, drawing(3370.4 * 6, 2383.9, 600_000, 20_000, 2))])
    # 3) liasse : 10 plans A0 + nomenclature A4
    pages = [(595, 842, text_page(595, 842, 99, 1))]
    for i in range(10):
        pages.append((A0[0], A0[1], drawing(A0[0], A0[1], 150_000, 6_000, 10 + i)))
    write_pdf(f"{out}/liasse_10xA0.pdf", pages)
    # 4) document texte de 400 pages A4
    write_pdf(f"{out}/doc_400p.pdf", [(595, 842, text_page(595, 842, i, i + 1)) for i in range(400)])
    print("ok")
