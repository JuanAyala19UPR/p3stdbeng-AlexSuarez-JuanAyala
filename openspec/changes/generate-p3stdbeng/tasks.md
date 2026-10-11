# Tasks

## 1. Scaffold `../p3stdbeng`

- [ ] 1.1 Copy this repository to a fresh `../p3stdbeng` (fail if it exists), excluding `build*/`, `data/`, `.git`, `.opencode/`, `openspec/changes/archive/`, and `EXPLAIN.md`; keep `openspec/specs/`, `tablecatalog/`, the fixtures, and every module source
- [ ] 1.2 Rename the binaries and set the project identity: CMake project name `p3stdbeng`, check binary `p3stdbeng`, CLI source and binary `p3stdbeng_cli.cpp`/`p3stdbeng_cli`, bench `p3stdbeng_bench`; verify the student tree builds cleanly as-is (no stubs yet)

## 2. Stub the B+tree algorithms

- [ ] 2.1 Replace the twelve algorithm-method bodies in `../p3stdbeng/index/BPlusTree.cpp` with guided stubs per design D2 — `verify_rec`, `tree_search`, `run_start`, `gather_run`, `locate_in_run`, `insert_leaf`, `insert_aux`, `erase_leaf`, `erase_rec`, `fix_child_underflow`, `borrow_from_left`, `borrow_from_right`, `merge_child` — each keeping the exact signature, opening with `// TODO(student):` plus the steps sketch and lecture-slide references, `(void)`-casting unused parameters, and returning the type's failure value; leave every provided method (constructor, header/pin/allocation helpers, order helpers, `build`, the public wrappers, `rebuild_from_leaves`, `chain_freed`) untouched
- [ ] 2.2 Verify the designed failure surface (design D3): the student tree builds cleanly, the suite runs to completion, and the result splits exactly — storage, sql, index, and loader suites fully green; the B+tree module suite red; the exec suite green except its B+tree-dependent checks; suite exit code non-zero

## 3. Student handout

- [ ] 3.1 Rewrite `../p3stdbeng/README.md` as the student handout per design D5: the engine description, the "everything works except the four B+tree operations" paragraph naming the stub file and the untouched headers, the implement-until-the-suite-passes contract, the suggested order (verify first, then search, insert, erase), the suite/CTest invocation, and the due-date placeholder
- [ ] 3.2 `git init` in `../p3stdbeng`, add an initial commit of the generated tree, and confirm this repository's own check suite is still green (the generation must not have touched the solution)
