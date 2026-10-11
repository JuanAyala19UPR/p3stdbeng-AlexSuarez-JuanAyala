# isam-index

## Purpose

Provides the engine's first access path above the heap file: a single-column ISAM index that maps key values to heap row ids, built by bulk load, maintained by insert and erase, and never rebalanced.

## Requirements

### Requirement: Bulk build from table contents
Creating an index for a table column SHALL index every row currently in the table's data file. After the build completes, every existing row SHALL be reachable through the index by its key value, and index entries SHALL be ordered by key (integers and doubles numerically, strings lexicographically).

#### Scenario: Build over populated data file
- **WHEN** an index is created on column `pid` of a data file holding 520 rows
- **THEN** a lookup for any existing `pid` value returns that row's id, and the index reports 520 entries

#### Scenario: Build over an empty data file
- **WHEN** an index is created on a data file holding no rows
- **THEN** the index is valid and empty, and every lookup on it returns nothing

### Requirement: Point lookup returns all duplicates
The index is non-unique. A lookup for a key value SHALL return the row ids of every entry whose key equals it, and nothing else.

#### Scenario: Duplicate keys
- **WHEN** three rows share key value 42 and the index is looked up for 42
- **THEN** exactly those three row ids are returned, and no row with a different key appears

### Requirement: Range lookup walks keys in order
A range lookup with bounds derived from `<`, `<=`, `>`, `>=` SHALL return matching entries in ascending key order, duplicates adjacent, regardless of where the rows physically live in the heap file.

#### Scenario: Ascending range
- **WHEN** rows with keys 5, 1, 9, 1 exist and the index is searched for keys greater than 1 and less than 9
- **THEN** the returned row ids are ordered 1, 5 by key, with both key-1 rows adjacent, and key 9 is absent

### Requirement: Insert maintenance without rebalancing
After a row is inserted into the data file and its key entered into the index, lookups and ranges SHALL find the new row. When the target leaf page is full the new entry SHALL spill into that leaf's overflow chain. Index structure built before the insert SHALL remain in place: inserts SHALL NOT add separator levels, split leaves, or reorder existing pages.

#### Scenario: Insert is immediately visible
- **WHEN** a row with key 77 is inserted after the index was built
- **THEN** a lookup for 77 returns the new row id and a range covering 77 includes it

#### Scenario: Overflow keeps answers correct
- **WHEN** enough rows sharing one leaf's key neighborhood are inserted that its entries exceed one page
- **THEN** lookups and ranges still return every matching row, including the spilled ones

### Requirement: Erase removes exact entries in place
The index SHALL support erasing one entry by (key, row id): after erasing, lookups and ranges SHALL no longer return that row id for that key, while every other entry SHALL remain findable, and the reported entry count SHALL decrease by exactly one. Erase SHALL NOT restructure the index: no separator level changes, no leaf-chain relinking, no page removal — the erased position closes by shifting the page's remaining entries forward, so a page's entries stay contiguous and sorted, and an overflow page emptied by erases stays in its leaf's chain. Erasing a (key, row id) pair that is not present SHALL fail with a named error and change nothing.

#### Scenario: Erased entry disappears from every path
- **WHEN** three rows share key 42 and one is erased by (42, its row id)
- **THEN** a lookup for 42 returns exactly the two remaining row ids, the count is one lower, and a range covering 42 excludes the erased row

#### Scenario: The structure is byte-identical around the erased entry
- **WHEN** an entry is erased from a leaf or overflow page
- **THEN** the page's remaining entries are contiguous and in key order, every separator page is byte-identical to before the erase, the leaf chain is unchanged, and verification reports no findings

#### Scenario: An emptied overflow page stays in its chain
- **WHEN** every entry of one overflow page is erased
- **THEN** the page remains in its leaf's overflow chain with zero entries, and a later insert into that leaf may fill it again

#### Scenario: Erasing an absent entry fails
- **WHEN** erase is called with a (key, row id) pair the index does not hold
- **THEN** the call fails with a named error and the entry count is unchanged

### Requirement: Persistence and self-identification
An index file SHALL survive close and reopen with all entries intact. Opening an index file SHALL verify it is an index file and that its recorded table name, column name, and key type match what the caller declares; any mismatch or foreign file SHALL fail with a named error and never yield entries.

#### Scenario: Reopen preserves entries
- **WHEN** a session closes an index file and a later session opens it again
- **THEN** lookups and ranges return the same answers as before the close

#### Scenario: Wrong table refused
- **WHEN** an index file built for table `person` is opened declaring table `car`
- **THEN** the open fails with a named error and no entries are returned

### Requirement: Integrity verification
The index SHALL support a verification pass that walks every entry and reports, with named diagnostics: entries whose row id does not resolve to an occupied slot in the data file (dangling), and any sequence violations of key order. A clean index verifies with no findings.

#### Scenario: Dangling entry detected
- **WHEN** an index holds an entry whose row id names a free heap slot
- **THEN** verification reports that entry by key and row id, and exits non-clean

#### Scenario: Clean index verified
- **WHEN** verification runs on an index whose every entry matches an occupied slot
- **THEN** no findings are reported
