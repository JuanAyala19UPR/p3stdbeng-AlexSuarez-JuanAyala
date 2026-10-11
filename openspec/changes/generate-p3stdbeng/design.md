# Design

## Context

See the proposal — Why. The reference is `../p2stdbeng`: p2dbeng with one module's `.cpp` stubbed, archive/EXPLAIN/.opencode removed, binaries renamed, README rewritten as the handout, and the full check suite kept as grader. p2std's stub bodies follow one form: the original method signature kept, a `// TODO(student):` comment sketching the steps (with the algorithm's key facts), `(void)`-casts for the unused parameters, and a compile-correct failure return. The twelve stubbed methods and the provided set were confirmed with the project's owner: **stub at the auxiliary level; build is provided**.

## Goals / Non-Goals

**Goals:**
- `../p3stdbeng` builds out of the box, runs its suite, and fails exactly on the B+tree-dependent checks.
- The student's work surface is the twelve algorithm methods — the four lecture operations and their sub-steps — with signatures, contracts, and slide references in the untouched headers.
- The solution (`this` repository) remains untouched and green.

**Non-Goals:**
- No assignment handout document (the .docx is the professor's to write).
- No changes to this repository's code, specs, or suite.
- No grading automation beyond the supplied CTest suite.

## Decisions

### D1 — The stub set is the twelve algorithm methods; everything else in the file is provided
Stubbed: `verify_rec`, `tree_search`, `run_start`, `gather_run`, `locate_in_run`, `insert_leaf`, `insert_aux`, `erase_leaf`, `erase_rec`, `fix_child_underflow`, `borrow_from_left`, `borrow_from_right`, `merge_child` (thirteen names — `insert_aux` and `erase_rec` included; the count is twelve algorithm methods plus the leaf/borrow sub-steps they own). Provided in place: the constructor, `write_header`/`read_header`, pin helpers, `alloc_next_block`, `chain_freed`, the four order helpers, `build`, `lookup`, `range`, `insert`, `erase`, `verify` (the wrapper that assembles the walk and prints nothing), and `rebuild_from_leaves`. Rationale: the wrappers carry the pin/count/header bookkeeping that is not the lesson; `build` is scaffolding that makes the tree exist; `rebuild_from_leaves` is a subtle escape hatch, not a lecture topic (its absence would also make the duplicate-heavy shadow checks unpassable even with correct lecture work).

### D2 — Stub bodies compile and fail honestly, in p2std's form
Each stub keeps the exact signature, opens with `// TODO(student):` naming the algorithm, its steps sketch, and its lecture-slide references, then `(void)`-casts the parameters and returns the type's failure value (`false`, `idx::kNoPage`, or 0). Rationale: the student tree must build before any work; the failure values route through the existing named-error conventions so failure messages stay meaningful ("index is not open"-style refusals still behave); and the hint sketch is what the lecture's slides back up.

### D3 — The failure surface is designed, then verified
The generation's acceptance: the student tree **builds cleanly**, its suite runs, and the result splits exactly — storage, sql, index (ISAM), and loader suites fully green; the B+tree module suite red across its checks; the exec suite green except its B+tree-dependent checks (the btree oracle gauntlet, the BTREE-declared dispatch checks). The suite's exit code is non-zero. This split is asserted as part of the generation's verification, so a stub that accidentally passes or a provided method accidentally broken is caught at generation time.

### D4 — The surgery list mirrors p2std exactly
From the copied tree: delete `openspec/changes/archive/` (the answer key — the B+tree change's design.md holds the split/borrow/merge details), delete `EXPLAIN.md`, delete `.opencode/`, delete `build*/`/`data/`/`.git` from the copy, rename the three binaries and the CLI source (`p3stdbeng.cpp`? no — the CLI source becomes `p3stdbeng_cli.cpp`, the check binary `p3stdbeng`, the bench `p3stdbeng_bench`), set the CMake project name to `p3stdbeng`, keep `openspec/specs/` (the behavioral contract, including `bplustree-index`), keep the `tablecatalog/` fixtures, and rewrite `README.md` as the student handout (what works, what is stubbed, the implement-until-the-suite-passes contract, a due-date placeholder). `git init` with an initial commit at the end. Rationale: p2std's students received exactly this shape; the archived design documents are the only place the full solution narrative exists.

### D5 — The README handout names the work surface precisely
Following p2std's README form: the engine description (identical to p3dbeng's), then the student paragraph — everything works except the four B+tree operations; the methods declared in `index/BPlusTree.h`, documented there in full with their lecture-slide references, are blank stubs in `index/BPlusTree.cpp`; implement them until the check suite passes; the headers must not change; a suggested order (verify first — the net before the surgery — then search, insert, erase). Due date as a placeholder for the professor.

## Risks / Trade-offs

- [A stub with a wrong failure value could crash rather than fail] → Every stub returns the type's failure value through the existing conventions; the generation's run of the student suite (D3) catches crashes as failures and the split assertion pins the expected failure count.
- [The provided `rebuild_from_leaves` calls `build` and the wrapper calls the stubbed auxiliaries — the call graph crosses the stub boundary] → The stub bodies are self-contained failures; the provided code paths that call them (wrapper → `insert_aux`) return failure cleanly, which the suite reports as failing checks, not crashes.
- [Copy drift: p3dbeng moves on after the generation] → The student tree is a snapshot by design (like p2std); the generation is cheap to re-run if the solution ever changes.

## Migration Plan

Generate into a fresh `../p3stdbeng` (fail if it exists), verify the designed failure surface, init git and commit. The parent repository is untouched. Re-running the generation after changes to p3dbeng regenerates the snapshot.

## Open Questions

- The due date on the handout README is a placeholder (`DUE DATE: <set by instructor>`), to be filled by the professor — same as p2std's README carrying its date.
