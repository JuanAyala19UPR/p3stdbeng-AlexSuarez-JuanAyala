#pragma once

// BPlusTree — a single-column B+tree index over one heap file, stored in
// its own file of 4096-byte blocks. The lecture's dynamic tree index
// (after Ramakrishna & Gehrke): sorted leaf pages chained DOUBLY hold the
// data entries; separator pages in the |P0| K1 | P1 | ... | Km | Pm| form
// route descents; every node keeps the order-d shape (d <= m <= 2d keys,
// the root exempt). Splits on insert and borrow-or-merge on erase keep
// every leaf at the same depth; height changes only at the root.
//
// Block 0 is a header page: it identifies the file (magic BPT1, key type
// and width, table and column names) and holds the root, first leaf,
// level count, entry count, highest allocated block, and the head of the
// free-block list (pages freed by merges are reused before new blocks
// are allocated). The index never reads or writes heap records; callers
// hand in values and row addresses, and verify() judges rids through a
// caller-supplied occupancy callback.
//
// One encoding: keys move through KeyCodec only, entries through the
// BptPages views only. Errors follow the house convention — bool plus a
// cleared error out-parameter carrying a named message.
//
// The recursion contract (the lecture's own algorithm shapes): every
// public operation is a thin wrapper; the work happens in the private
// recursive auxiliaries below, whose extra parameters carry state up the
// unwinding exactly as the lecture's (newNode, newKey) out-params do.

#include "bufpool/BufferManager.h"
#include "index/BptPages.h"
#include "index/Entries.h"
#include "index/KeyCodec.h"
#include "schema/TableSchema.h"
#include "tuple/Column.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bptree {

class BPlusTree {
public:
    // Binds the index to a buffer manager, a table's schema, and the one
    // column it orders. The schema must outlive the index.
    BPlusTree(bufman::BufferManager& manager,
              const schema::TableSchema& schema, std::size_t column_index);

    // BPlusTree::is_open — the file's open state. Parameters: none.
    // Returns: true between a successful create/build/open and close.
    bool is_open() const { return open_; }
    // BPlusTree::entry_count — the header's entry count. Parameters:
    // none. Returns: how many entries the index currently holds.
    std::size_t entry_count() const { return entry_count_; }
    // BPlusTree::codec — the bound column's key codec. Parameters: none.
    // Returns: a reference to the codec (its width and comparison order
    // govern every encoded key the caller hands in).
    const isam::KeyCodec& codec() const { return codec_; }

    // Creates a fresh, empty index file at `path` (header block only)
    // and leaves it open. Refuses an existing file that already holds
    // blocks, and a column with no record-able key width.
    bool create(const std::string& path, std::string& error);

    // Bulk build: a fresh file filled from `entries` in one pass —
    // sorted in memory, leaves filled left to right linking the doubly
    // linked chain, separator levels built bottom-up until one page (the
    // root) holds a level. Leaves the index open. Refuses an existing
    // non-empty file.
    bool build(const std::string& path,
               const std::vector<std::pair<tuple::Column, idx::Rid>>& entries,
               std::string& error);

    // Opens an existing index file, validating the magic, key type and
    // width, and the table and column names against the bound schema. A
    // file of another index kind (an ISAM file, say) is refused by name.
    bool open(const std::string& path, std::string& error);

    // Flushes and closes this index's file through the shared manager.
    bool close(std::string& error);

    // All entries whose key equals `key` (duplicates contiguous by key;
    // the order within the run is unspecified). The descent follows the
    // lecture's routing; boundary duplicates left of the landing leaf
    // are gathered through the prev links.
    bool lookup(const tuple::Column& key, std::vector<idx::Rid>& out,
                std::string& error);

    // Entries with lo <= key <= hi (bounds optional, inclusivity per
    // bound), in ascending key order, duplicates adjacent. Walks the
    // leaf chain from the leftmost leaf that may hold `lo`.
    bool range(const std::optional<RangeBound>& lo,
               const std::optional<RangeBound>& hi,
               std::vector<idx::Rid>& out, std::string& error);

