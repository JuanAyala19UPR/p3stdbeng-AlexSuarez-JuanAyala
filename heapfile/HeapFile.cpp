#include "heapfile/HeapFile.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace heapfile {

namespace {

// Releases a pin without disturbing `error`, which on failure paths already
// carries the real cause (unpin clears the string it is given).
void release(bufman::BufferManager& manager, std::size_t frame, bool was_dirty) {
    std::string unpin_error;
    manager.unpin(frame, was_dirty, unpin_error);
}

// The header page's record area (its slots are never used): magic, the five
// structural fields, then the table name. Fixed-width fields come before
// the variable-length name so every field has a constant offset.
//
//   0   magic "TUP2"
//   4   free_head     first page with a free slot (kNoPage when empty)
//   8   free_tail     last page appended to the free list
//   12  full_head     first page whose every slot is used
//   16  full_tail
//   20  next_page_id  the id the next page allocation will use (starts at 1)
//   24  name_length
//   28  name bytes
struct HeaderFields {
    std::uint32_t free_head = kNoPage;
    std::uint32_t free_tail = kNoPage;
    std::uint32_t full_head = kNoPage;
    std::uint32_t full_tail = kNoPage;
    std::uint32_t next_page_id = 1;
};

constexpr std::uint32_t kHeapMagic = 0x32505554u; // "TUP2"
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kHeaderFreeHeadOffset = 4;
constexpr std::size_t kHeaderFreeTailOffset = 8;
constexpr std::size_t kHeaderFullHeadOffset = 12;
constexpr std::size_t kHeaderFullTailOffset = 16;
constexpr std::size_t kHeaderNextPageIdOffset = 20;
constexpr std::size_t kNameLengthOffset = 24;
constexpr std::size_t kNameOffset = 28;
constexpr std::size_t kMaxTableNameLength = 200;

std::size_t head_offset(bool free_list) {
    return free_list ? kHeaderFreeHeadOffset : kHeaderFullHeadOffset;
}

std::size_t tail_offset(bool free_list) {
    return free_list ? kHeaderFreeTailOffset : kHeaderFullTailOffset;
}

HeaderFields read_header_fields(const char* buffer) {
    HeaderFields fields;
    fields.free_head = page_detail::read_u32(buffer, kHeaderFreeHeadOffset);
    fields.free_tail = page_detail::read_u32(buffer, kHeaderFreeTailOffset);
    fields.full_head = page_detail::read_u32(buffer, kHeaderFullHeadOffset);
    fields.full_tail = page_detail::read_u32(buffer, kHeaderFullTailOffset);
    fields.next_page_id = page_detail::read_u32(buffer, kHeaderNextPageIdOffset);
    return fields;
}

void write_header_fields(char* buffer, const HeaderFields& fields) {
    page_detail::write_u32(buffer, kHeaderFreeHeadOffset, fields.free_head);
    page_detail::write_u32(buffer, kHeaderFreeTailOffset, fields.free_tail);
    page_detail::write_u32(buffer, kHeaderFullHeadOffset, fields.full_head);
    page_detail::write_u32(buffer, kHeaderFullTailOffset, fields.full_tail);
    page_detail::write_u32(buffer, kHeaderNextPageIdOffset, fields.next_page_id);
}

// Writes the identification record into a bound header-page buffer: magic,
// empty lists, allocation frontier at 1, table name. Returns false with a
// named error when the table name is too long.
bool write_identification(char* buffer, const schema::TableSchema& schema,
                          std::string& error) {
    const std::string& name = schema.table_name();
    if (name.size() > kMaxTableNameLength) {
        error = "table name is " + std::to_string(name.size()) +
                " bytes; at most " + std::to_string(kMaxTableNameLength) +
                " fit the header page";
        return false;
    }
    page_detail::write_u32(buffer, kMagicOffset, kHeapMagic);
    write_header_fields(buffer, HeaderFields{});
    page_detail::write_u32(buffer, kNameLengthOffset,
                           static_cast<std::uint32_t>(name.size()));
    std::memcpy(buffer + kNameOffset, name.data(), name.size());
    return true;
}

// Checks the identification record against `schema`. False with a named
// error when the magic is missing or the name differs.
bool check_identification(const char* buffer,
                          const schema::TableSchema& schema,
                          std::string& error) {
    if (page_detail::read_u32(buffer, kMagicOffset) != kHeapMagic) {
        error = "not a tuple heap file";
        return false;
    }
    const std::uint32_t length = page_detail::read_u32(buffer, kNameLengthOffset);
    if (length > kMaxTableNameLength) {
        error = "not a tuple heap file";
        return false;
    }
    const std::string stored(buffer + kNameOffset, length);
    if (stored != schema.table_name()) {
        error = "heap file is for table '" + stored + "'";
        return false;
    }
    return true;
}

// The list pointers must name allocated pages (or be empty). A dirty page
// may not be on disk yet, so next_page_id can legitimately exceed the
// on-disk block count by at most the pool's frame count; anything beyond
// that is a corrupt header, not slack.
bool header_fields_in_range(const HeaderFields& fields, std::uint64_t blocks,
                            std::string& error) {
    constexpr std::uint64_t kDirtySlack = 4096; // >= any pool this engine builds
    const bool ok =
        fields.next_page_id >= 1 &&
        fields.next_page_id <= blocks + kDirtySlack &&
        (fields.free_head == kNoPage || fields.free_head < fields.next_page_id) &&
        (fields.free_tail == kNoPage || fields.free_tail < fields.next_page_id) &&
        (fields.full_head == kNoPage || fields.full_head < fields.next_page_id) &&
        (fields.full_tail == kNoPage || fields.full_tail < fields.next_page_id);
    if (!ok) {
        error = "header list pointers are out of range";
    }
    return ok;
}

}

