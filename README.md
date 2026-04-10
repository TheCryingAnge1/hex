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

Methods tried since that note

- Replaced the bounded local search with exhaustive backtracking over completion
  plans.
- Added an in-memory transposition cache for outer search states with a
  canonicalized partial-graph fingerprint.
- Added progress telemetry, depth counters, cache stats, and candidate
  generation diagnostics.
- Fixed a major correctness bug where completion plans generated from one
  materialized root variant were being replayed onto a different variant.
- Widened leaf-root materialization:
  - multiple branch/split/outward assignments,
  - generic neighborhood shell as an additional leaf variant,
  - center-style fallback retained as another variant.
- Relaxed the old leaf-root participation cap and shifted pruning effort toward
  exact residual feasibility bounds instead.
- Replaced local completion permutation search with an include/exclude
  edge-subset search.
- Added exact local packing bounds and early pending-root impossibility pruning
  during both candidate generation and outer branching.
- Added cached per-variant optimistic loop ceilings to support the new
  feasibility checks.

Current status

- Depth 0 and depth 1 validate.
- Depth 2 still fails under the current neighborhood/completion model.
- The solver is now much closer to exhausting the modeled depth-2 search space
  rather than timing out in obvious search-order pathologies.
- The recurring failure is still local-root completion failure, but the exact
  failing root moves as the model is widened:
  - earlier runs repeatedly died at roots like `5`, `11`, and `12`,
  - current runs often reach larger states and then die at leaf roots like `7`
    or `8` with `no completion candidates`.
- The important interpretation is:
  - the remaining blocker is likely model incompleteness or overconstraint,
    not just missing brute force.

What the current code appears to prove

- With the present role-specific materialization and completion rules, the
  reachable depth-2 search space can be explored aggressively with pruning and
  still does not yield a valid completion.
- That is not proof that the target graph is impossible.
- It is evidence that the current local neighborhood assumptions, especially
  around non-center roots, are still excluding viable constructions or
  misclassifying them.

Suggestions going forward

- Implement explicit `split`-root materialization instead of letting split roots
  fall back to center/generic handling. The pattern notes already call split and
  frontier refinement the active incomplete area.
- Implement explicit `frontier`-root materialization for shifted roots that are
  not yet specialized. The current generic shell is only a stopgap.
- Revisit the leaf-root shell around failing roots (`7`, `8` in recent runs):
  - the current 4x3 child accounting may still be too rigid,
  - outward/frontier reuse may need to branch more than it currently does,
  - some legal local configurations may not fit the existing branch grouping.
- Distinguish search-order heuristics from impossibility pruning. Ordering may
  safely use the most constrained variant; pruning must only fire when all
  variants are impossible.
- If deeper runs are resumed, keep the current telemetry. It is now good enough
  to separate model failures from search failures.

Next suggested experiment

- Add dedicated split-root and frontier-root neighborhood builders, then rerun
  the depth-2 validation first before attempting deeper expansion.
- After that, branch the failing leaf-root shell over a broader set of outward
  reuse/grouping interpretations and check whether the repeated depth-2
  dead-end moves again or disappears.

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
