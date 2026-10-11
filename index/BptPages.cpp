#include "index/BptPages.h"

#include <cstring>
#include <stdexcept>

namespace bptree {
// leaf_capacity — the number of data entries one leaf page can hold for
// a given key width: the 4096-byte block minus the count/prev/next leaf
// header, one entry per (key + rid) slot.
//
// Parameters:
//   key_width: the codec's encoded key width in bytes (4 for Integer,
//     8 for Double, the column's declared size for String).
//
// Returns: the entry capacity; always at least 2d with d = capacity / 2.
std::size_t leaf_capacity(std::size_t key_width) {
    return (kBlockSize - 12) / (key_width + 8);
}
// separator_capacity — the number of (key, child) pairs one separator
// page can hold for a given key width: the block minus the count and
// first-child fields.
//
// Parameters:
//   key_width: the codec's encoded key width in bytes.
//
// Returns: the pair capacity; always at least 2d with d = capacity / 2.
std::size_t separator_capacity(std::size_t key_width) {
    return (kBlockSize - 8) / (key_width + 4);
}

// --- BptLeafPage -------------------------------------------------------------
// BptLeafPage::raw_count — reads the 32-bit count field raw, so the
// entry count's on-disk width can never drift to the platform size_t.
//
// Parameters: none.
//
// Returns: the stored count (4-byte field widened).
std::size_t BptLeafPage::raw_count() const {
    std::uint32_t count = 0;
    std::memcpy(&count, buffer_ + kCountOffset, sizeof(count));
    return count;
}
// BptLeafPage::write_count — writes the count field as a 32-bit value,
// the single funnel every count mutation goes through.
//
// Parameters:
//   count: the new entry count; must fit the capacity (unchecked here —
//     the callers already validated).
//
// Returns: nothing; the field reads back as `count`.
void BptLeafPage::write_count(std::size_t count) {
    const std::uint32_t raw = static_cast<std::uint32_t>(count);
    std::memcpy(buffer_ + kCountOffset, &raw, sizeof(raw));
}
// BptLeafPage::bind — attaches the view to a 4096-byte page buffer
// (a pinned frame's memory) at the given key width. Non-owning: the
// buffer must outlive the view.
//
// Parameters:
//   buffer: the page bytes; not zeroed, not owned.
//   key_width: the encoded key width; fixes the entry stride and the
//     capacity.
//
// Returns: nothing. Every accessor afterwards reads through the view.
void BptLeafPage::bind(char* buffer, std::size_t key_width) {
    buffer_ = buffer;
    key_width_ = key_width;
    capacity_ = leaf_capacity(key_width);
}
// BptLeafPage::format — initializes the page as a valid empty leaf: zero
// entries, the given chain links. The only writer of structural state
// from nothing; the caller must hold the frame's pin.
//
// Parameters:
//   prev_leaf: the left neighbour's block id, or kNoPage.
//   next_leaf: the right neighbour's block id, or kNoPage.
//
// Returns: nothing; the page reads back as count 0 with the given links.
void BptLeafPage::format(std::uint32_t prev_leaf, std::uint32_t next_leaf) {
    std::memset(buffer_, 0, kBlockSize);
    auto write_u32 = [&](std::size_t offset, std::uint32_t value) {
        std::memcpy(buffer_ + offset, &value, 4);
    };
    write_u32(kCountOffset, 0);
    write_u32(kPrevOffset, prev_leaf);
    write_u32(kNextOffset, next_leaf);
}
// BptLeafPage::attach — validates the stored state against the capacity:
// the count field must not claim more entries than the page can hold.
// Corrupted bytes or a foreign page format surface here, named.
//
// Parameters:
//   error: cleared, then set on failure with the count and the capacity.
//
// Returns: true when the page's stored state is internally possible;
// false with a named error when the count exceeds the capacity.
bool BptLeafPage::attach(std::string& error) const {
    const std::size_t count = raw_count();
    if (count > capacity_) {
        error = "leaf page holds " + std::to_string(count) +
                " entries; at most " + std::to_string(capacity_) +
                " fit a " + std::to_string(kBlockSize) + "-byte page";
        return false;
    }
    return true;
}
// BptLeafPage::count — the number of entries on the page.
//
// Parameters: none.
//
// Returns: the stored entry count.
std::size_t BptLeafPage::count() const { return raw_count(); }
// BptLeafPage::prev_leaf — the doubly linked chain's left link: the
// left neighbour's block id, or kNoPage at the chain's first leaf.
//
// Parameters: none.
//
// Returns: the stored prev-leaf block id.
std::uint32_t BptLeafPage::prev_leaf() const {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer_ + kPrevOffset, 4);
    return value;
}
// BptLeafPage::set_prev_leaf — rewrites the chain's left link (the
// merge's chain closure and the split's fresh leaf use it).
//
// Parameters:
//   page_id: the new left neighbour's block id, or kNoPage.
//
// Returns: nothing.
void BptLeafPage::set_prev_leaf(std::uint32_t page_id) {
    std::memcpy(buffer_ + kPrevOffset, &page_id, 4);
}
// BptLeafPage::next_leaf — the doubly linked chain's right link: the
// right neighbour's block id, or kNoPage at the chain's last leaf.
//
// Parameters: none.
//
// Returns: the stored next-leaf block id.
std::uint32_t BptLeafPage::next_leaf() const {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer_ + kNextOffset, 4);
    return value;
}
// BptLeafPage::set_next_leaf — rewrites the chain's right link (the
// split's fresh leaf and the merge's closure use it).
//
// Parameters:
//   page_id: the new right neighbour's block id, or kNoPage.
//
// Returns: nothing.
void BptLeafPage::set_next_leaf(std::uint32_t page_id) {
    std::memcpy(buffer_ + kNextOffset, &page_id, 4);
}
// BptLeafPage::lower_bound — the first entry index whose key compares
// >= `key` (binary search over the sorted entries). The parameter is a
// RAW pointer on purpose: encoded keys hold embedded NULs and a
// char*->string conversion would truncate at the first one.
//
// Parameters:
//   key: the encoded target key, exactly codec.width() bytes.
//   codec: the key codec; supplies the comparison order.
//
// Returns: an index in 0..count() — the insertion point that keeps the
// page sorted; count() when every key compares less.
std::size_t BptLeafPage::lower_bound(const char* key,
const isam::KeyCodec& codec) const {
    std::size_t lo = 0;
    std::size_t hi = count();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const char* mid_key = buffer_ + entry_offset(mid);
        if (codec.compare(mid_key, key) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}
// BptLeafPage::get_entry — the data entry at slot i in its in-flight
// form (decoded-width key string + rid).
//
// Parameters:
//   i: the entry position; 0 <= i < count().
//
// Returns: the (key, rid) pair. Throws std::out_of_range when i is past
// the end — the container-like convention.
idx::DataEntry BptLeafPage::get_entry(std::size_t i) const {
    if (i >= count()) {
        throw std::out_of_range("leaf entry index out of range");
    }
    idx::DataEntry entry;
    entry.key.assign(buffer_ + entry_offset(i), key_width_);
    std::memcpy(&entry.rid.page_id, buffer_ + entry_offset(i) + key_width_, 4);
    std::memcpy(&entry.rid.slot, buffer_ + entry_offset(i) + key_width_ + 4, 4);
    return entry;
}
// BptLeafPage::insert_entry — sorted insert: the entry lands at its
// lower_bound position, the tail memmoves to keep order, and the count
// grows by one. Nothing is half-written: a refusal is the only failure.
//
// Parameters:
//   entry: the (encoded key, rid) pair; the key is already
//     codec.width() bytes.
//   codec: the key codec; supplies the comparison order.
//   error: cleared, then set on failure with the capacity message.
//
// Returns: true on success; false with a named error when the page is
// already full (count == capacity) — the caller splits instead.
bool BptLeafPage::insert_entry(const idx::DataEntry& entry, const isam::KeyCodec& codec,
std::string& error) {
    const std::size_t n = count();
    if (n >= capacity_) {
        error = "leaf page is full (" + std::to_string(capacity_) +
                " entries)";
        return false;
    }
    // The entry's key arrives already encoded to the codec's fixed
    // width; the entry is written in one pass after the tail memmove,
    // so a refusal (above) is the only failure and nothing is
    // half-written.
    const std::size_t pos = lower_bound(entry.key.data(), codec);
    const std::size_t stride = key_width_ + 8;
    if (pos < n) {
        std::memmove(buffer_ + entry_offset(pos + 1),
                     buffer_ + entry_offset(pos), (n - pos) * stride);
    }
    char* raw = buffer_ + entry_offset(pos);
    std::memcpy(raw, entry.key.data(), key_width_);
    std::memcpy(raw + key_width_, &entry.rid.page_id, 4);
    std::memcpy(raw + key_width_ + 4, &entry.rid.slot, 4);
    write_count(n + 1);
    return true;
}
// BptLeafPage::erase_entry — removes the entry at position i: the
// entries after it shift one slot forward (tail-shift), so the page
// stays contiguous and sorted and the count drops by one. The vacated
// slot closes up — unlike the ISAM leaf, no gap remains.
//
// Parameters:
//   i: the caller-found position; 0 <= i < count().
//   error: cleared, then set on failure naming the page and count.
//
// Returns: true on success (nothing can fail after the bounds check —
// no half-shift is ever written); false with a named error when i is
// out of range.
bool BptLeafPage::erase_entry(std::size_t i, std::string& error) {
    const std::size_t n = count();
    if (i >= n) {
        error = "entry index out of range (leaf holds " + std::to_string(n) +
                " entries)";
        return false;
    }
    // Symmetric with insert_entry: one memmove closes the position, then
    // the count drops — nothing can fail in between, so the page is
    // never left half-shifted.
    const std::size_t stride = key_width_ + 8;
    if (i + 1 < n) {
        std::memmove(buffer_ + entry_offset(i),
                     buffer_ + entry_offset(i + 1), (n - i - 1) * stride);
    }
    write_count(n - 1);
    return true;
}
// BptLeafPage::move_tail_to — the split's right half: every entry from
// position `from` to the end memmoves into the destination view (freshly
// bound), and this page's count drops to `from`. The caller owns the
// chain links afterwards (format/prev/next are the caller's writes).
//
// Parameters:
//   from: the first moved position; the caller's split point (d on an
//     insert split: this keeps d, the destination takes d+1).
//   destination: a bound leaf-page view to fill; its previous contents
//     are overwritten.
//   key_width: the encoded key width; the entry stride.
//
// Returns: nothing; afterwards this page holds `from` entries and the
// destination holds count() - `from`, both contiguous and sorted.
void BptLeafPage::move_tail_to(std::size_t from, BptLeafPage& destination,
                               std::size_t key_width) {
    const std::size_t n = count();
    const std::size_t moved = n - from;
    const std::size_t stride = key_width + 8;
    std::memcpy(destination.buffer_ + kEntriesOffset,
                buffer_ + entry_offset(from), moved * stride);
    destination.write_count(moved);
    write_count(from);
}

// --- BptSeparatorPage --------------------------------------------------------
// BptSeparatorPage::bind — attaches the view to a 4096-byte page buffer
// at the given key width. Non-owning: the buffer must outlive the view.
//
// Parameters:
//   buffer: the page bytes; not zeroed, not owned.
//   key_width: the encoded key width; fixes the pair stride and the
//     capacity.
//
// Returns: nothing. Every accessor afterwards reads through the view.
void BptSeparatorPage::bind(char* buffer, std::size_t key_width) {
    buffer_ = buffer;
    key_width_ = key_width;
    capacity_ = separator_capacity(key_width);
}
// BptSeparatorPage::format — initializes the page as a valid empty
// separator: zero pairs, first child = kNoPage. The caller overwrites
// both immediately (a separator is never stored empty).
//
// Parameters: none.
//
// Returns: nothing; the page reads back as count 0 with no first child.
void BptSeparatorPage::format() {
    std::memset(buffer_, 0, kBlockSize);
    const std::uint32_t none = idx::kNoPage;
    std::memcpy(buffer_ + kFirstChildOffset, &none, 4);
}
// BptSeparatorPage::attach — validates the stored state against the
// capacity: the count field must not claim more pairs than fit. The
// separator castes are distinguished by the caller (the tree knows the
// level); attach only checks the arithmetic.
//
// Parameters:
//   error: cleared, then set on failure with the count and capacity.
//
// Returns: true when the page's stored state is internally possible;
// false with a named error when the count exceeds the capacity.
bool BptSeparatorPage::attach(std::string& error) const {
    const std::size_t count = raw_count();
    if (count > capacity_) {
        error = "separator page holds " + std::to_string(count) +
                " separators; at most " + std::to_string(capacity_) +
                " fit a " + std::to_string(kBlockSize) + "-byte page";
        return false;
    }
    return true;
}
// BptSeparatorPage::count — the number of (key, child) pairs on the
// page; the page routes count() + 1 children (pairs plus the first
// child).
//
// Parameters: none.
//
// Returns: the stored pair count.
std::size_t BptSeparatorPage::count() const { return raw_count(); }
// BptSeparatorPage::raw_count — reads the 32-bit count field raw, so
// the pair count's on-disk width can never drift to the platform size_t.
//
// Parameters: none.
//
// Returns: the stored pair count (4-byte field widened).
std::size_t BptSeparatorPage::raw_count() const {
    std::uint32_t count = 0;
    std::memcpy(&count, buffer_ + kCountOffset, sizeof(count));
    return count;
}
// BptSeparatorPage::write_count — writes the count field as a 32-bit
// value, the single funnel every pair-count mutation goes through.
//
// Parameters:
//   count: the new pair count; must fit the capacity (unchecked here).
//
// Returns: nothing; the field reads back as `count`.
void BptSeparatorPage::write_count(std::size_t count) {
    const std::uint32_t raw = static_cast<std::uint32_t>(count);
    std::memcpy(buffer_ + kCountOffset, &raw, sizeof(raw));
}
// BptSeparatorPage::child — the child pointer pair i routes to: pair i
// = (K_{i+1}, P_{i+1}), so child(i) is P_{i+1} — the subtree holding
// keys in [K_{i+1}, K_{i+2}).
//
// Parameters:
//   i: the pair position; 0 <= i < count().
//
// Returns: the child block id stored with pair i.
std::uint32_t BptSeparatorPage::child(std::size_t i) const {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer_ + entry_offset(i) + key_width_, 4);
    return value;
}
// BptSeparatorPage::separator_key — pair i's encoded key bytes: the
// separator K_{i+1}, which equals the first key of the child it routes
// to (the exactness invariant). Raw pointer on purpose — encoded keys
// hold embedded NULs.
//
// Parameters:
//   i: the pair position; 0 <= i < count().
//
// Returns: a pointer into the page buffer, codec.width() bytes valid
// while the frame stays pinned.
const char* BptSeparatorPage::separator_key(std::size_t i) const {
    return buffer_ + entry_offset(i);
}
// BptSeparatorPage::set_separator_key — rewrites pair i's key bytes:
// the exactness maintenance after a redistribution or a first-key erase
// (the recursive frame knows the child's index in the parent).
//
// Parameters:
//   i: the pair position; 0 <= i < count().
//   key: the new encoded separator, exactly codec.width() bytes.
//
// Returns: nothing.
void BptSeparatorPage::set_separator_key(std::size_t i, const char* key) {
    std::memcpy(buffer_ + entry_offset(i), key, key_width_);
}
// BptSeparatorPage::entry — pair i as an IndexEntry: the named form of
// the (encoded key, child) pair, for callers that pass the pair along.
//
// Parameters:
//   i: the pair position; 0 <= i < count().
//
// Returns: the (key, child) pair copied out of the page.
idx::IndexEntry BptSeparatorPage::entry(std::size_t i) const {
    idx::IndexEntry entry;
    entry.key.assign(buffer_ + entry_offset(i), key_width_);
    std::memcpy(&entry.child, buffer_ + entry_offset(i) + key_width_, 4);
    return entry;
}
// BptSeparatorPage::first_child — the leftmost child P0: the subtree
// holding keys < K1. A separator page routes count() + 1 children and
// this is the one no pair names.
//
// Parameters: none.
//
// Returns: the first child's block id (kNoPage on a formatted-but-empty
// page — never stored that way by the tree).
std::uint32_t BptSeparatorPage::first_child() const {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer_ + kFirstChildOffset, 4);
    return value;
}
// BptSeparatorPage::set_first_child — rewrites the leftmost child
// pointer (build, the wrapper's new root, and the rotations use it).
//
// Parameters:
//   page_id: the new first child's block id.
//
// Returns: nothing.
void BptSeparatorPage::set_first_child(std::uint32_t page_id) {
    std::memcpy(buffer_ + kFirstChildOffset, &page_id, 4);
}
// BptSeparatorPage::append — adds one (key, child) pair after the
// current last pair, in ascending key order (the build's bottom-up level
// construction is the caller and it feeds sorted pairs). The count grows
// by one.
//
// Parameters:
//   index_entry: the (encoded key, child) pair to add.
//   error: cleared, then set on failure with the capacity message.
//
// Returns: true on success; false with a named error when the page is
// full (count == capacity).
bool BptSeparatorPage::append(const idx::IndexEntry& index_entry,
std::string& error) {
    const std::size_t n = count();
    if (n >= capacity_) {
        error = "separator page is full (" + std::to_string(capacity_) +
                " separators)";
        return false;
    }
    char* raw = buffer_ + entry_offset(n);
    std::memcpy(raw, index_entry.key.data(), key_width_);
    std::memcpy(raw + key_width_, &index_entry.child, 4);
    write_count(n + 1);
    return true;
}
// BptSeparatorPage::routing_child_index — the lecture's routing rule in
// one place (slides 10 and 24): the POSITION of the child after the
// LAST separator <= `key` — 0 routes to the first child (key < K1), i
// to the child pair i-1 routes to (K_i <= key < K_{i+1}), count() to
// the last child (key >= Km).
//
// Parameters:
//   key: the encoded target key, exactly codec.width() bytes (raw
//     pointer — embedded NULs).
//   codec: the key codec; supplies the comparison order.
//
// Returns: a child position in 0..count(); the caller resolves it to a
// block id (first_child, or child(position - 1)).
std::size_t BptSeparatorPage::routing_child_index(
    const char* key, const isam::KeyCodec& codec) const {
    // The position of the child after the LAST separator <= key: the
    // count of separators that compare <= key. Positions run
    // 0..count(), so the result is always a valid child position.
    std::size_t lo = 0;
    std::size_t hi = count();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (codec.compare(separator_key(mid), key) <= 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}
// BptSeparatorPage::insert_pair_after — seats one (key, child) pair so
// the new child sits right after the child currently at position
// `after_child`: pair position = after_child (0 pushes every pair
// right; i inserts between separator i and its child). The tail
// memmoves to keep order; the count grows by one.
//
// Parameters:
//   after_child: the child position the new pair follows (the descend's
//     routing position).
//   pair: the (encoded key, child) pair; the key equals the new child's
//     subtree minimum (copy-up exactness).
//   error: cleared, then set on failure with the capacity message.
//
// Returns: true on success; false with a named error when the page is
// full — the caller splits instead.
bool BptSeparatorPage::insert_pair_after(std::size_t after_child,
const idx::IndexEntry& pair,
                                         std::string& error) {
    const std::size_t n = count();
    if (n >= capacity_) {
        error = "separator page is full (" + std::to_string(capacity_) +
                " separators)";
        return false;
    }
    // Pair i holds (K_{i+1}, P_{i+1}): inserting after child `after_child`
    // means the new pair lands at pair index `after_child` (0 pushes
    // every pair right; the new child rides at that position).
    const std::size_t pos = after_child;
    const std::size_t stride = key_width_ + 4;
    if (pos < n) {
        std::memmove(buffer_ + entry_offset(pos + 1),
                     buffer_ + entry_offset(pos), (n - pos) * stride);
    }
    char* raw = buffer_ + entry_offset(pos);
    std::memcpy(raw, pair.key.data(), key_width_);
    std::memcpy(raw + key_width_, &pair.child, 4);
    write_count(n + 1);
    return true;
}

// BptSeparatorPage::erase_pair — drops pair i (the merge's separator
// removal: the pair's key and child pointer both go, the child having
// been re-homed by the caller). The tail memmoves to close the gap; the
// count drops by one.
//
// Parameters:
//   i: the pair position; 0 <= i < count().
//   key_width: the encoded key width; the pair stride.
//
// Returns: nothing (bounds are the caller's to hold — the tree's
// recursion always names a valid pair).
void BptSeparatorPage::erase_pair(std::size_t i, std::size_t key_width) {
    const std::size_t n = count();
    const std::size_t stride = key_width + 4;
    if (i + 1 < n) {
        std::memmove(buffer_ + entry_offset(i),
                     buffer_ + entry_offset(i + 1), (n - i - 1) * stride);
    }
    write_count(n - 1);
}
// BptSeparatorPage::move_tail_to — the split's right half: every pair
// from position `from` to the end memmoves into the destination view,
// and this page's count drops to `from`. The destination's FIRST CHILD
// is the caller's to set (the child pair `from - 1` routes to, or the
// carried middle pair's child on an insert split).
//
// Parameters:
//   from: the first moved pair position.
//   destination: a bound separator view to fill; its pairs (not its
//     first child) are overwritten.
//   key_width: the encoded key width; the pair stride.
//
// Returns: nothing; afterwards this page holds `from` pairs and the
// destination holds count() - `from`, both ascending.
void BptSeparatorPage::move_tail_to(std::size_t from,
BptSeparatorPage& destination,
                                    std::size_t key_width) {
    const std::size_t n = count();
    const std::size_t moved = n - from;
    const std::size_t stride = key_width + 4;
    // Pairs [from..n) move; the right half's first child is the caller's
    // to set (pair from-1 routes to it, or the carried middle pair's
    // child on an insert split).
    std::memcpy(destination.buffer_ + kPairsOffset,
                buffer_ + entry_offset(from), moved * stride);
    destination.write_count(moved);
    write_count(from);
}

}