HeapFile::HeapFile(bufman::BufferManager& manager) : manager_(manager) {}

bool HeapFile::is_open() const {
    return open_;
}

bool HeapFile::require_open(std::string& error) const {
    if (!open_) {
        error = "heap file is not open";
        return false;
    }
    return true;
}

bool HeapFile::create(const std::string& path,
                      const schema::TableSchema& schema, std::string& error) {
    error.clear();
    if (open_) {
        error = "cannot create \"" + path + "\": this heap file is already open";
        return false;
    }
    const schema::TupleSerializer serializer(schema);
    const std::size_t size = serializer.record_size();
    if (size == 0) {
        error = "cannot create \"" + path +
                "\": the catalog has no record layout (no columns, or "
                "String columns without declared width)";
        return false;
    }
    if (size > bufman::kPageSize - kPageHeaderSize - 1) {
        error = "cannot create \"" + path + "\": a " +
                std::to_string(bufman::kPageSize) +
                "-byte page fits records of at most " +
                std::to_string(bufman::kPageSize - kPageHeaderSize - 1) +
                " bytes, the catalog needs " + std::to_string(size);
        return false;
    }
    // open_for_append creates the file when missing, so this also covers the
    // fresh-path case; an existing empty path is allowed.
    bufman::File file;
    if (!manager_.open_file(path, file, error)) {
        return false;
    }
    const std::uint64_t blocks = bufman::block_count(file, error);
    if (!error.empty()) {
        std::string close_error;
        manager_.close_file(file, close_error);
        return false;
    }
    if (blocks > 0) {
        error = "cannot create \"" + path + "\": the file already holds " +
                std::to_string(blocks) + " block(s)";
        std::string close_error;
        manager_.close_file(file, close_error);
        return false;
    }

    std::size_t frame = 0;
    if (!manager_.alloc_page(file, 0, frame, error)) {
        std::string close_error;
        manager_.close_file(file, close_error);
        return false;
    }
    SlottedPage header;
    header.bind(manager_.frame(frame).buffer, size);
    // prev/next default to kNoPage: the header page is on no list, and its
    // trailer links are never read again — the list heads live in the
    // record area.
    header.format();
    if (!write_identification(manager_.frame(frame).buffer, schema, error)) {
        release(manager_, frame, false);
        std::string close_error;
        manager_.close_file(file, close_error);
        return false;
    }
    release(manager_, frame, true);

    // Write the header back and leave a one-block file on disk.
    manager_.close_file(file, error);
    return error.empty();
}

