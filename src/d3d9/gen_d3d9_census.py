#!/usr/bin/env python3
#
# MADEIRA (WOW64_DESIGN.md section 8.4, measurement 1): generate the
# [d3d9-census] per-method counters for the Direct3D 9 frontend.
#
# This file is part of Madeira's DXMT fork and is distributed under
# GPL-3.0-or-later; see research/dxmt/LICENSE-MADEIRA.md.
#
# Section 8 asks "which of the 320 vtable slots actually get called, and how
# many times per frame", because that number multiplied by the cost of one
# unix call (measurement 2, unixcall-bench-x86.exe) decides whether a
# synchronous shim is viable at all.  There is no choke point in a COM
# frontend -- every slot is a separate virtual function -- so the counter has
# to sit at the top of each one.  Writing 317 of them by hand, and a matching
# name table by hand, is exactly the drift wmt_api_census.c's header comment
# warns about, so BOTH the injected call sites and the name table are emitted
# from one scan by this script:
#
#   ./gen_d3d9_census.py            # rewrite d3d9_census_names.h + inject
#   ./gen_d3d9_census.py --check    # fail if anything is out of date
#   ./gen_d3d9_census.py --strip    # remove every injected line again
#
# The code IS the index into d3d9_census_names[], the same contract the
# winemetal dispatch table has: a hand-maintained second list would silently
# attribute calls to the wrong method.
#
# Line endings are preserved verbatim (this checkout keeps research/dxmt CRLF
# in the working tree while git stores LF -- see WOW64_DESIGN.md section 7.11).

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NAMES_H = "d3d9_census_names.h"
CENSUS_HPP = "d3d9_census.hpp"
INCLUDE_LINE = '#include "d3d9_census.hpp"'
NAMESPACE_LINE = "namespace dxmt {"

# The one method every present path funnels through.  MTLD3D9Device::Present
# and ::PresentEx both forward to it (d3d9_device.cpp:2573, :11972), so
# ticking the frame counter in all three would count one frame as three.
FRAME_METHODS = {("MTLD3D9SwapChain", "Present")}

MARKER = re.compile(r"^\s*D3D9_CENSUS(?:_FRAME)?\(")
STDM = re.compile(r"\bSTDMETHODCALLTYPE\b")
QUALNAME = re.compile(r"\s*([A-Za-z_]\w*)::([A-Za-z_]\w*)\s*\(")


def split_lines(text):
    """Split keeping the exact terminator of each line."""
    return text.splitlines(keepends=True)


def eol_of(text):
    return "\r\n" if "\r\n" in text[:4096] else "\n"


def indent_of(line):
    return line[: len(line) - len(line.lstrip())]


def scan(text):
    """Yield (class, method, insert_offset) for every STDMETHODCALLTYPE
    definition, in source order.  insert_offset is just past the '{' that
    opens the body."""
    out = []
    for m in STDM.finditer(text):
        q = QUALNAME.match(text, m.end())
        if not q:
            continue  # a declaration in a header-like block, not a definition
        depth = 0
        i = q.end() - 1  # the '('
        while i < len(text):
            c = text[i]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        else:
            raise SystemExit("unterminated argument list for %s::%s" % q.groups())
        # skip trailing const / noexcept / override and whitespace to the body
        j = i + 1
        while j < len(text) and text[j] not in "{;":
            j += 1
        if j >= len(text) or text[j] == ";":
            continue  # a pure declaration
        out.append((q.group(1), q.group(2), j + 1))
    return out


def sources(d):
    return sorted(
        fn
        for fn in os.listdir(d)
        if fn.endswith(".cpp") and STDM.search(read(os.path.join(d, fn)))
    )


_cache = {}


def read(path):
    if path not in _cache:
        with open(path, "r", encoding="utf-8", newline="") as f:
            _cache[path] = f.read()
    return _cache[path]


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    _cache[path] = text


