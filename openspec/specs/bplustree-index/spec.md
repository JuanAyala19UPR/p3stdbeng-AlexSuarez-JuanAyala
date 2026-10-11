# bplustree-index

## Purpose

Provides the engine's self-balancing access path: a single-column B+tree index that maps key values to heap row ids, built by bulk load, kept logarithmic by splitting on insert and borrowing or merging on erase, with every leaf at the same depth at all times.

## Requirements

### Requirement: Bulk build from table contents
Creating a B+tree index for a table column SHALL index every row currently in the table's data file as a balanced tree built bottom-up from filled leaves. After the build completes, every existing row SHALL be reachable through the index by its key value, index entries SHALL be ordered by key (integers and doubles numerically, strings lexicographically), and every leaf SHALL sit at the same depth.

#### Scenario: Build over populated data file
- **WHEN** a B+tree index is created on column `pid` of a data file holding 520 rows
- **THEN** a lookup for any existing `pid` value returns that row's id, the index reports 520 entries, and verification finds the tree balanced with no findings

#### Scenario: Build over an empty data file
- **WHEN** a B+tree index is created on a data file holding no rows
- **THEN** the index is valid and empty, and every lookup on it returns nothing

### Requirement: Point lookup returns all duplicates
The index is non-unique. A lookup for a key value SHALL return the row ids of every entry whose key equals it, and nothing else — including duplicates that span leaf boundaries.

#### Scenario: Duplicate keys across a leaf boundary
- **WHEN** rows with key 42 occupy the end of one leaf and the start of the next, and the index is looked up for 42
- **THEN** exactly those row ids are returned — both leaves' entries — and no row with a different key appears

### Requirement: Range lookup walks keys in order
A range lookup with bounds derived from `<`, `<=`, `>`, `>=` SHALL return matching entries in ascending key order, duplicates adjacent, regardless of where the rows physically live in the heap file.

#### Scenario: Ascending range
- **WHEN** rows with keys 5, 1, 9, 1 exist and the index is searched for keys greater than 1 and less than 9
- **THEN** the returned row ids are ordered 1, 5 by key, with both key-1 rows adjacent, and key 9 is absent

### Requirement: Insert splits and keeps the tree balanced
After a row is inserted into the data file and its key entered into the index, lookups and ranges SHALL find the new row. Entering an entry into a full leaf (2d entries, d being the node order) SHALL split the leaf — d entries staying, d+1 moving to a new leaf — and copy the new leaf's smallest key up as the separator; a full separator page SHALL split by moving its middle key up; a root split SHALL allocate a new root above both halves and grow the height by exactly one. After any sequence of inserts, every leaf SHALL remain at the same depth, and no page other than the path from the root to the affected leaf SHALL change.

#### Scenario: Leaf split is immediately visible
- **WHEN** enough rows are inserted that the landing leaf splits
- **THEN** a lookup for the inserted key returns its row id, a range across the split boundary returns every matching row in key order, and both halves of the old leaf remain reachable

#### Scenario: Height grows only at a root split
- **WHEN** inserts proceed from an empty index until a separator page first overflows
- **THEN** the height increases by exactly one at that moment, the old root's two halves hang under a fresh root, and every leaf is still at the same depth

#### Scenario: Depth stays uniform under randomized inserts
- **WHEN** thousands of randomly ordered rows are inserted
- **THEN** verification after the sequence reports every leaf at the same depth and no occupancy violations

### Requirement: Erase removes exact entries and rebalances
The index SHALL support erasing one entry by (key, row id): after erasing, lookups and ranges SHALL no longer return that row id for that key, every other entry SHALL remain findable, the reported entry count SHALL decrease by exactly one, and vacated slots SHALL close up — pages keep their entries contiguous and the tree keeps every page within the order rule (between d and 2d entries, where d is half the page's entry capacity rounded down) except the root. When an erase leaves a page underflowed, the tree SHALL restore the invariant by moving an entry from an adjacent sibling (borrow, rewriting the separator between them) or by merging the page with a sibling and dropping the separator from the parent; an underflow at the parent's level SHALL propagate the same way; a root left with a single child SHALL be collapsed, shrinking the height by exactly one (the root alone is exempt from the order rule). Erasing a (key, row id) pair that is not present SHALL fail with a named error and change nothing.

#### Scenario: Erased entry disappears from every path
- **WHEN** three rows share key 42 and one is erased by (42, its row id)
- **THEN** a lookup for 42 returns exactly the two remaining row ids, the count is one lower, and a range covering 42 excludes the erased row

#### Scenario: Underflow is resolved by borrowing
- **WHEN** an erase leaves a leaf below its minimum while an adjacent sibling holds more than its minimum
- **THEN** an entry moves from that sibling through their separator, the separator is rewritten to stay exact, the height is unchanged, and verification finds no occupancy violations

#### Scenario: Merge collapses a level when siblings cannot lend
- **WHEN** an erase leaves a leaf below its minimum while both adjacent siblings sit at their minimum
- **THEN** the leaf merges with one sibling, the parent drops the separator and the child pointer, the height shrinks only when the collapse reaches the root, and verification finds the tree balanced and clean

#### Scenario: Root collapses to keep the height honest
- **WHEN** erases empty a tree down to a root whose separator level holds a single child
- **THEN** the root is collapsed, the height decreases by exactly one, and lookups still answer correctly

#### Scenario: Erasing an absent entry fails
- **WHEN** erase is called with a (key, row id) pair the index does not hold
- **THEN** the call fails with a named error and the entry count is unchanged

### Requirement: Persistence and self-identification
A B+tree index file SHALL survive close and reopen with all entries and its balance intact. An index file SHALL identify its kind by its own magic: opening an index file SHALL verify the kind matches the index type the caller declares, and that its recorded table name, column name, and key type match the caller's schema — a foreign file or any mismatch, including an ISAM file opened as a B+tree, SHALL fail with a named error and never yield entries.

#### Scenario: Reopen preserves entries and balance
- **WHEN** a session closes a B+tree index file and a later session opens it again
- **THEN** lookups and ranges return the same answers as before the close, and verification is still clean

#### Scenario: Wrong table refused
- **WHEN** a B+tree file built for table `person` is opened declaring table `car`
- **THEN** the open fails with a named error and no entries are returned

#### Scenario: Wrong kind refused
- **WHEN** an ISAM index file is opened declaring itself a B+tree (or the reverse)
- **THEN** the open fails with a named error and no entries are returned

### Requirement: Integrity verification checks structure, balance, and exactness
The index SHALL support a verification pass that walks the whole tree from the header's root and reports, with named diagnostics: a page reachable twice or not at all, key-order violations within a page or across the leaf chain, a page whose occupancy violates the order rule — fewer than d or more than 2d entries (other than the root) — a separator that does not equal the first key of the child it routes to, a leaf whose depth differs from another leaf's, entries whose row id does not resolve to an occupied slot in the data file (dangling, reported by key and row id), and a walked total that disagrees with the header. A clean tree verifies with no findings.

#### Scenario: Dangling entry detected
- **WHEN** the tree holds an entry whose row id names a free heap slot
- **THEN** verification reports that entry by key and row id, and exits non-clean

#### Scenario: Imbalance or stale separator detected
- **WHEN** a leaf's depth is corrupted, or a separator no longer equals its child's first key
- **THEN** verification reports the finding by name and exits non-clean

#### Scenario: Clean tree verified
- **WHEN** verification runs on a tree whose every entry matches an occupied slot and whose structure was maintained only by build, insert, and erase
- **THEN** no findings are reported