bool HeapFile::open(const std::string& path,
                    const schema::TableSchema& schema, std::string& error) {
    error.clear();
    if (open_) {
        error = "cannot open \"" + path + "\": this heap file is already open";
        return false;
    }
    const schema::TupleSerializer serializer(schema);
    const std::size_t size = serializer.record_size();
    if (size == 0) {
        error = "cannot open \"" + path +
                "\": the catalog has no record layout (no columns, or "
                "String columns without declared width)";
        return false;
    }
    if (size > bufman::kPageSize - kPageHeaderSize - 1) {
        error = "cannot open \"" + path + "\": a " +
                std::to_string(bufman::kPageSize) +
                "-byte page fits records of at most " +
                std::to_string(bufman::kPageSize - kPageHeaderSize - 1) +
                " bytes, the catalog needs " + std::to_string(size);
        return false;
    }
    // Probe with a read-only open first: manager_.open_file would silently
    // create a missing file (O_CREAT), and a missing path must be reported
    // as such rather than as a validation failure.
    bufman::File probe;
    if (!bufman::open_for_read(path, probe, error)) {
        error = "cannot open \"" + path + "\": " + error;
        return false;
    }
    bufman::close(probe);

    if (!manager_.open_file(path, file_, error)) {
        return false;
    }

    const std::uint64_t blocks = bufman::block_count(file_, error);
    if (error.empty() && blocks == 0) {
        error = "cannot open \"" + path + "\": the file has no header page";
    }
    std::size_t frame = 0;
    if (error.empty() && manager_.pin(file_, 0, frame, error)) {
        SlottedPage header;
        header.bind(manager_.frame(frame).buffer, size);
        // Identification first: the magic and name do not depend on the
        // record size, and a foreign file must be reported as such rather
        // than as a slot-count mismatch from attach.
        if (!check_identification(manager_.frame(frame).buffer, schema,
                                  error)) {
            error = "cannot open \"" + path + "\": " + error;
        } else if (!header.attach(error)) {
            error = "cannot open \"" + path + "\": " + error;
        } else if (!header_fields_in_range(
                       read_header_fields(manager_.frame(frame).buffer),
                       blocks, error)) {
            error = "cannot open \"" + path + "\": " + error;
        }
        release(manager_, frame, false);
    }
    if (!error.empty()) {
        // Unwind completely: an unusable file must not stay open. The close
        // gets its own error string so the real cause above survives.
        std::string close_error;
        manager_.close_file(file_, close_error);
        file_ = bufman::File{};
        return false;
    }
    schema_ = &schema;
    serializer_.emplace(schema);
    record_size_ = size;
    open_ = true;
    return true;
}

HeapFileIterator HeapFile::scan() {
    // One header visit for the two list heads and the walk bound. A closed
    // file yields an exhausted iterator whose next() reports the state.
    HeaderFields fields;
    if (open_) {
        std::string probe_error;
        std::size_t frame = 0;
        if (manager_.pin(file_, 0, frame, probe_error)) {
            fields = read_header_fields(manager_.frame(frame).buffer);
            release(manager_, frame, false);
        }
    }
    return HeapFileIterator(*this, fields.free_head, fields.full_head,
                            fields.next_page_id);
}

// --- List surgery -----------------------------------------------------------