def transform(text, entries, strip):
    """Insert (or refresh, or remove) one census line per definition, and the
    census include.  Works back-to-front so earlier offsets stay valid."""
    eol = eol_of(text)
    defs = scan(text)
    assert len(defs) == len(entries), "%d defs vs %d entries" % (len(defs), len(entries))
    for cls, meth, off in reversed(defs):
        nl = text.find("\n", off)
        if nl < 0:
            nl = len(text) - 1
        tail = text[off:nl].rstrip("\r")
        macro = "D3D9_CENSUS_FRAME" if (cls, meth) in FRAME_METHODS else "D3D9_CENSUS"
        line = "  %s(D3D9_CENSUS_%s_%s);" % (macro, cls, meth)
        if tail.strip():
            # body opens on the '{' line: keep it there, prepend the counter
            if MARKER.match(tail):
                cut = text.find(";", off) + 1
                text = text[:off] + ("" if strip else " " + line.strip()) + text[cut:]
            elif not strip:
                text = text[:off] + " " + line.strip() + text[off:]
            continue
        # the usual shape: '{' ends the line.  Consume an existing marker line.
        rest = nl + 1
        after = text[rest:]
        nl2 = after.find("\n")
        nextline = (after[:nl2] if nl2 >= 0 else after).rstrip("\r")
        if MARKER.match(nextline):
            rest += (nl2 + 1) if nl2 >= 0 else len(after)
        text = text[: nl + 1] + ("" if strip else line + eol) + text[rest:]

    has_include = INCLUDE_LINE in text
    if strip and has_include:
        text = text.replace(INCLUDE_LINE + eol + eol, "", 1)
        text = text.replace(INCLUDE_LINE + eol, "", 1)
    elif not strip and not has_include:
        pos = text.find(NAMESPACE_LINE)
        if pos < 0:
            raise SystemExit("no `namespace dxmt {` to anchor the include on")
        text = text[:pos] + INCLUDE_LINE + eol + eol + text[pos:]
    return text


HEADER = """\
/* GENERATED by gen_d3d9_census.py -- DO NOT EDIT.
 *
 * MADEIRA (WOW64_DESIGN.md section 8.4): the [d3d9-census] method codes and
 * their names, emitted from one scan of the STDMETHODCALLTYPE definitions in
 * this directory's .cpp files -- the same scan that injects the D3D9_CENSUS()
 * call sites, so a code can never name a method other than the one that
 * counted it.
 *
 * Distributed under GPL-3.0-or-later; see LICENSE-MADEIRA.md.
 *
 * The name table is behind D3D9_CENSUS_NAME_TABLE so that the 14 translation
 * units which only need the enum do not each carry a copy of 317 strings.
 * That is also why this file has two guards instead of a `#pragma once`: the
 * reporter includes it twice, once through d3d9_census.hpp for the enum and
 * again with D3D9_CENSUS_NAME_TABLE defined, and `#pragma once` would make
 * the second include a silent no-op.
 */
#ifndef D3D9_CENSUS_NAMES_H
#define D3D9_CENSUS_NAMES_H

#define D3D9_CENSUS_COUNT %d

enum d3d9_census_code {
%s};

#endif /* D3D9_CENSUS_NAMES_H */

#if defined(D3D9_CENSUS_NAME_TABLE) && !defined(D3D9_CENSUS_NAMES_TABLE_DEFINED)
#define D3D9_CENSUS_NAMES_TABLE_DEFINED
static const char *const d3d9_census_names[D3D9_CENSUS_COUNT] = {
%s};
#endif
"""


def main():
    strip = "--strip" in sys.argv
    check = "--check" in sys.argv
    d = HERE

    files = sources(d)
    all_entries = []
    per_file = []
    for fn in files:
        text = read(os.path.join(d, fn))
        ents = [(c, m) for c, m, _ in scan(text)]
        per_file.append((fn, ents))
        all_entries.extend(ents)

    eol = eol_of(read(os.path.join(d, files[0])))
    enum_body = "".join(
        "  D3D9_CENSUS_%s_%s = %d,\n" % (c, m, i)
        for i, (c, m) in enumerate(all_entries)
    )
    names_body = "".join('    "%s::%s",\n' % (c, m) for (c, m) in all_entries)
    header = HEADER % (len(all_entries), enum_body, names_body)
    if eol != "\n":
        header = header.replace("\n", eol)

    changed = []
    npath = os.path.join(d, NAMES_H)
    if strip:
        pass
    elif not os.path.exists(npath) or read(npath) != header:
        changed.append(NAMES_H)
        if not check:
            write(npath, header)

    for fn, ents in per_file:
        p = os.path.join(d, fn)
        old = read(p)
        new = transform(old, ents, strip)
        if new != old:
            changed.append(fn)
            if not check:
                write(p, new)

    verb = "stale" if check else ("stripped" if strip else "updated")
    print("%d methods across %d files; %d file(s) %s" % (len(all_entries), len(files), len(changed), verb))
    for c in changed:
        print("   ", c)
    if check and changed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
