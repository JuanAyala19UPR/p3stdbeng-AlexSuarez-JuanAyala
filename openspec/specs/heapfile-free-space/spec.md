# heapfile-free-space

## Purpose

Gives the heap file a header-addressed two-list free-space index — pages with free space, and full pages — so INSERT picks a page in O(1) instead of walking a chain, while erase-driven reuse stays immediate. The lists live in page trailers and five header fields; membership is derived from occupancy, and a structural self-check can verify the whole structure at any time.

## Requirements

### Requirement: Every allocated page sits on exactly one header-addressed list
The heap file's header SHALL carry `free_head`, `free_tail`, `full_head`, and `full_tail` pointers, and every allocated data page SHALL be reachable from exactly one of the two lists. A page's list membership SHALL be derived from its occupancy — a page belongs on the full list if and only if every slot is occupied — and SHALL NOT be stored as separate per-page state. The header SHALL also carry `next_page_id`, the id the next page allocation will use. New pages SHALL be appended at the tails, so each list's order reflects allocation (and re-allocation) order.

#### Scenario: Two-page file, one full one free
- **WHEN** a file holds page 1 (all slots used) and page 2 (one slot used)
- **THEN** the header's `full_head` reaches page 1, `free_head` reaches page 2, and no page is reachable from both lists

#### Scenario: Membership follows occupancy
- **WHEN** the last free slot of a free-list page is filled, and later a slot of that page is erased again
- **THEN** after the fill the page is reachable only from `full_head`, and after the erase only from `free_head`

### Requirement: INSERT picks a page without walking
INSERT SHALL store each row into the free list's head page — the first page in free-list order with a free slot — without visiting any other page. Only when the free list is empty SHALL INSERT allocate a fresh page (id taken from `next_page_id`, which then advances), append it to the free list, and store the row there. Pages SHALL be allocated once and never removed: a page whose records were all erased stays on the free list, and the file never shrinks.

#### Scenario: Bulk load scans in allocation order
- **WHEN** enough rows are inserted to allocate several pages, each filling completely
- **THEN** a full scan returns the rows in insertion order (page 1's rows before page 2's, and so on)

#### Scenario: Erased pages are reused before new pages are allocated
- **WHEN** rows of a multi-page file are deleted until an earlier page has free space, and new rows are inserted
- **THEN** the new rows land in the freed page and the file's page count does not grow

### Requirement: Pages move between lists only on fill or unfill
A page SHALL move from the free list to the full list exactly when an insert occupies its last free slot, and from the full list to the free list exactly when an erase frees a slot on a previously full page. UPDATE SHALL never move a page between lists (records are fixed-width, so occupancy never changes), and find/update/erase SHALL keep addressing records directly by (page, slot).

#### Scenario: Filling a page's last slot moves it to the full list
- **WHEN** an insert occupies the last free slot of the free list's only page, and the table needs another row
- **THEN** the next insert allocates a new page instead of revisiting the full page

#### Scenario: UPDATE leaves list membership alone
- **WHEN** rows of a partially-filled page are updated
- **THEN** the page stays where it was — reachable from the same list head as before the updates

### Requirement: Scans walk the free list, then the full list
A scan SHALL visit the free list's pages in list order, then the full list's pages in list order, returning every occupied slot's tuple. Within one list the order SHALL be the list's order (tail-append keeps it aligned with allocation order); the order across lists and under interleaved mutation SHALL remain unspecified.

#### Scenario: Single-page tables scan exactly as before
- **WHEN** a table's rows all live on one page
- **THEN** a scan returns them in slot order, unchanged from the previous format

#### Scenario: Mixed full and free pages both appear
- **WHEN** a file holds rows on both lists and a scan runs with no concurrent mutation
- **THEN** every row is returned exactly once — no page and no slot is skipped or visited twice

### Requirement: The structure can verify itself
The heap file SHALL offer a structural self-check that walks both lists from the header and reports a named error if the lists are inconsistent: a head or tail pointer that disagrees with the walk, a cycle or an unreachable page, a page visited twice, a page on a list contradicting its occupancy, the header page appearing on a list, the union of the two lists not covering exactly the allocated page ids (1 through next_page_id − 1), or an on-disk data page the walk cannot reach. An empty file (header only) SHALL pass.

#### Scenario: Healthy file passes
- **WHEN** the self-check runs on a file built by ordinary inserts and erases
- **THEN** it succeeds

#### Scenario: A severed link is reported
- **WHEN** a page's list links are corrupted so a page becomes unreachable
- **THEN** the self-check fails with a named error instead of silently losing the page's rows

### Requirement: The TUP2 format refuses foreign files
A heap file SHALL identify itself with magic `TUP2`; opening a file with any other magic SHALL be refused with a named error. No migration path is provided: files written by the previous single-chain format SHALL NOT open.

#### Scenario: Old-format file refused
- **WHEN** a file whose header magic is `TUP1` is opened
- **THEN** the open fails with a named error and the file is left untouched

#### Scenario: Fresh files are TUP2
- **WHEN** a heap file is created and immediately opened
- **THEN** the open succeeds and the file's header reports the TUP2 magic with empty lists and `next_page_id` 1