bool HeapFile::append_fresh_page(std::uint32_t page_id, std::size_t page_frame,
                                 std::string& error) {
    // Only called right after an allocation, when the free list is empty by
    // construction — the fresh page becomes head and tail, and the header
    // is the only auxiliary pin.
    std::size_t header_frame = 0;
    if (!manager_.pin(file_, 0, header_frame, error)) {
        return false;
    }
    char* header = manager_.frame(header_frame).buffer;
    const HeaderFields fields = read_header_fields(header);
    if (fields.free_head != kNoPage) {
        release(manager_, header_frame, false);
        error = "internal error: fresh page appended to a non-empty free list";
        return false;
    }
    page_detail::write_u32(header, kHeaderFreeHeadOffset, page_id);
    page_detail::write_u32(header, kHeaderFreeTailOffset, page_id);
    page_detail::write_u32(header, kHeaderNextPageIdOffset, page_id + 1);
    release(manager_, header_frame, true);

    SlottedPage page;
    page.bind(manager_.frame(page_frame).buffer, record_size_);
    page.set_prev_page(kNoPage);
    page.set_next_page(kNoPage);
    return true;
}

bool HeapFile::move_between_lists(bool to_full, std::uint32_t page_id,
                                  std::size_t page_frame, std::string& error) {
    // The moving page arrives pinned; its prev/next links name its
    // neighbors in the list it is leaving.
    SlottedPage self;
    self.bind(manager_.frame(page_frame).buffer, record_size_);
    const std::uint32_t prev = self.prev_page();
    const std::uint32_t next = self.next_page();
    // Moving TO the full list means moving FROM the free list.
    const bool src_is_free = to_full;

    // 1. Header first (id 0), then every auxiliary page in ascending
    //    page-id order. The pin set is always distinct: the source-list
    //    neighbors and the destination tail sit on the other list than the
    //    moving page, and page 0 is on no list. Once all pins are held,
    //    every remaining step is a memory write — the move is either fully
    //    applied or not attempted.
    std::size_t header_frame = 0;
    if (!manager_.pin(file_, 0, header_frame, error)) {
        return false;
    }
    const HeaderFields fields =
        read_header_fields(manager_.frame(header_frame).buffer);
    if (page_id == 0 || (src_is_free && fields.free_head == kNoPage) ||
        (!src_is_free && fields.full_head == kNoPage)) {
        release(manager_, header_frame, false);
        error = "internal error: moving page " + std::to_string(page_id) +
                " off an empty list";
        return false;
    }
    const std::uint32_t dest_tail =
        to_full ? fields.full_tail : fields.free_tail;

    // The auxiliary candidates: source-list neighbors and the destination
    // tail (kNoPage entries need no pin). Pin them in ascending id order.
    struct Aux {
        std::uint32_t id;
        std::size_t frame = 0;
        bool pinned = false;
    };
    Aux aux[3] = {{prev}, {next}, {dest_tail}};
    for (int i = 1; i < 3; ++i) { // insertion sort by id, kNoPage last
        Aux key = aux[i];
        int j = i - 1;
        for (; j >= 0; --j) {
            const bool key_last = key.id == kNoPage;
            const bool slot_last = aux[j].id == kNoPage;
            if (key_last || (!slot_last && aux[j].id <= key.id)) {
                break;
            }
            aux[j + 1] = aux[j];
        }
        aux[j + 1] = key;
    }
    bool all_pinned = true;
    for (Aux& entry : aux) {
        if (entry.id == kNoPage) {
            continue;
        }
        if (manager_.pin(file_, entry.id, entry.frame, error)) {
            entry.pinned = true;
        } else {
            all_pinned = false;
            break;
        }
    }
    if (!all_pinned) {
        for (Aux& entry : aux) {
            if (entry.pinned) {
                release(manager_, entry.frame, false);
            }
        }
        release(manager_, header_frame, false);
        return false;
    }

    // 2. All pins held — detach from the source list.
    char* header = manager_.frame(header_frame).buffer;
    if (prev == kNoPage) {
        page_detail::write_u32(header, head_offset(src_is_free), next);
    }
    if (next == kNoPage) {
        page_detail::write_u32(header, tail_offset(src_is_free), prev);
    }
    for (const Aux& entry : aux) {
        if (entry.id == kNoPage) {
            continue;
        }
        SlottedPage neighbor;
        neighbor.bind(manager_.frame(entry.frame).buffer, record_size_);
        if (entry.id == prev) {
            neighbor.set_next_page(next);
        }
        if (entry.id == next) {
            neighbor.set_prev_page(prev);
        }
        if (entry.id == dest_tail) {
            neighbor.set_next_page(page_id);
        }
    }

    // 3. Append to the destination list (at the tail). The offset helpers
    //    read true = free list, and the destination is free exactly when
    //    to_full is false.
    const bool dest_is_free = !to_full;
    if (dest_tail == kNoPage) {
        page_detail::write_u32(header, head_offset(dest_is_free), page_id);
        self.set_prev_page(kNoPage);
    } else {
        self.set_prev_page(dest_tail);
    }
    self.set_next_page(kNoPage);
    page_detail::write_u32(header, tail_offset(dest_is_free), page_id);

    // 4. Release the auxiliary pins (the caller releases the moving page).
    for (const Aux& entry : aux) {
        if (entry.id == kNoPage) {
            continue;
        }
        release(manager_, entry.frame, true);
    }
    release(manager_, header_frame, true);
    return true;
}

