# p3stdbeng

p3stdbeng is a small single-table SQL database engine written from scratch in
C++20, built layer by layer: block files, a buffer pool, slotted heap pages
with two free-space lists, a tuple codec, a JSON catalog, a SQL layer that
parses, binds, and executes statements against that storage, and two index
types over one column — an ISAM index with announced access paths, and a
B+tree built to the class lecture on tree-based indexing (after Ramakrishna
& Gehrke).

**This is the student version: everything works except the four B+tree
operations.** The methods declared in `index/BPlusTree.h` — documented
there in full, with their lecture-slide references — are blank stubs in
`index/BPlusTree.cpp`, each opening with a `// TODO(student):` sketch of
the algorithm. Your task is to implement them until the check suite
passes. Everything else — the TUP2 heap file, the buffer manager, the
catalog, the ISAM index, the SQL layer, the executors, the B+tree page
views (`index/BptPages.*`), the bulk loader, and all the tests — is
provided and must not change.

**Due date: DUE DATE: <set by instructor>.**

## What it does

- **Storage**: a table is a heap file of fixed-width records in 4096-byte
  slotted pages organized as two header-addressed free-space lists — pages
  with free space, and full pages — so INSERT picks its page in O(1). A
  structural self-check (`HeapFile::check_structure`) verifies the whole
  structure at any time.
- **Indexing**: one index per column, of either type — ISAM (provided:
  bulk-built, frozen separators, overflow chains) or B+tree (**your work**:
  self-balancing — leaves split on insert, siblings borrow or merge on
  erase, every leaf at the same depth, separators kept exact). Both are
  maintained across INSERT, UPDATE, and DELETE.
  `CREATE INDEX <name> ON <table> (<column>) [USING ISAM | BTREE]` records
  the type in the catalog.
- **Access paths**: every SELECT announces `(via index scan)` or
  `(via heap scan)`. Correctness never depends on the choice — the
  differential oracle holds every path to identical answers.
- **Types**: Integer, Double, and fixed-width String columns. The binder is
  type-strict, and an integer operation whose result is not representable
  in 32 bits is a named run-time error.
- **Catalogs**: each table's schema lives in a JSON file under
  `tablecatalog/`; data files are `data/<table>.tbl` and index files
  `data/<table>.tbl.<column>.idx` beside them.

## Your work

The stubbed methods, in the suggested implementation order:

1. **`verify_rec`** — the integrity walk (reachability, occupancy, order,
   separator exactness, leaf-depth uniformity, dangling rids). Build the
   net first: every later step gets caught by it.
2. **`tree_search`** — the recursive descent (slides 17, 24).
3. **`run_start` + `gather_run` + `locate_in_run`** — the equal-key run
   through the doubly linked leaves, including the boundary-duplicate
   subtlety.
4. **`insert_leaf` + `insert_aux`** — split propagation (slides 34-37).
5. **`erase_leaf` + `erase_rec` + `fix_child_underflow` +
   `borrow_from_left` + `borrow_from_right` + `merge_child`** — the
   rebalancing cascade (slides 38-49).

The headers (`index/BPlusTree.h`, `index/BptPages.h`) are the assignment's
documentation: every method's contract — purpose, parameters, return value
— is written at its definition, and the design constants (order d, the
split boundaries, the routing rule) are fixed there. Do not modify the
header files or any provided file.

## Repository structure

| Directory | Purpose |
|-----------|---------|
| `file/` | Block-level I/O: 4096-byte block read/write on plain files |
| `bufpool/` | Buffer pool and buffer manager: pin/unpin, eviction, write-back |
| `policy/` | Replacement policies (LRU) injected into the buffer manager |
| `heapfile/` | The TUP2 heap file: slotted pages, two free-space lists, self-check |
| `tuple/` | In-memory row values (`Tuple`, `Column`) |
| `schema/` | Table schema, JSON catalog loader + atomic writer, the tuple serializer, CSV bulk load, a row generator |
| `sql/` | SQL lexer, parser, AST (with a printer and a draw helper) |
| `index/` | `KeyCodec`, the ISAM page views and `IsamIndex` (provided), the B+tree page views (`BptPages`, provided), and the B+tree (`BPlusTree` — **the stubs you implement**) |
| `exec/` | `Value`, `bind_expr`, `Evaluator`, `Planner` (access path), `Executor` (the session state and statement dispatch) |
| `tools/` | `bulk_load` — the pool-free bootstrap loader that builds TUP2 files directly |
| `openspec/` | Behavior specs per capability (the contracts your work is graded against) |

Three binaries share the library code:

- `p3stdbeng` — the check suite (`main.cpp`): the storage, SQL, index,
  exec, loader, and B+tree suites, ending with the differential oracle.
- `p3stdbeng_cli` — the SQL session (`p3stdbeng_cli.cpp`): a REPL where
  statements accumulate until a `;`, plus the escape hatches (`scan`,
  `read`, `seek`, `bulk`, `append`, `checkindex`, `quit`).
- `p3stdbeng_bench` — the index-vs-heap benchmark (it passes once your
  B+tree works).

## Building

CMake 4.3+ with a C++20 compiler:

```sh
cmake -S . -B build
cmake --build build
```

## Testing

Run the check suite (also registered with CTest):

```sh
ctest --test-dir build --output-on-failure
# or directly:
./build/p3stdbeng
```

The suite is the specification: every check names the behavior it demands,
and the B+tree suite's randomized sequences (inserts and erases checked
against a shadow model, with index verification demanded clean after every
step) leave nowhere for a subtle bug to hide. The storage, SQL, index
(ISAM), and loader suites pass before you start — they are the provided
infrastructure's own regression net, and they must stay green.

## The session

```sh
./build/p3stdbeng_cli
```

```sql
CREATE TABLE person (pid INTEGER, name VARCHAR(10), age INT, city VARCHAR(3));
INSERT INTO person VALUES (1, 'Ruth', 42, 'PR'), (2, 'Bob', 25, 'NY');
CREATE INDEX person_pid ON person (pid) USING BTREE;
SELECT name FROM person WHERE pid = 2;      -- (via index scan) - your B+tree
UPDATE person SET age = age + 1 WHERE pid = 1;
DELETE FROM person WHERE pid = 2;
checkindex person person
quit
```

All errors are returned as named strings — the engine reports what failed
instead of throwing (the B+tree's container-like accessors being the one
exception, per their documented contracts).

## Submission

Implement the stubbed methods until the full check suite passes
(`ctest --test-dir build --output-on-failure` exits 0). Submit the
implementation files you changed — `index/BPlusTree.cpp` — unless your
instructor specifies a different procedure.