    // Enters one (key, rid) pair: descends by the lecture's routing,
    // inserts at the leaf, and splits full pages upward — a full leaf
    // keeps d entries and gives d+1 to a new leaf (its smallest key
    // copied up), a full separator page moves its middle key up, and a
    // root split allocates a new root (the height grows by one).
    bool insert(const tuple::Column& key, idx::Rid rid, std::string& error);

    // Removes the one entry equal to (`key`, `rid`): the exact entry is
    // located first (the landing leaf plus the equal-key run through the
    // leaf links), then removed; underflowed pages borrow from a sibling
    // or merge with one, cascading upward, and a root left with a single
    // child is dropped. A (key, rid) pair the index does not hold is a
    // named error with nothing changed.
    bool erase(const tuple::Column& key, idx::Rid rid, std::string& error);

    // Walks the whole tree and reports named findings: a page reachable
    // twice or not at all, key-order violations within a page or across
    // the leaf chain, an occupancy violation of the order rule (other
    // than the root), a separator that does not equal the first key of
    // the child it routes to, a leaf whose depth differs from another
    // leaf's, entries whose rid fails `occupied` (dangling), and a total
    // that disagrees with the header. `entry_count` receives the walked
    // count.
    bool verify(const std::function<bool(const idx::Rid&)>& occupied,
                std::size_t& entry_count, std::vector<std::string>& findings,
                std::string& error);

private:
    // Header block (block 0) fields.
    struct Header {
        std::uint32_t magic = 0;
        std::uint32_t key_type = 0;
        std::uint32_t key_width = 0;
        std::uint32_t root = idx::kNoPage;
        std::uint32_t first_leaf = idx::kNoPage;
        std::uint32_t levels = 1;
        std::uint32_t last_block = idx::kNoPage;
        std::uint32_t freed_head = idx::kNoPage;
        std::uint64_t entry_count = 0;
        std::string table_name;
        std::string column_name;
    };

    bool write_header(std::string& error);
    bool read_header(Header& header, std::string& error);

    // Pins a block and binds the requested view; unpins on failure.
    bool pin_leaf(std::uint32_t block, BptLeafPage& page, std::size_t& frame,
                  std::string& error);
    bool pin_separator(std::uint32_t block, BptSeparatorPage& page,
                       std::size_t& frame, std::string& error);

    // Allocates the next data block: the free list's head when one was
    // freed by a merge, otherwise one past `last_block_`.
    bool alloc_next_block(std::uint32_t& block, std::size_t& frame,
                          std::string& error);
    // Chains a merged-away block onto the free list; the caller holds
    // the block pinned through the surgery and releases it dirty.
    void chain_freed(std::uint32_t block, char* buffer);

    // The order-d bounds (design D3): d = floor(capacity / 2), so a full
    // node holds exactly 2d entries.
    std::size_t min_leaf_entries() const;
    std::size_t max_leaf_entries() const;
    std::size_t min_separator_keys() const;
    std::size_t max_separator_keys() const;

    // --- the recursion contract (design D1) --------------------------------

    // The lecture's treeSearch (slide 24): pin one separator, route by
    // the page's keys, recurse; zero levels left is the landing leaf.
    std::uint32_t tree_search(std::uint32_t block,
                              std::uint32_t levels_remaining,
                              const std::string& key_bytes,
                              std::string& error);

    // The upward carrier of the lecture's insertAux (slides 34-37): the
    // new right page and the separator that routes to it.
    struct Split {
        std::string separator_key;
        std::uint32_t new_block = idx::kNoPage;
    };
    bool insert_aux(std::uint32_t block, std::uint32_t levels_remaining,
                    const std::string& key_bytes, idx::Rid rid,
                    Split& split, std::string& error);