// --- Row operations ---------------------------------------------------------

bool HeapFile::insert(const tuple::Tuple& tuple, RowId& row_id,
                      std::string& error) {
    if (!require_open(error)) {
        return false;
    }
    // serialize validates the tuple against the schema first (arity,
    // types, string widths) and fails before any page is pinned.
    std::array<char, bufman::kPageSize> record{};
    if (!serializer_->serialize(tuple, record.data(), record.size(), error)) {
        return false;
    }

    // 1. Pick the target page: the free list's head — the oldest page with
    //    a free slot — found without walking anything. When the free list
    //    is empty, allocate a fresh page.
    HeaderFields fields;
    {
        std::size_t frame = 0;
        if (!manager_.pin(file_, 0, frame, error)) {
            return false;
        }
        fields = read_header_fields(manager_.frame(frame).buffer);
        release(manager_, frame, false);
    }
    std::uint32_t target = fields.free_head;
    std::size_t held_frame = 0; // a pin already held across step 2
    bool pin_held = false;
    if (target == kNoPage) {
        if (fields.next_page_id >= kNoPage) {
            error = "insert would need page id " +
                    std::to_string(fields.next_page_id) +
                    ", beyond the 32-bit row id range";
            return false;
        }
        const std::uint32_t new_page_id = fields.next_page_id;
        std::size_t new_frame = 0;
        // The allocation happens before any list write: alloc_page is the
        // only step that can fail here, and a failure leaves the file
        // untouched. Once it succeeds, append_fresh_page's writes are pure
        // memory operations. The allocation's pin is kept for step 2.
        if (!manager_.alloc_page(file_, new_page_id, new_frame, error)) {
            return false;
        }
        SlottedPage fresh;
        fresh.bind(manager_.frame(new_frame).buffer, record_size_);
        fresh.format();
        if (!append_fresh_page(new_page_id, new_frame, error)) {
            release(manager_, new_frame, true);
            return false;
        }
        target = new_page_id;
        held_frame = new_frame;
        pin_held = true;
    }

    // 2. Store the row in the target page's first free slot. The page
    //    stays pinned through the possible list transition.
    std::size_t frame = 0;
    if (pin_held) {
        frame = held_frame;
    } else if (!manager_.pin(file_, target, frame, error)) {
        return false;
    }
    SlottedPage page;
    page.bind(manager_.frame(frame).buffer, record_size_);
    if (!page.attach(error)) {
        release(manager_, frame, false);
        return false;
    }
    const std::optional<std::size_t> slot = page.first_free_slot();
    if (!slot) {
        // Unreachable while the lists are consistent — free list means
        // "has a free slot" by derivation. check_structure is the tool
        // for exactly this drift.
        release(manager_, frame, false);
        error = "free list holds page " + std::to_string(target) +
                " with no free slot; the lists are inconsistent";
        return false;
    }
    if (!page.put_record(*slot, record.data(), record_size_, error)) {
        release(manager_, frame, false);
        return false;
    }
    const bool filled = page.occupied_count() == page.capacity();
    if (filled && !move_between_lists(true, target, frame, error)) {
        // The row is stored; only the list move failed (pin exhaustion).
        // The drift is real and reported — check_structure would flag it.
        release(manager_, frame, true);
        return false;
    }
    release(manager_, frame, true);

    row_id = RowId{target, static_cast<std::uint32_t>(*slot)};
    return true;
}

