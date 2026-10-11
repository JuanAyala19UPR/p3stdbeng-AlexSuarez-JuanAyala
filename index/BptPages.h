#pragma once

// BptPages — the B+tree's own page views, mirroring the SlottedPage
// discipline the rest of the engine uses: non-owning views bound to a
// pinned frame's buffer, format/attach the only writers of structural
// fields, attach validating stored counts against capacity. The module
// owns its views outright (the tear-out rule): nothing here is borrowed
// from ISAM's IndexPages at the type level.
//
// Two castes, per the lecture:
//
//   BptLeafPage      data entries — sorted (key, rid) arrays, chained
//                    DOUBLY (prev and next, slides 22 and 35)
//   BptSeparatorPage index entries — the |P0| K1 | P1 | ... | Km | Pm |
//                    form (slide 10), with the routing rule in one place

#include "index/Entries.h"
#include "index/KeyCodec.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bptree {

// 4096-byte block size, taken from the buffer pool's frame size.
constexpr std::size_t kBlockSize = 4096;

// One end of a range: a key value plus whether the bound includes it.
struct RangeBound {
    tuple::Column value;
    bool inclusive = true;
};

// Leaf page: count u32 + prev-leaf u32 + next-leaf u32 + entries.
std::size_t leaf_capacity(std::size_t key_width);
// Separator page: count u32 + first-child u32 + (key + child) pairs.
std::size_t separator_capacity(std::size_t key_width);

// --- BptLeafPage: a data page of the B+tree ---------------------------------

class BptLeafPage {
public:
    void bind(char* buffer, std::size_t key_width);

    // format: zero entries, chain links as given.
    void format(std::uint32_t prev_leaf, std::uint32_t next_leaf);
    // Stored count must fit the capacity: corrupted or foreign bytes are
    // a hard error, exactly like SlottedPage::attach.
    bool attach(std::string& error) const;

    std::size_t count() const;
    // BptLeafPage::capacity — the entry capacity this page was bound
    // with. Parameters: none. Returns: the fixed entry capacity
    // (leaf_capacity of the bind-time key width).

    std::uint32_t prev_leaf() const;
    void set_prev_leaf(std::uint32_t page_id);
    std::uint32_t next_leaf() const;
    void set_next_leaf(std::uint32_t page_id);

    // First entry index whose key is >= `key` (binary search; `key` is
    // encoded, exactly codec.width() bytes — a raw pointer, because
    // encoded keys hold embedded NULs and a char*->string conversion
    // would truncate at the first one).
    std::size_t lower_bound(const char* key, const isam::KeyCodec& codec) const;
    // The data entry at slot i. Throws std::out_of_range for i >= count()
    // (the container-like convention).
    idx::DataEntry get_entry(std::size_t i) const;
    // Sorted insert: memmoves the tail to keep order. Refuses a full page
    // with a named error; never writes a half-entry.
    bool insert_entry(const idx::DataEntry& entry, const isam::KeyCodec& codec,
                      std::string& error);
    // The caller-found position's entry is removed: the entries after it
    // shift one slot forward, so the page stays contiguous and sorted and
    // the count drops by one. Refuses an out-of-range position; nothing
    // else can fail, and no half-shift is ever written.
    bool erase_entry(std::size_t i, std::string& error);
    // Moves this page's entries from position `from` to the end into a
    // freshly formatted page view (the split's right half). The source
    // count drops to `from`. Both views must be bound and the source
    // must hold more entries than `from`.
    void move_tail_to(std::size_t from, BptLeafPage& destination,
                      std::size_t key_width);

private:
    // The stored count is a 32-bit field; every read and write funnels
    // through these two so the width can never drift.
    std::size_t raw_count() const;
    void write_count(std::size_t count);

    // BptLeafPage::entry_offset — the byte offset of entry i: the page
    // header, then i slots of (key + rid). Parameters: i — the entry
    // position. Returns: the offset; the caller guarantees the bounds.
    std::size_t entry_offset(std::size_t i) const {
        return kEntriesOffset + i * (key_width_ + 8);
    }

    static constexpr std::size_t kCountOffset = 0;
    static constexpr std::size_t kPrevOffset = 4;
    static constexpr std::size_t kNextOffset = 8;
    static constexpr std::size_t kEntriesOffset = 12;

    char* buffer_ = nullptr;
    std::size_t key_width_ = 0;
    std::size_t capacity_ = 0;
};

// --- BptSeparatorPage: an index page (internal) ------------------------------

class BptSeparatorPage {
public:
    void bind(char* buffer, std::size_t key_width);

    // format: zero separators, first child = kNoPage.
    void format();
    bool attach(std::string& error) const;

    std::size_t count() const;
    // BptSeparatorPage::capacity — the pair capacity this page was bound
    // with. Parameters: none. Returns: the fixed pair capacity
    // (separator_capacity of the bind-time key width).

    std::uint32_t child(std::size_t i) const;      // child routed by separator i
    const char* separator_key(std::size_t i) const;
    // Rewrites separator i's key bytes (the caller-encoded fixed width).
    void set_separator_key(std::size_t i, const char* key);
    // The i-th index entry — the named form of the (key, child) pair.
    idx::IndexEntry entry(std::size_t i) const;
    std::uint32_t first_child() const;             // the leftmost child
    void set_first_child(std::uint32_t page_id);

    // Build-time writer: appends one index entry in ascending key order.
    // Refuses a full page.
    bool append(const idx::IndexEntry& index_entry, std::string& error);

    // The lecture's routing rule in one place (slides 10 and 24): P0
    // leads to keys < K1, Pi to keys in [Ki, Ki+1), Pm to keys >= Km —
    // the position of the child after the LAST separator <= `key`
    // (0 routes to the first child, i to the child pair i-1 routes to).
    std::size_t routing_child_index(const char* key, const isam::KeyCodec& codec) const;

    // Inserts one (key, child) pair so the new child sits right after
    // the child currently at position `after_child` (0 inserts before
    // the first separator, i inserts between separator i and its child).
    // Refuses a full page with a named error.
    bool insert_pair_after(std::size_t after_child, const idx::IndexEntry& pair,
                           std::string& error);
    // Drops the separator at position i; the child it routed to merges
    // away at the caller's discretion (the pair's child pointer is
    // forgotten — the caller has already re-homed the children).
    void erase_pair(std::size_t i, std::size_t key_width);
    // Moves this page's pairs from position `from` to the end into a
    // freshly formatted separator view (the split's right half); the
    // caller sets the right half's first child (the child pair `from - 1`
    // routes to, or the carried middle pair's child on an insert split).
    // The source count drops to `from`.
    void move_tail_to(std::size_t from, BptSeparatorPage& destination,
                      std::size_t key_width);

private:
    std::size_t raw_count() const;
    void write_count(std::size_t count);

    // BptSeparatorPage::entry_offset — the byte offset of pair i: the
    // page header, then i slots of (key + child). Parameters: i — the
    // pair position. Returns: the offset; the caller guarantees the
    // bounds.
    std::size_t entry_offset(std::size_t i) const {
        return kPairsOffset + i * (key_width_ + 4);
    }

    static constexpr std::size_t kCountOffset = 0;
    static constexpr std::size_t kFirstChildOffset = 4;
    static constexpr std::size_t kPairsOffset = 8;

    char* buffer_ = nullptr;
    std::size_t key_width_ = 0;
    std::size_t capacity_ = 0;
};

}
