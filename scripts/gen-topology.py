#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# YAML -> C generator. Reads topology/*.yaml and emits a single
# board_table.c containing the global board_profiles[] array + count.
#
# PyYAML is preferred. If unavailable, falls back to a minimal parser
# that handles the subset used by topology files in this repo.

import os
import sys
import glob
import re

try:
    import yaml  # type: ignore
    HAVE_YAML = True
except ImportError:
    HAVE_YAML = False


def minimal_yaml_parse(text):
    """Tiny YAML subset: top-level scalars, lists of inline dicts ({k: v, ...}),
    lists of scalars, multi-line `|` block for `notes:`. Good enough for our
    board files. Only used when PyYAML missing."""
    out = {}
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line.strip() or line.lstrip().startswith("#"):
            i += 1
            continue
        if not line.startswith(" "):
            m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):\s*(.*)$", line)
            if not m:
                i += 1
                continue
            key, rest = m.group(1), m.group(2).strip()
            if rest == "|":
                i += 1
                buf = []
                while i < len(lines) and (lines[i].startswith("  ") or not lines[i].strip()):
                    buf.append(lines[i][2:] if lines[i].startswith("  ") else lines[i])
                    i += 1
                out[key] = "\n".join(buf).rstrip()
                continue
            if rest == "":
                items = []
                i += 1
                while i < len(lines):
                    ln = lines[i]
                    if not ln.strip() or ln.lstrip().startswith("#"):
                        i += 1
                        continue
                    if not ln.startswith("  "):
                        break
                    s = ln.strip()
                    if s.startswith("- "):
                        items.append(parse_inline(s[2:]))
                    else:
                        break
                    i += 1
                out[key] = items
                continue
            out[key] = parse_scalar(rest)
            i += 1
            continue
        i += 1
    return out


def parse_inline(s):
    s = s.strip()
    if s.startswith("{") and s.endswith("}"):
        d = {}
        body = s[1:-1]
        depth = 0
        current = ""
        parts = []
        for ch in body:
            if ch == "{":
                depth += 1
                current += ch
            elif ch == "}":
                depth -= 1
                current += ch
            elif ch == "," and depth == 0:
                parts.append(current)
                current = ""
            else:
                current += ch
        if current.strip():
            parts.append(current)
        for p in parts:
            if ":" not in p:
                continue
            k, _, v = p.partition(":")
            d[k.strip()] = parse_scalar(v.strip())
        return d
    return parse_scalar(s)


def parse_scalar(s):
    s = s.strip()
    if not s:
        return ""
    if s.startswith('"') and s.endswith('"'):
        return s[1:-1]
    if s.startswith("'") and s.endswith("'"):
        return s[1:-1]
    if s in ("true", "True", "yes"):
        return True
    if s in ("false", "False", "no"):
        return False
    try:
        return int(s)
    except ValueError:
        pass
    return s


def load_yaml(path):
    text = open(path).read()
    if HAVE_YAML:
        return yaml.safe_load(text)
    doc = minimal_yaml_parse(text)
    if "products" in doc and isinstance(doc["products"], list):
        flat = []
        for entry in doc["products"]:
            if isinstance(entry, str) and entry.startswith("- "):
                flat.append(parse_scalar(entry[2:]))
            else:
                flat.append(entry)
        doc["products"] = flat
    return doc


C_ESC = {"\\": "\\\\", '"': '\\"', "\n": "\\n", "\t": "\\t"}


def cstr(s):
    if s is None:
        return "NULL"
    out = "".join(C_ESC.get(c, c) for c in str(s))
    return f'"{out}"'


def emit_profile(slug, doc):
    pkgs = doc.get("packages", []) or []
    lines = []
    lines.append(f"static const board_package_t {slug}_packages[] = {{")
    for p in pkgs:
        width = p.get("chip_width", doc.get("chip_width", 8))
        lines.append(
            "    { .designator = %s, .channel = %d, .rank = %d, "
            ".byte_lane = %d, .chip_width = %d, .location_hint = %s },"
            % (
                cstr(p["designator"]),
                int(p["channel"]),
                int(p["rank"]),
                int(p["byte_lane"]),
                int(width),
                cstr(p.get("location_hint")),
            )
        )
    lines.append("};")
    lines.append("")

    products = doc.get("products", []) or []
    prod_init = ", ".join(cstr(x) for x in products)
    if not prod_init:
        prod_init = "NULL"

    lines.append(f"static const board_profile_t {slug}_profile = {{")
    lines.append(f"    .board_id          = {cstr(doc['board_id'])},")
    lines.append(f"    .friendly_name     = {cstr(doc['friendly_name'])},")
    lines.append(f"    .products          = {{ {prod_init} }},")
    lines.append(f"    .num_products      = {len(products)},")
    lines.append(f"    .package_count     = {len(pkgs)},")
    lines.append(f"    .chip_width        = {int(doc.get('chip_width', 8))},")
    lines.append(f"    .channels          = {int(doc.get('channels', 2))},")
    lines.append(f"    .ranks_per_channel = {int(doc.get('ranks_per_channel', 1))},")
    lines.append(f"    .packages          = {slug}_packages,")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def slugify(board_id):
    s = re.sub(r"[^A-Za-z0-9]+", "_", board_id).strip("_").lower()
    if not s or s[0].isdigit():
        s = "b_" + s
    return s


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: gen-topology.py <topology-dir> <out.c>\n")
        return 1

    topo_dir, out_path = argv[1], argv[2]
    files = sorted(glob.glob(os.path.join(topo_dir, "*.yaml")))

    header = (
        "// SPDX-License-Identifier: GPL-2.0\n"
        "// AUTO-GENERATED by scripts/gen-topology.py. DO NOT EDIT.\n"
        "// Source: topology/*.yaml\n"
        "\n"
        '#include "stdint.h"\n'
        '#include "board_topology.h"\n'
        "\n"
    )

    body_parts = []
    slugs = []
    for f in files:
        doc = load_yaml(f)
        if not doc or "board_id" not in doc:
            sys.stderr.write(f"skip: {f} (no board_id)\n")
            continue
        slug = slugify(doc["board_id"])
        slugs.append(slug)
        body_parts.append(f"// ---- from {os.path.basename(f)} ----")
        body_parts.append(emit_profile(slug, doc))

    tail = []
    tail.append("const board_profile_t *const board_profiles[] = {")
    for s in slugs:
        tail.append(f"    &{s}_profile,")
    tail.append("};")
    tail.append(f"const unsigned board_profile_count = {len(slugs)};")
    tail.append("")

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w") as fh:
        fh.write(header)
        fh.write("\n".join(body_parts))
        fh.write("\n")
        fh.write("\n".join(tail))

    sys.stderr.write(f"wrote {out_path} with {len(slugs)} profile(s)\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
