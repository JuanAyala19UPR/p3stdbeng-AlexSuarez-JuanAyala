# bulk-loader

## Purpose

A bulk loader that fills an empty heap file with generated records at O(N) page writes — each page composed directly and written to disk exactly once, the TUP2 two-list structure built as pages fill — performing its own raw block I/O with no buffer manager, so bootstrapping a table needs no per-row engine work at all.

## Requirements

### Requirement: Bulk fill of an empty heap file
Given a table schema, a record count N, a record generator, and a data path whose file is missing or empty (no file, or a one-block TUP2 header page with empty lists), the bulk loader SHALL produce a heap file at that path holding exactly the N records the generator returned — performing its own raw block I/O, without a buffer manager. It SHALL fill one slotted page at a time with K = `slot_capacity(record_size)` records, serializing each tuple against the schema, and record each full page on the header's full list in generation order; the final page, when partial, SHALL sit on the free list with the header's `next_page_id` covering every written page. The caller SHALL run the tool before any buffer manager opens the path: the tool writes blocks directly and cannot honor another pool's cached state.

#### Scenario: Load into a missing data file
- **WHEN** the tool runs against a path with no file, a valid schema, N = 250, and a working generator
- **THEN** the file exists afterwards, holds exactly 250 records, and its header page carries the TUP2 identification for the table

#### Scenario: Pages fill to capacity in generation order
- **WHEN** the tool loads N records whose per-page capacity is K and N is an exact multiple of K
- **THEN** every page holds exactly K records in generator order, all pages sit on the full list in generation order, and scanning the file through the engine yields the records in generation order

#### Scenario: Partial final page lands on the free list
- **WHEN** the tool loads N records that do not fill the last page completely
- **THEN** the last page sits on the free list with exactly N − (number of earlier pages × K) records, the earlier pages sit on the full list in generation order, and the heap's structural self-check passes

### Requirement: Engine-equivalent result
The bulk-loaded file SHALL be indistinguishable from a file filled by sequential `HeapFile::insert` calls: it opens through `HeapFile::open` against the same schema, its two lists and page occupancy match what sequential inserts of the same rows produce, `scan` yields the same record sequence and the same row-id sequence as the sequential twin (whatever the scan-order rule makes of the two-list layout), `find` succeeds for every reported row id, a count through the engine equals N, and the heap's structural self-check passes.

#### Scenario: Differential check against per-row insertion
- **WHEN** the same N generated rows are loaded twice — once through the tool, once through per-row inserts — and both files are scanned through the engine
- **THEN** the scans yield identical record sequences and identical row-id sequences, and both files pass the structural self-check

#### Scenario: Row ids resolve
- **WHEN** any row id the tool's load reported (or a scan observed) is fetched with `HeapFile::find`
- **THEN** the fetch returns the record the generator produced for that position

### Requirement: Refusals
The tool SHALL refuse, with a named error and without modifying the file, when: the file already holds records (more than one block, or non-empty lists); the schema has no record layout (no columns, or String columns without declared width); or a record does not fit a 4096-byte page.

#### Scenario: Non-empty file refused
- **WHEN** the tool runs against a data file that already holds records
- **THEN** it fails with a named error and the file's record content is unchanged

#### Scenario: Wrong table or corrupt header refused
- **WHEN** the tool runs against a one-block file whose header is missing the TUP2 identification or names a different table
- **THEN** it fails with a named error

### Requirement: Generator contract
The tool SHALL call the supplied generator exactly N times, in index order 0..N-1, and store each returned tuple as the record at that generation position. A generator failure at index i SHALL fail the load with a named error identifying the index; records already written may remain, and the error does not silence earlier work.

#### Scenario: Generator failure surfaced
- **WHEN** the generator returns a failure at index i < N
- **THEN** the load fails with an error naming index i, and no records beyond position i are added
