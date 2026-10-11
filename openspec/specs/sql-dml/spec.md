# sql-dml

## Purpose

Lets a session put rows in, change them, remove them, and get rows out: INSERT, single-table SELECT with projection, WHERE, and LIMIT, UPDATE with simultaneous assignment, and DELETE — with an index-backed or scan-backed access path chosen per SELECT and always announced, and every declared index maintained across INSERT, UPDATE, and DELETE.

## Requirements

### Requirement: INSERT validates then stores
`INSERT INTO <table> VALUES (<row>), (<row>), ...` SHALL accept only rows whose value count, per-position types, and string widths match the table's catalog. A row that violates the catalog SHALL be refused with a named error and nothing from that statement stored. Every accepted row SHALL land in the data file first and then in every index declared for the table, and be immediately findable by both paths. The statement SHALL report how many rows it stored.

#### Scenario: Valid multi-row insert
- **WHEN** `INSERT INTO person VALUES (1, 'Ruth', 42, 'PR'), (2, 'Amy', 30, 'MA')` runs against the 4-column catalog
- **THEN** two rows are stored, the report says 2, and both a heap scan and an index lookup on `pid` find them

#### Scenario: Bad row refused whole
- **WHEN** one value row has a 20-character name for a `VARCHAR(10)` column
- **THEN** the statement fails with a named error, zero rows from it are stored, and later lookups find none of them

### Requirement: SELECT projects named columns
`SELECT * FROM <t>` or `SELECT <col>, ... FROM <t>` over a single table SHALL print each result row's projected columns as name/value pairs in projection order, followed by the row count. `*` SHALL return all columns in catalog order; an explicit select list SHALL return columns in the order written; a column may be renamed with an alias. Naming an unknown column SHALL fail with a named error before any rows are produced. A WHERE that matches no rows SHALL return an empty result, not an error.

#### Scenario: Projection narrows and orders output
- **WHEN** `SELECT city, pid FROM person WHERE pid = 1` runs on a row `pid=1, name=Ruth, age=42, city=PR`
- **THEN** the output shows only `city=PR` and `pid=1`, in that order, and reports 1 row

#### Scenario: Unknown column refused
- **WHEN** `SELECT nosuch FROM person` runs
- **THEN** the statement fails with a named error and no rows are printed

#### Scenario: No matches
- **WHEN** the WHERE expression matches no rows
- **THEN** the statement succeeds with zero rows and no error

### Requirement: LIMIT caps the result
`SELECT ... [LIMIT <n>]` SHALL stop producing rows once n rows have been produced. A LIMIT on a statement that matches fewer rows SHALL return all of them.

#### Scenario: LIMIT caps the result
- **WHEN** `SELECT * FROM person LIMIT 5` runs against a table holding more than 5 rows
- **THEN** exactly 5 rows are returned

#### Scenario: LIMIT larger than the answer
- **WHEN** `SELECT * FROM person LIMIT 100` runs against a table holding 3 rows
- **THEN** all 3 rows are returned

### Requirement: WHERE expressions evaluate with defined semantics
The WHERE clause SHALL accept the expression language over one row: comparisons, AND/OR/NOT with short-circuit evaluation, arithmetic, and parentheses. Semantics: mixed integer/double arithmetic and comparison promote to double; division of two integers truncates; strings compare lexicographically; there are no NULLs. Comparing a string value with a numeric one, or referencing an unknown column, SHALL fail at bind time with a named error before any row is read.

#### Scenario: Arithmetic in predicates
- **WHEN** `SELECT * FROM person WHERE age / 2 = 21` runs on a row with age 42
- **THEN** the row is returned (integer division truncates), and a row with age 43 is not

#### Scenario: Type mismatch caught at bind time
- **WHEN** `SELECT * FROM person WHERE name = 5` runs
- **THEN** the statement fails with a named bind error and no access path runs at all

### Requirement: Access-path selection is announced and safe
For each SELECT the engine SHALL choose between two access paths and print which it took: `(via index scan)` when an equality or AND-combined range conjunct pins the indexed column to literal bounds, otherwise `(via heap scan)`. Index-path candidates SHALL still pass any remaining conjuncts as a filter before being returned. Top-level OR, or no conjunct naming the indexed column, SHALL take the heap path. Correctness SHALL NOT depend on the choice: both paths SHALL return identical rows — as a set; without ORDER BY the row order is unspecified and may legitimately differ between the paths (the index walks keys in order, the heap scans row ids).