    // The upward carrier of the erase cascade (slides 38-49): whether
    // this page now holds fewer than d entries, and whether its
    // subtree's smallest key changed (so a parent separator needs
    // rewriting).
    struct EraseUp {
        bool underflow = false;
        bool first_changed = false;
        std::string new_first;
    };
    bool erase_rec(std::uint32_t block, std::uint32_t levels_remaining,
                   const std::string& key_bytes, idx::Rid rid,
                   EraseUp& up, std::string& error);

    // One verify walk: structure, occupancy, separator exactness, depth
    // uniformity, per-page and chain order. Returns the subtree's
    // smallest key (empty `has_min` at the root's left edge).
    struct VerifyWalk {
        const std::function<bool(const idx::Rid&)>& occupied;
        std::vector<std::string>& findings;
        std::size_t& walked;
        std::uint32_t min_depth = 0;
        std::uint32_t max_depth = 0;
        std::vector<char> seen;
    };
    bool verify_rec(std::uint32_t block, std::uint32_t depth, bool is_root,
                    VerifyWalk& walk, std::string& error,
                    std::optional<std::string>* subtree_min = nullptr);

    // Walks the leaf chain leftward from `landing` while the previous
    // leaf's last key can still equal the target (the prev-step that
    // closes boundary duplicates); returns the run's leftmost leaf.
    std::uint32_t run_start(std::uint32_t landing,
                            const std::string& key_bytes, std::string& error);
    // Scans the equal-key run left to right from `start`, collecting
    // every entry whose key equals the target.
    bool gather_run(const std::string& key_bytes, std::uint32_t start,
                    std::vector<idx::Rid>& out, std::string& error);
    // Locates the exact (key, rid) within the run; reports the holding
    // leaf and position, or a named error.
    bool locate_in_run(const std::string& key_bytes, idx::Rid rid,
                       std::uint32_t start, std::uint32_t& leaf_block,
                       std::size_t& pos, std::string& error);

    // The insert and erase leaf phases and the underflow repairs.
    bool insert_leaf(std::uint32_t block, const std::string& key_bytes,
                     idx::Rid rid, Split& split, std::string& error);
    bool erase_leaf(std::uint32_t block, const std::string& key_bytes,
                    idx::Rid rid, EraseUp& up, std::string& error);
    bool fix_child_underflow(std::uint32_t block, std::uint32_t levels_remaining,
                             std::size_t child_index, EraseUp& up,
                             std::string& error);
    std::size_t quick_count(std::uint32_t block, bool leaf,
                            std::string& error);
    bool borrow_from_left(std::uint32_t block, std::size_t child_index,
                          std::uint32_t child_block, std::uint32_t left_block,
                          bool child_is_leaf, std::size_t child_count,
                          EraseUp& up, std::string& error);
    bool borrow_from_right(std::uint32_t block, std::size_t child_index,
                           std::uint32_t child_block,
                           std::uint32_t right_block, bool child_is_leaf,
                           std::size_t child_count, EraseUp& up,
                           std::string& error);
    bool merge_child(std::uint32_t block, std::size_t child_index,
                     std::uint32_t child_block, std::uint32_t left_block,
                     std::uint32_t right_block, bool child_is_leaf,
                     std::size_t child_count, EraseUp& up, std::string& error);
    // The crossed-subtree erase's simple correct repair: rebuild from
    // the leaf chain.
    bool rebuild_from_leaves(std::string& error);

    bufman::BufferManager& manager_;
    const schema::TableSchema& schema_;
    std::size_t column_index_ = 0;
    isam::KeyCodec codec_;

    bufman::File file_{};
    std::string path_;
    bool open_ = false;

    std::uint32_t root_ = idx::kNoPage;
    std::uint32_t first_leaf_ = idx::kNoPage;
    std::uint32_t levels_ = 1;
    std::uint32_t last_block_ = idx::kNoPage;
    std::uint32_t freed_head_ = idx::kNoPage;
    std::size_t entry_count_ = 0;
};

}