bool HeapFile::pin_record_page(const RowId row_id, SlottedPage& page,
                               std::size_t& frame, std::string& error) {
    if (row_id.page_id == 0) {
        error = "page 0 is the header page; it never stores records";
        return false;
    }
    const std::uint64_t blocks = bufman::block_count(file_, error);
    if (!error.empty()) {
        return false;
    }
    // A page exists iff it is resident in the pool or already written back:
    // dirty pages reach disk only on eviction/flush, so the on-disk block
    // count alone would lag behind the pool and reject live pages.
    if (row_id.page_id >= blocks && !manager_.resident(file_, row_id.page_id)) {
        error = "page " + std::to_string(row_id.page_id) +
                " is beyond the end of the file";
        return false;
    }
    if (!manager_.pin(file_, row_id.page_id, frame, error)) {
        return false;
    }
    page.bind(manager_.frame(frame).buffer, record_size_);
    if (!page.attach(error)) {
        release(manager_, frame, false);
        return false;
    }
    if (!page.is_occupied(row_id.slot, error)) {
        if (error.empty()) {
            error = "no record at row id (page " + std::to_string(row_id.page_id) +
                    ", slot " + std::to_string(row_id.slot) + "): the slot is free";
        }
        release(manager_, frame, false);
        return false;
    }
    return true;
}

bool HeapFile::find(const RowId row_id, tuple::Tuple& tuple,
                    std::string& error) {
    if (!require_open(error)) {
        return false;
    }
    std::size_t frame = 0;
    SlottedPage page;
    if (!pin_record_page(row_id, page, frame, error)) {
        return false;
    }
    std::array<char, bufman::kPageSize> record{};
    const bool got = page.get_record(row_id.slot, record.data(), record.size(),
                                     error);
    release(manager_, frame, false);
    if (!got) {
        return false;
    }
    return serializer_->deserialize(record.data(), record.size(), tuple,
                                    error);
}

bool HeapFile::update(const RowId row_id, const tuple::Tuple& tuple,
                      std::string& error) {
    if (!require_open(error)) {
        return false;
    }
    std::array<char, bufman::kPageSize> record{};
    if (!serializer_->serialize(tuple, record.data(), record.size(), error)) {
        return false;
    }
    std::size_t frame = 0;
    SlottedPage page;
    if (!pin_record_page(row_id, page, frame, error)) {
        return false;
    }
    const bool ok = page.put_record(row_id.slot, record.data(), record_size_,
                                    error);
    release(manager_, frame, ok);
    return ok;
}

bool HeapFile::erase(const RowId row_id, std::string& error) {
    if (!require_open(error)) {
        return false;
    }
    std::size_t frame = 0;
    SlottedPage page;
    if (!pin_record_page(row_id, page, frame, error)) {
        return false;
    }
    // The transition decision must be made before the slot is cleared:
    // "was full" is what sends the page back to the free list.
    const bool was_full = page.occupied_count() == page.capacity();
    if (!page.erase(row_id.slot, error)) {
        release(manager_, frame, false);
        return false;
    }
    if (was_full && !move_between_lists(false, row_id.page_id, frame, error)) {
        // The slot is freed; only the list move failed (pin exhaustion).
        release(manager_, frame, true);
        return false;
    }
    release(manager_, frame, true);
    return true;
}

// --- Structure self-check ---------------------------------------------------

