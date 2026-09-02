#!/usr/bin/env python3
"""ingest_attacks.py -- load the anotak attack tables for every CACHED character.

Why not `run.py --all-chars`: that calls anotak_crawl.discover_chars(), which
fetches the anotak index over the network and hangs when the site is
unreachable. Nothing here needs the network -- all 59 characters were crawled
long ago and sit in ingest/data/anotak_PL*.json. This drives gen_attacks()
straight off that cache.

    PYTHONIOENCODING=utf-8 python tools/re_kb/ingest/ingest_attacks.py
    PYTHONIOENCODING=utf-8 python tools/re_kb/ingest/ingest_attacks.py --dry-run
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import common                      # noqa: E402
import ingest                      # noqa: E402


def cached_chars():
    out = []
    for p in sorted(glob.glob(os.path.join(common.DATA_DIR, "anotak_PL*.json"))):
        m = re.search(r"(PL[0-9A-Fa-f]{2})\.json$", os.path.basename(p))
        if m:
            out.append(m.group(1).upper())
    return out


def main(argv):
    dry = "--dry-run" in argv
    chars = cached_chars()
    print("cached characters: %d" % len(chars))
    if not chars:
        print("no anotak_PL*.json in %s" % common.DATA_DIR)
        return 2

    total_err = 0
    for i, pl in enumerate(chars, 1):
        sql = ingest.gen_attacks(pl)
        n = sql.count("UPSERT attack:")
        if dry:
            print("  [%2d/%d] %s  %d attacks (dry)" % (i, len(chars), pl, n))
            continue
        err = ingest.apply("anotak_%s_atk.surql" % pl.lower(), sql)
        total_err += err
        print("  [%2d/%d] %s  %d attacks, %d ERR" % (i, len(chars), pl, n, err))
    print("-" * 60)
    print("%d characters, %d errors%s"
          % (len(chars), total_err, " (DRY RUN)" if dry else ""))
    return 1 if total_err else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
