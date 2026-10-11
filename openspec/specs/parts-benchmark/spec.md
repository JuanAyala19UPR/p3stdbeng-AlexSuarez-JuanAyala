# parts-benchmark

## Purpose

A repeatable benchmark executable that measures the engine's search cost on N `parts` rows (default one million), first without indexes (heap scan) and then with an ISAM index on the searched column (index scan), so the value of the index is a measured number rather than a claim.

## Requirements

### Requirement: Clean-slate start
Every benchmark run SHALL begin by dropping every index declared on the `parts` table, tolerating their absence — a genuine heap-scan baseline requires no indexes. In the default mode the run SHALL then also drop the `parts` table itself, so the run starts with no `parts` catalog, no `data/parts.tbl`, and no index files. In `--reuse` mode the table SHALL be kept: only the indexes are dropped, so the catalog and data file survive for reuse.

#### Scenario: Run after a previous run
- **WHEN** a previous run left `tablecatalog/parts.json`, `data/parts.tbl`, and `data/parts.tbl.partid.idx` on disk and a default-mode run starts
- **THEN** the new run removes all of them via the engine's DROP statements before creating anything

#### Scenario: First run on a bare checkout
- **WHEN** no `parts` table or indexes exist and a default-mode run starts
- **THEN** the drop attempts report absence without failing the run

#### Scenario: Reuse run keeps the data
- **WHEN** a previous run left the `parts` catalog and data file and a `--reuse` run starts
- **THEN** the run drops any leftover indexes but does not drop the table, and `data/parts.tbl` still holds its rows

### Requirement: Record count argument
The run SHALL accept the number of rows to generate as a `--rows N` (or `--rows=N`) argument, defaulting to 1,000,000, and a `--reuse` flag. The two SHALL NOT be combined: a run carrying both SHALL fail with a named argument error before anything is created. In default mode the value SHALL be validated before anything is created: a non-numeric, zero, negative, or greater-than-9,999,999 count SHALL fail with a named error and a non-zero exit. In `--reuse` mode the record count is not an argument: N SHALL be the row count of the existing table, and every N-derived rule (the lookup key 54645 when N >= 54645, otherwise N) SHALL follow that count.

#### Scenario: Default count
- **WHEN** the benchmark runs with no `--rows` argument
- **THEN** it loads 1,000,000 rows

#### Scenario: Explicit count
- **WHEN** the benchmark runs with `--rows 100`
- **THEN** it loads exactly 100 rows and the report reflects the smaller run

#### Scenario: Invalid count rejected
- **WHEN** the benchmark runs with `--rows abc`, `--rows 0`, `--rows -5`, or `--rows 10000000`
- **THEN** it fails with a named error before any file is created and exits non-zero

#### Scenario: Rows and reuse conflict
- **WHEN** the benchmark runs with both `--rows 100` and `--reuse`
- **THEN** it fails with a named argument error before any file is touched and exits non-zero

#### Scenario: Reuse derives the count
- **WHEN** a `--reuse` run starts on a table holding 100,000 rows
- **THEN** N is 100,000, the lookup key is 54645, and the report and measurements reflect that count

### Requirement: Reuse mode
A `--reuse` run SHALL skip creating the table and loading data, and instead reuse the existing `parts` table and its data file. Before any measurement, the run SHALL verify the data is actually there: a missing `parts` catalog, a missing `data/parts.tbl`, or a table with zero rows SHALL fail with a named error and a non-zero exit. The run SHALL then verify the reused file against the regenerated rows exactly as a loading run does (row count and the per-row spot checks), so a reuse run whose file does not match the generator's data fails loudly.

#### Scenario: Reuse with data present
- **WHEN** a previous run left the `parts` catalog and a data file holding rows, and a `--reuse` run starts
- **THEN** no table is created and no data is loaded, N is the existing table's row count, and all measurements proceed

#### Scenario: Reuse with no data fails
- **WHEN** a `--reuse` run starts with no `parts` table (bare checkout), or with a table whose data file is missing or empty
- **THEN** it fails with a named error before any measurement and exits non-zero

#### Scenario: Reuse verifies the file
- **WHEN** a `--reuse` run's regenerated rows are compared against the reused file
- **THEN** the count and the spot-checked rows match the generator's data, and a mismatch fails the run by name