bool HeapFile::check_structure(std::string& error) {
    error.clear();
    if (!require_open(error)) {
        return false;
    }
    HeaderFields fields;
    std::uint64_t blocks = 0;
    {
        std::size_t frame = 0;
        if (!manager_.pin(file_, 0, frame, error)) {
            return false;
        }
        fields = read_header_fields(manager_.frame(frame).buffer);
        blocks = bufman::block_count(file_, error);
        release(manager_, frame, false);
        if (!error.empty()) {
            return false;
        }
    }
    if (!header_fields_in_range(fields, blocks, error)) {
        return false;
    }

    // Walk both lists, one pin at a time. `seen` indexes page ids; the
    // range check above bounds next_page_id, so the vector is small.
    std::vector<char> seen(fields.next_page_id, 0);
    const std::uint32_t heads[2] = {fields.free_head, fields.full_head};
    const std::uint32_t tails[2] = {fields.free_tail, fields.full_tail};
    for (int list = 0; list < 2; ++list) {
        const char* list_name = list == 0 ? "free" : "full";
        std::uint32_t current = heads[list];
        std::uint32_t last = kNoPage;
        while (current != kNoPage) {
            if (current == 0) {
                error = std::string("the header page appears on the ") +
                        list_name + " list";
                return false;
            }
            if (current >= fields.next_page_id) {
                error = std::string("page ") + std::to_string(current) +
                        " on the " + list_name +
                        " list is beyond next_page_id";
                return false;
            }
            if (seen[current] != 0) {
                error = std::string("page ") + std::to_string(current) +
                        " is reachable twice; the lists are cyclic or "
                        "share a page";
                return false;
            }
            seen[current] = 1;
            std::size_t frame = 0;
            if (!manager_.pin(file_, current, frame, error)) {
                return false;
            }
            SlottedPage page;
            page.bind(manager_.frame(frame).buffer, record_size_);
            if (!page.attach(error)) {
                release(manager_, frame, false);
                return false;
            }
            // Membership ⇔ occupancy: the free list holds exactly the
            // pages with a free slot, the full list exactly the full ones.
            const bool full = page.occupied_count() == page.capacity();
            if (full != (list == 1)) {
                error = std::string("page ") + std::to_string(current) +
                        (full ? " is full but sits on the free list"
                              : " has free space but sits on the full list");
                release(manager_, frame, false);
                return false;
            }
            last = current;
            current = page.next_page();
            release(manager_, frame, false);
        }
        if ((heads[list] == kNoPage) != (tails[list] == kNoPage)) {
            error = std::string("the ") + list_name +
                    " list's head and tail disagree about emptiness";
            return false;
        }
        if (tails[list] != last) {
            error = std::string("the ") + list_name +
                    " list's tail pointer does not match the walk";
            return false;
        }
    }

    // The union of both lists is exactly the allocated pages.
    std::size_t reachable = 0;
    for (std::uint32_t id = 1; id < fields.next_page_id; ++id) {
        reachable += seen[id];
    }
    if (reachable != fields.next_page_id - 1) {
        error = std::to_string(fields.next_page_id - 1 - reachable) +
                " allocated page(s) are on no list";
        return false;
    }

    // Every on-disk data page must be reachable (dirty pages may not be on
    // disk yet, so not the other way around). A block beyond next_page_id
    // cannot be allocated and is corruption in its own right.
    for (std::uint64_t block = 1; block < blocks; ++block) {
        if (block >= seen.size()) {
            error = "on-disk page " + std::to_string(block) +
                    " is beyond next_page_id";
            return false;
        }
        if (seen[block] == 0) {
            error = "on-disk page " + std::to_string(block) +
                    " is on no list";
            return false;
        }
    }
    return true;
}

// --- Lifecycle --------------------------------------------------------------

bool HeapFile::resident(std::uint32_t page_id) const {
    return manager_.resident(file_, page_id);
}