#### Scenario: Equality takes the index
- **WHEN** an index exists on `pid` and `SELECT * FROM person WHERE pid = 42` runs
- **THEN** the marker is `(via index scan)` and only rows with pid 42 are returned

#### Scenario: AND range intersects on the indexed column
- **WHEN** an index exists on `pid` and `SELECT * FROM person WHERE pid > 10 AND pid < 20 AND city = 'PR'` runs
- **THEN** the marker is `(via index scan)`, only pid values 11..19 that also satisfy `city = 'PR'` are returned, and the result equals the same statement's heap-path result

#### Scenario: OR degrades to the heap
- **WHEN** an index exists on `pid` and `SELECT * FROM person WHERE pid = 1 OR city = 'MA'` runs
- **THEN** the marker is `(via heap scan)` and the rows are exactly those satisfying the whole predicate

### Requirement: UPDATE applies simultaneously-computed new rows and maintains indexes
The system SHALL execute `UPDATE <table> SET <col> = <expr> ... [WHERE <expr>]` by first collecting every row that matches the WHERE expression together with its current values and row id, then overwriting each collected row with a new row computed from that row's old values — so assignments never observe each other's results — and, for every declared index whose column a row's value changed, erasing the row's old entry and inserting the new one, so lookups and ranges track the update immediately. Rows that do not match SHALL remain unchanged. A collection failure SHALL apply nothing.

#### Scenario: Self-referential assignment
- **WHEN** the user runs `UPDATE person SET age = age + 1 WHERE pid = 1`
- **THEN** the matching row's age increases by exactly one

#### Scenario: Simultaneous assignment semantics
- **WHEN** an UPDATE's SET list assigns `a = b, b = a` on a table with columns `a` and `b`
- **THEN** the two columns exchange values (the swap happens), rather than both ending up with the original value of `b`

#### Scenario: Key-column update moves the index entry
- **WHEN** an index exists on `pid` and `UPDATE person SET pid = 99 WHERE pid = 1` runs
- **THEN** a lookup for 99 finds the row, a lookup for 1 no longer returns it, and index verification reports no findings

#### Scenario: Non-key update leaves the index untouched
- **WHEN** an index exists on `pid` and `UPDATE person SET age = age + 1 WHERE pid = 1` runs
- **THEN** the row's index entry still resolves to the same row id and the index count is unchanged

#### Scenario: No matches leaves data untouched
- **WHEN** an UPDATE's WHERE expression matches no rows
- **THEN** no row changes and the statement succeeds

### Requirement: DELETE removes matching rows and maintains indexes
The system SHALL execute `DELETE FROM <table> [WHERE <expr>]` by first collecting every matching row with its row id, then removing each collected row from the data file and erasing that row's entry from every declared index, so no lookup or range ever returns a deleted row again. Freed space SHALL become reusable by later INSERTs. A DELETE without WHERE SHALL remove every row. A collection failure SHALL erase nothing.

#### Scenario: Conditional delete
- **WHEN** the user runs `DELETE FROM person WHERE age < 18`
- **THEN** exactly the matching rows disappear, other rows remain readable, and index lookups no longer return any deleted row

#### Scenario: Deleted rows leave the index clean
- **WHEN** rows are deleted from an indexed table and index verification runs
- **THEN** no dangling entries are reported

#### Scenario: Freed rows are reusable
- **WHEN** rows are deleted and new rows are inserted afterwards
- **THEN** the inserts succeed and a full scan returns the new rows

### Requirement: Scans fail fast on unreadable records
The system SHALL fail a statement whose scan encounters a record in the data file that violates the table's declared structure, with a named error identifying the failure; it SHALL NOT return partial SELECT results and SHALL NOT apply any collected UPDATE or DELETE when the collection scan fails.

#### Scenario: Corrupt record blocks SELECT
- **WHEN** a SELECT scans a data file containing a record that violates the table's declared structure
- **THEN** the statement fails with a named error and returns no rows

#### Scenario: Corrupt record blocks UPDATE application
- **WHEN** an UPDATE's collection scan encounters a record that violates the table's declared structure before the scan completes
- **THEN** the statement fails with a named error and no row is modified

### Requirement: The wider language is refused, not misparsed
Statements in the grammar but outside the surface — joins, GROUP BY, ORDER BY, aggregate calls, subqueries — SHALL be refused with a named error naming the unsupported feature, and nothing SHALL be executed.

#### Scenario: Unsupported clause named
- **WHEN** `SELECT city, COUNT(*) FROM person GROUP BY city` runs
- **THEN** the response is a named error mentioning GROUP BY and no rows are printed
