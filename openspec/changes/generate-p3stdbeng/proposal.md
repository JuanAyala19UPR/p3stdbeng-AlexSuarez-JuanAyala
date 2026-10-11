# Proposal

## Why

The B+tree module was built to be torn out and handed to students: its headers carry per-method contracts (purpose, parameters, return value) with lecture-slide references, its recursive auxiliaries' signatures are fixed, and its 96-check module suite plus the exec oracle's B+tree gauntlet form a complete grading contract. The parent course materials set the pattern: `../p2stdbeng` is p2dbeng with exactly one module's `.cpp` reduced to guided stubs (`// TODO(student):` sketches in every body), the archived changes and `EXPLAIN.md` removed (they are the answer key), `.opencode/` stripped, the binaries renamed, the README rewritten as the student handout, and the full check suite kept as both specification and grader. This change generates `../p3stdbeng` — the student version of p3dbeng — by the same pattern: the whole engine provided and working, the B+tree's algorithm methods stubbed, and the suite green everywhere except the work students must do.

## What Changes

- **A new sibling project `../p3stdbeng`**, generated from this repository by copy and surgery:
  - The full engine is provided and working — TUP2 storage, both index types' infrastructure, exec lineage, CLI, benchmark, catalog fixtures, and all six check suites unchanged.
  - **`index/BptPages.cpp` is provided in full** (byte-level page views — the p2std pattern where `SlottedPage` was infrastructure).
  - **`index/BPlusTree.cpp` is stubbed at the auxiliary level**: the constructor, header/pin/allocation plumbing, order helpers, `build`, the public wrappers (`lookup`, `range`, `insert`, `erase`), `rebuild_from_leaves`, and `chain_freed` stay as-is; the twelve algorithm methods become guided stubs — `tree_search`, `run_start`, `gather_run`, `locate_in_run`, `insert_leaf`, `insert_aux`, `erase_leaf`, `erase_rec`, `fix_child_underflow`, `borrow_from_left`, `borrow_from_right`, `merge_child`, and `verify_rec` — each opening with `// TODO(student):` plus the algorithm sketch and its lecture-slide references, with compile-correct stub bodies.
  - The header files are untouched — the contracts are the assignment.
  - The answer key is removed: no `openspec/changes/archive/`, no `EXPLAIN.md`, no `.opencode/`.
  - The binaries are renamed (`p3stdbeng`, `p3stdbeng_cli`, `p3stdbeng_bench`) and the README is rewritten as the student handout ("everything works except the four B+tree operations…"), following `p2stdbeng`'s.
  - `git init` with an initial commit.
- **The designed failure surface**: in the student tree the storage, sql, index, exec infrastructure, and loader suites still pass; the B+tree module suite and the exec oracle's B+tree-dependent checks fail until students implement the stubbed methods. Implementing all twelve makes the full suite pass.

## Capabilities

### New Capabilities

None — the generated project's behavior is this repository's existing behavior; the change is a distribution build. `skip_specs` is declared.

### Modified Capabilities

None.

## Impact

- A new directory `../p3stdbeng` (sibling of this repository); this repository's code is untouched except that the generation reads it.
- The suite-as-grader contract: 96 B+tree module checks plus the exec oracle's B+tree sections define "done" for the students.
- Out of scope: the professor's assignment handout document (the `p2.doc`-style .docx) — the professor writes that separately.