bool HeapFile::close(std::string& error) {
    if (!require_open(error)) {
        return false;
    }
    // Reset the HeapFile's state only when the manager actually released
    // the file: a failed close leaves it open through the manager, and
    // forgetting it here would orphan the registration (and its fd). The
    // caller can inspect `error` and retry the close.
    if (!manager_.close_file(file_, error)) {
        return false;
    }
    open_ = false;
    schema_ = nullptr;
    serializer_.reset();
    record_size_ = 0;
    file_ = bufman::File{};
    return true;
}

// --- HeapFileIterator -------------------------------------------------------

HeapFileIterator::HeapFileIterator(HeapFile& file, std::uint32_t free_head,
                                   std::uint32_t full_head,
                                   std::uint32_t allocated)
    : file_(&file),
      page_id_(free_head),
      next_list_(full_head),
      slot_(0),
      seen_(allocated, 0) {}

bool HeapFileIterator::next(tuple::Tuple& tuple, std::string& error) {
    error.clear();
    if (!file_->is_open()) {
        error = "heap file is not open";
        return false;
    }
    std::array<char, bufman::kPageSize> record{};
    for (;;) {
        if (page_id_ == kNoPage) {
            // The current list is done; promote the remaining one.
            if (next_list_ == kNoPage) {
                return false; // both lists exhausted, error stays empty
            }
            page_id_ = next_list_;
            next_list_ = kNoPage;
            slot_ = 0;
        }
        if (page_id_ >= seen_.size()) {
            error = "list walk reached page " + std::to_string(page_id_) +
                    ", beyond the allocated page range";
            page_id_ = kNoPage;
            next_list_ = kNoPage;
            return false;
        }
        if (page_id_ != iterating_) {
            // Entering a page the walk already left: the lists are cyclic.
            if (seen_[page_id_] != 0) {
                error = "list walk revisited page " +
                        std::to_string(page_id_) + "; the lists are cyclic";
                page_id_ = kNoPage;
                next_list_ = kNoPage;
                return false;
            }
            seen_[page_id_] = 1;
            iterating_ = page_id_;
        }
        std::size_t frame = 0;
        if (!file_->manager_.pin(file_->file_, page_id_, frame, error)) {
            return false;
        }
        SlottedPage page;
        page.bind(file_->manager_.frame(frame).buffer, file_->record_size_);
        if (!page.attach(error)) {
            release(file_->manager_, frame, false);
            return false;
        }
        const std::uint32_t following = page.next_page();
        bool found = false;
        for (std::uint32_t slot = slot_; slot < page.capacity(); ++slot) {
            if (!page.is_occupied(slot, error)) {
                if (!error.empty()) {
                    release(file_->manager_, frame, false);
                    return false;
                }
                continue;
            }
            if (!page.get_record(slot, record.data(), record.size(), error)) {
                release(file_->manager_, frame, false);
                return false;
            }
            // Copy first, decode after the pin is released: the record
            // bytes live in the frame buffer only while it stays pinned.
            release(file_->manager_, frame, false);
            if (!file_->serializer_->deserialize(record.data(), record.size(),
                                                 tuple, error)) {
                return false;
            }
            current_ = RowId{page_id_, slot};
            if (slot + 1 < page.capacity()) {
                slot_ = slot + 1; // stay on this page
            } else {
                page_id_ = following;
                slot_ = 0;
                iterating_ = kNoPage; // the walk is leaving this page
            }
            found = true;
            break;
        }
        if (found) {
            return true;
        }
        // Page exhausted from slot_ on: continue with the next one.
        release(file_->manager_, frame, false);
        page_id_ = following;
        slot_ = 0;
        iterating_ = kNoPage; // the walk is leaving this page
    }
}

// --- Free functions ---------------------------------------------------------

bool erase_file(bufman::BufferManager& manager, const std::string& path,
                std::string& error) {
    error.clear();
    if (manager.is_open(path)) {
        error = "cannot erase \"" + path + "\": the file is open";
        return false;
    }
    if (std::remove(path.c_str()) != 0) {
        error = "cannot erase \"" + path + "\": " + std::strerror(errno);
        return false;
    }
    return true;
}

}
