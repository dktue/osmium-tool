# apply-changes block passthrough (prototype)

This branch is an experimental prototype. It is **not** an upstream pull request.
It was developed with the help of an AI coding assistant (Claude). The upstream
project does not accept pull requests with AI content, so this code lives only
in this fork, as a proof of concept for the idea discussed in the upstream issue.

## What it does

`osmium apply-changes` normally decodes, merges and re-encodes the whole input
file. When an up-to-date planet is kept current with hourly or daily diffs, most
of the file is untouched by a given diff. With this branch, PBF blocks that no
change touches are copied to the output as raw bytes, without decoding or
recompressing them. Touched blocks are decoded, merged and re-encoded in the
thread pool. `--no-passthrough` switches back to the old behaviour.

The output contains the same objects as upstream `apply-changes` (verified by
comparing `osmium cat -f opl` output). It is usually not byte-identical, because
block boundaries differ.

## Measurements

AMD Ryzen 9 3900X (12C/24T), 32 GB RAM, WSL2. Planet of 2026-10-05, re-encoded
with `osmium cat` (libosmium block layout). Upstream and this branch alternated,
3 runs each, output to `/dev/null`. Median wall time in seconds.

| Input + diff | upstream | this branch | speedup |
|---|---|---|---|
| planet + 1 hourly diff | 2521 | 200 | 12.6× |
| planet + 1 daily diff | 2428 | 285 | 8.5× |
| Germany + 1 daily diff | 112 | 18.9 | 5.9× |
| Germany + 7 daily diffs | 119 | 29.9 | 4.0× |
| Baden-Württemberg + 1 daily diff | 15.0 | 1.9 | 7.7× |
| Baden-Württemberg + planet daily diff (almost every block touched) | 30.4 | 22.1 | 1.38× |

Limits:

- With real output instead of `/dev/null`, planet runs become disk-bound and the
  gain is much smaller. Chained Germany updates with real output: 5.0–5.8×.
- The first update of an original planet.osm.org file touches far more blocks
  (about 14 % of the bytes for an hourly diff instead of 1.5 %) and gains less.
- In the almost-everything-touched case it uses more CPU than upstream.
- Long update chains fragment the file (more, smaller blocks); not addressed yet.
- If the input contains the same object ID more than once, upstream keeps only
  the first copy, while copied blocks keep all copies (documented in the man page).