### Requirement: Parts table and generated data
In default mode the run SHALL create the table `parts (partid INTEGER, pname VARCHAR(10), pcolor VARCHAR(10), pweight DOUBLE, pprice DOUBLE, pmaterial VARCHAR(20))` — mapping the requested `CHAR(n)` columns to the engine's `VARCHAR(n)` — and load exactly N rows into it (N is the record count argument). In `--reuse` mode the run SHALL NOT create the table or load data; it SHALL reuse the existing table's contents and SHALL still generate the N rows in memory (N is the existing table's row count) with the same deterministic seed, for verification only. Data generation SHALL be deterministic (fixed seed) and produce:

- `partid`: sequential integers 1 through N, so the benchmark's lookup key (54645 when N >= 54645, otherwise N) exists exactly once;
- `pcolor`: one of `red`, `blue`, `white`, `beige`, `green`, `black`, `yellow`;
- `pprice`: a normal distribution clamped to [0.5, 3000] (mean 1500.25, stddev ≈ 499.92);
- `pweight`: a normal distribution clamped to [1, 50] (mean 25.5, stddev ≈ 8.17);
- `pname` and `pmaterial`: reproducible strings within their declared widths.

#### Scenario: Deterministic regeneration
- **WHEN** two runs complete on the same machine with the same record count
- **THEN** both runs generate identical row contents and identical row counts (the configured N)

#### Scenario: Lookup key exists
- **WHEN** the data is in place (loaded, or reused)
- **THEN** exactly one row has the configured lookup key (`partid = 54645` when N >= 54645, otherwise `partid = N`) and every `pprice` lies in [0.5, 3000] and every `pweight` in [1, 50]

#### Scenario: Reuse regenerates for verification only
- **WHEN** a `--reuse` run regenerates the rows in memory
- **THEN** no rows are written to the data file, and the regenerated rows serve the fingerprint, the Q2 expected count, and the spot checks against the reused file

### Requirement: Baseline measurement without index
After the data is in place — just loaded, or reused with its leftover indexes dropped — with no indexes on the table, the run SHALL execute both benchmark queries and, for each, record the wall-clock execution time, the announced access path, and the number of rows returned:

- Q1: `SELECT * FROM parts WHERE partid = K;` where K is 54645 when N >= 54645, otherwise N (the largest generated partid)
- Q2: `SELECT * FROM parts WHERE pprice >= 1.0 AND pprice <= 10.00;`

Both SHALL be announced as heap scans (no index exists yet).

#### Scenario: Baseline runs on the heap path
- **WHEN** Q1 and Q2 execute after the data is in place
- **THEN** both report the heap-scan access path with recorded times, and Q1 returns exactly 1 row while Q2 returns every row with 1.0 <= pprice <= 10.00

### Requirement: Indexed measurement per query
The run SHALL, for each query in order: build an ISAM index on the column that query searches (`partid` for Q1, `pprice` for Q2), record the index build time, and re-execute the same query text, recording the new time, the announced access path, and the row count. The run SHALL then drop that ISAM index, build a B+tree on the same column, record the B+tree build time, and re-execute the same query text again, recording the new time, the announced access path, and the row count — leaving no index on the column when the next query's baseline is taken. Q2's baseline time SHALL be taken before `pprice` carries any index.

#### Scenario: Q1 re-run after its index exists
- **WHEN** the `partid` ISAM index build completes and Q1 re-executes
- **THEN** Q1 is announced as an index scan, its time is recorded, and it returns the same single row as the baseline

#### Scenario: Q2 re-run after its index exists
- **WHEN** the `pprice` ISAM index build completes and Q2 re-executes
- **THEN** Q2 is announced as an index scan, its time is recorded, and it returns the same row set as the baseline

#### Scenario: Q1 re-run again on the B+tree
- **WHEN** the `partid` ISAM index has been dropped and replaced by a B+tree, and Q1 re-executes
- **THEN** Q1 is announced as an index scan, its time and the B+tree build time are recorded, and it returns the same single row as the baseline

#### Scenario: Q2 re-run again on the B+tree
- **WHEN** the `pprice` ISAM index has been dropped and replaced by a B+tree, and Q2 re-executes
- **THEN** Q2 is announced as an index scan, its time and the B+tree build time are recorded, and it returns the same row set as the baseline

#### Scenario: Each query's baseline is index-free
- **WHEN** Q2's baseline executes
- **THEN** no index exists on `pprice` (the `partid` index from Q1's passes was already dropped), and the run reports Q2's baseline as a heap scan
### Requirement: Equal answers on both access paths
For each query, the rows returned by every indexed run — ISAM and B+tree — SHALL equal the rows returned by the baseline heap-scan run (as a set). The run SHALL fail loudly if any path's answers differ.

#### Scenario: Answers agree
- **WHEN** a query's indexed run finishes (either type)
- **THEN** its result set matches the baseline run's result set exactly, and the benchmark reports agreement

#### Scenario: Answers diverge
- **WHEN** an indexed run (either type) returns a different set of rows than the baseline run
- **THEN** the benchmark reports a mismatch as a failure, naming the query and the index type
### Requirement: Comparison report
At the end of a run, the benchmark SHALL print one report showing, per query: the baseline time, the ISAM time and speedup, and the B+tree time and speedup (each speedup as baseline/indexed); the ISAM and B+tree build times for each query's column; and the load time for the N generated rows. A `--reuse` run SHALL print the data provenance in place of the load time: marked as reused, with the reused row count. The report SHALL be plain text on standard output.

#### Scenario: Full report after a successful run
- **WHEN** all measurements complete in default mode
- **THEN** the report shows the six measured query times (two queries x heap/ISAM/B+tree), all four build times (two columns x two index types), the load time, and per-query speedups for both index types

#### Scenario: Reuse report marks provenance
- **WHEN** a `--reuse` run completes
- **THEN** the report marks the data as reused with the reused row count, and no load time is claimed