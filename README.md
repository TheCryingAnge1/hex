# hex

Prototype search code for a 4-regular graph construction problem derived from a
hand-drawn seed pattern.

Constraints

- every node must have degree 4
- no cycle shorter than 6
- every chosen root should see 12 cycles of length 6

Files

- `main.cpp`: current C++ search prototype
- `pattern.doc`: notes from the drawings and expansion assumptions
- `CLOUD_CODEX_PROMPT.md`: handoff brief for a longer cloud Codex search

Recent approach log (depth-4 push)

- Reintroduced image-derived seed links from the hand sketch as the starting
  motif, rather than relying only on a minimal tree seed.
- Added role-aware neighborhood materialization refinements:
  - branch-root and leaf-root neighborhood builders,
  - fallback from leaf neighborhood to center-style neighborhood when no
    productive completion move is detectable.
- Replaced exhaustive local permutation search with a scored greedy + bounded
  beam candidate generator.
- Added constrained-root expansion ordering (pick todo root with smallest
  candidate set first).
- Added diagnostics for materialization/candidate dead-ends; observed repeated
  depth-2 dead-ends at leaf-root completions (e.g., no completion candidates).

Next suggested experiment

- Implement a full brute-force search with a transposition cache (state memo):
  canonicalize each partial graph state (or radius-limited rooted signature)
  and skip revisits.
- Cache key recommendation:
  - sorted node role list,
  - normalized adjacency (canonical relabeling),
  - unresolved todo frontier summary.

Approximate storage cost for a full depth-4 map

- A 4-regular tree ball through depth 4 has:
  - `1 + 4 + 12 + 36 + 108 = 161` roots/nodes in the wavefront model.
- If a complete depth-4 construction lands in roughly `2,000` to `10,000`
  total nodes (practical range seen in this style of expansion), a compact
  adjacency snapshot is roughly:
  - `16 bytes/node` for 4x `int32` adjacency slots, plus role/flags.
  - Raw graph payload: ~`32 KB` (`2k` nodes) to ~`160 KB` (`10k` nodes).
- For brute-force caching, the dominant cost is number of states, not one
  state size. With canonical hashes + compact metadata:
  - ~`64–128 bytes/state` is realistic.
  - `1M` states => ~`64–128 MB`
  - `10M` states => ~`0.64–1.28 GB`
  - `50M` states => ~`3.2–6.4 GB`
