# sql-cli

## Purpose

The session front door: a SQL REPL that accumulates semicolon-terminated statements, survives every error, scripts cleanly when piped, keeps the raw row-id commands as escape hatches for inspecting what SQL claimed, and carries p2dbeng's `append` CSV bulk load.

## Requirements

### Requirement: Statement accumulation and dispatch
A line whose first word is an escape-hatch or session command (`scan`, `read`, `seek`, `bulk`, `append`, `checkindex`, `quit`) SHALL execute immediately. Any other input SHALL accumulate across lines until a line ending in `;`, then be parsed and executed as one SQL statement. The prompt SHALL indicate an unfinished statement while accumulating.

#### Scenario: Multi-line statement
- **WHEN** the user types `SELECT *` on one line and `FROM person;` on the next
- **THEN** one statement is formed and executed, producing one result

#### Scenario: Escape hatch runs bare
- **WHEN** the user types `scan person parts` with no semicolon
- **THEN** the raw scan executes immediately without SQL parsing

### Requirement: Errors never end the session
Every failure — parse error, bind error, execution error, unknown command — SHALL print a message naming the problem and return to the prompt. The session SHALL end only on `quit` or end of input, and the exit code SHALL be 0 unless flushing session files at exit fails, in which case it SHALL be 1.

#### Scenario: Bad input continues
- **WHEN** a statement fails to parse and the user then runs a valid SELECT
- **THEN** the parse error is printed, and the valid statement executes normally afterwards

### Requirement: Piped sessions are scriptable
When input is not a terminal the prompt SHALL be suppressed and output SHALL otherwise be identical to the interactive case, so the same session text produces the same result whether typed or piped.

#### Scenario: Piped session
- **WHEN** a session file containing DDL, INSERT, and SELECT is piped into the CLI
- **THEN** execution proceeds without prompts, prints the same statements' results, and exits with the flush-based exit code

### Requirement: Escape-hatch commands keep their row-id semantics
`scan`, `read`, `seek`, and `bulk` SHALL keep their existing behavior: records addressed by `(page, slot)` row id, scan reporting a count, reads showing a `(pool hit)` marker when the page is already resident, bulk continuing generated numbering after existing rows and maintaining the table's indexes. Files bind to the table they were first used with; naming a different table later SHALL be refused.

#### Scenario: Raw read shows the pool hit
- **WHEN** the same row id is read twice in a session with a small pool
- **THEN** the second read prints the `(pool hit)` marker

### Requirement: append bulk-loads CSV without SQL
The session SHALL accept an `append` command of the form `append <table> <csv-file>` that loads the CSV file's rows into the named table's data file (resolving the file path by the same rules as the other commands), creating the data file when missing, through the same index-maintaining insert path as SQL INSERT. Each invalid row SHALL be reported as `line N: reason` on the diagnostics stream and skipped, never fatal; the command SHALL end with counts of appended and skipped rows.

#### Scenario: Bulk load
- **WHEN** the user runs `append person person.csv` against a table whose data file does not yet exist
- **THEN** every valid CSV row is inserted into the table's data file (and into any declared index), and the command reports the appended and skipped counts

#### Scenario: Invalid rows never abort
- **WHEN** the CSV contains rows that violate the table's structure
- **THEN** each is reported with its line number and reason, valid rows are still appended, and the session continues

### Requirement: checkindex verifies a data file's indexes
`checkindex <table> <file>` SHALL run the index verification over every index the catalog declares for the table against that data file, reporting each index's entry count plus any dangling or out-of-order findings, and `OK` per index when clean. An unknown table SHALL fail with a named error.

#### Scenario: Clean index reports OK
- **WHEN** `checkindex person parts` runs and every index entry resolves to an occupied slot
- **THEN** each index reports its count and `OK`

#### Scenario: Dangling entry reported
- **WHEN** an index holds an entry whose slot is free in the data file
- **THEN** `checkindex` names the offending index, key, and row id, and the report is not clean

### Requirement: Escape-hatch paths resolve through data/
A file argument given to the escape-hatch commands (`scan`, `read`, `seek`, `bulk`, `append`, `checkindex`) that contains no `/` SHALL resolve relative to `data/`. A path containing a `/` SHALL be used exactly as given, so absolute and subdirectory paths keep working.

#### Scenario: Bare name finds the table file
- **WHEN** `scan parts parts` runs for a table stored at `data/parts.tbl`
- **THEN** the scan reads `data/parts.tbl` and reports its records

#### Scenario: Explicit path is honored as given
- **WHEN** `scan parts /tmp/demo.tbl` runs
- **THEN** the scan reads exactly `/tmp/demo.tbl`, not something under `data/`
