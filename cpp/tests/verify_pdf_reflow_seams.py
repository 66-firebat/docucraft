import argparse
import json
import math
import re
from pathlib import Path

import pymupdf as fitz


def verify(source_path, output_path, log_path):
    source = fitz.open(source_path)
    output = fitz.open(output_path)
    log = Path(log_path).read_text(encoding="utf-8")
    layout = re.search(r"target=(\d+)x(\d+) margins=(\d+),(\d+),(\d+),(\d+)", log)
    assert layout, "Missing layout diagnostics"
    width, height, left, right, top, bottom = map(int, layout.groups())
    expected = {int(m[1]): int(m[2]) for m in re.finditer(
        r"page=(\d+) sourcePt=.*? strip=\d+x(\d+)", log)}
    pattern = r"slice source=(\d+) output=(\d+) offset=(\d+) take=(\d+) used=(\d+) scale=([\d.]+) crop=([\d.,-]+)"
    endpoints, prior_pages, output_ends = {}, {}, {}
    seams = []
    errors = []
    released_pages = set()
    for m in re.finditer(pattern, log):
        n, page, offset, take, used = map(int, m.groups()[:5])
        scale = float(m[6])
        crop = fitz.Rect(list(map(float, m[7].split(","))))
        assert offset == endpoints.get(n, 0), (n, "source discontinuity")
        assert 0 < take <= height-top-used, (n, page, "physical overflow")
        assert used == output_ends.get(page, 0), (n, page, "output overlap/gap")
        assert left + math.ceil(crop.width * scale - .001) <= width-right+1
        if used+take > height-top-bottom:
            released_pages.add(page)
        if offset and prior_pages[n] != page:
            y = crop.y0 + offset/scale
            for block in source[n-1].get_text("dict")["blocks"]:
                for line in block.get("lines", []):
                    box = fitz.Rect(line["bbox"])
                    if box.intersects(crop) and box.y0+.2 < y < box.y1-.2:
                        errors.append({"source": n, "output": page, "y": y,
                                       "crossed_text_box": list(box)})
            pix = source[n-1].get_pixmap(matrix=fitz.Matrix(scale, scale),
                clip=fitz.Rect(crop.x0, y-1/scale, crop.x1, y+1/scale),
                colorspace=fitz.csGRAY, alpha=False)
            if min(pix.samples, default=255) < 220:
                errors.append({"source": n, "output": page, "y": y,
                               "ink_at_seam": min(pix.samples)})
            seams.append((n, page, y))
        endpoints[n] = offset+take
        prior_pages[n] = page
        output_ends[page] = used+take
    assert endpoints == expected, ("source coverage", endpoints, expected)
    assert len(output_ends) == len(output), "Missing output pages"
    result = {"source_pages": len(expected), "output_pages": len(output),
              "checked_page_seams": len(seams),
              "bottom_margin_released_pages": len(released_pages), "errors": errors}
    print(json.dumps(result, ensure_ascii=False, indent=2))
    assert not errors, f"{len(errors)} unsafe seam checks"
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source")
    parser.add_argument("output")
    parser.add_argument("log")
    args = parser.parse_args()
    verify(args.source, args.output, args.log)
