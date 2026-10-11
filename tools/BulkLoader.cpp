#include "tools/BulkLoader.h"

#include "file/BlockFile.h"
#include "heapfile/SlottedPage.h"
#include "schema/TupleSerializer.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace tools {

namespace {

// The TUP2 header page's record area, re-derived from the same layout
// HeapFile encodes: magic "TUP2" at 0, then the five list fields, then
// the name. The differential check in the suite fails loudly if the two
// derivations ever drift.
constexpr std::uint32_t kHeapMagic = 0x32505554u; // "TUP2"
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kFreeHeadOffset = 4;
constexpr std::size_t kFreeTailOffset = 8;
constexpr std::size_t kFullHeadOffset = 12;
constexpr std::size_t kFullTailOffset = 16;
constexpr std::size_t kNextPageIdOffset = 20;
constexpr std::size_t kNameLengthOffset = 24;
constexpr std::size_t kNameOffset = 28;
constexpr std::size_t kMaxTableNameLength = 200;

std::uint32_t read_u32(const char* buffer, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

void write_u32(char* buffer, std::size_t offset, std::uint32_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

// Closes the block file on every exit path; close() is infallible.
struct FileGuard {
    bufman::File file;

    explicit FileGuard(bufman::File f) : file(f) {}
    ~FileGuard() {
        if (file.fd >= 0) {
            bufman::close(file);
        }
    }
};

// Composes and writes block 0: a SlottedPage-formatted header page whose
// record area carries the TUP2 identification and the two list heads.
// `full_first`/`full_last` are the full list's ends (kNoPage when empty);
// `free_first`/`free_last` the free list's; `next_page_id` is the
// allocation frontier the loader reached.
bool write_header_page(bufman::File& file, const schema::TableSchema& schema,
                       std::size_t record_size, std::uint32_t full_first,
                       std::uint32_t full_last, std::uint32_t free_first,
                       std::uint32_t free_last, std::uint32_t next_page_id,
                       std::string& error) {
    const std::string& name = schema.table_name();
    if (name.size() > kMaxTableNameLength) {
        error = "bulk load: table name is " + std::to_string(name.size()) +
                " bytes; at most " + std::to_string(kMaxTableNameLength) +
                " fit the header page";
        return false;
    }
    std::array<char, bufman::kBlockSize> block{};
    heapfile::SlottedPage header;
    header.bind(block.data(), record_size);
    header.format(); // the header page is on no list
    write_u32(block.data(), kMagicOffset, kHeapMagic);
    write_u32(block.data(), kFreeHeadOffset, free_first);
    write_u32(block.data(), kFreeTailOffset, free_last);
    write_u32(block.data(), kFullHeadOffset, full_first);
    write_u32(block.data(), kFullTailOffset, full_last);
    write_u32(block.data(), kNextPageIdOffset, next_page_id);
    write_u32(block.data(), kNameLengthOffset,
              static_cast<std::uint32_t>(name.size()));
    std::memcpy(block.data() + kNameOffset, name.data(), name.size());
    return bufman::write_block(file, 0, block.data(), block.size(), error);
}

// Validates an existing one-block file's header: identification record
// first (a foreign file must be reported as such), then the slotted
// layout, then empty lists (a loader refills only a virgin file).
bool check_empty_header(const char* block, const schema::TableSchema& schema,
                        std::size_t record_size, std::string& error) {
    if (read_u32(block, kMagicOffset) != kHeapMagic) {
        error = "bulk load: not a tuple heap file";
        return false;
    }
    const std::uint32_t length = read_u32(block, kNameLengthOffset);
    if (length > kMaxTableNameLength) {
        error = "bulk load: not a tuple heap file";
        return false;
    }
    const std::string stored(block + kNameOffset, length);
    if (stored != schema.table_name()) {
        error = "bulk load: heap file is for table '" + stored + "'";
        return false;
    }
    heapfile::SlottedPage header;
    header.bind(const_cast<char*>(block), record_size);
    if (!header.attach(error)) {
        error = "bulk load: " + error;
        return false;
    }
    if (read_u32(block, kFreeHeadOffset) != heapfile::kNoPage ||
        read_u32(block, kFullHeadOffset) != heapfile::kNoPage) {
        error = "bulk load: the file already holds records";
        return false;
    }
    return true;
}

}  // namespace

bool bulk_load(const std::string& path, const schema::TableSchema& schema,
               std::size_t count, const RowGenerator& generator,
               BulkLoadStats& stats, std::string& error) {
    stats = BulkLoadStats{};

    // The record layout decides everything structural; the same bounds
    // HeapFile::create enforces apply here.
    const schema::TupleSerializer serializer(schema);
    const std::size_t record_size = serializer.record_size();
    if (record_size == 0) {
        error = "bulk load: the catalog has no record layout (no columns, "
                "or String columns without declared width)";
        return false;
    }
    if (record_size > bufman::kBlockSize - heapfile::kPageHeaderSize - 1) {
        error = "bulk load: a " + std::to_string(bufman::kBlockSize) +
                "-byte page fits records of at most " +
                std::to_string(bufman::kBlockSize -
                               heapfile::kPageHeaderSize - 1) +
                " bytes, the catalog needs " + std::to_string(record_size);
        return false;
    }
    const std::size_t capacity = heapfile::slot_capacity(record_size);

    bufman::File file;
    if (!bufman::open_for_append(path, file, error)) {
        return false;
    }
    FileGuard guard(file);

    // Bootstrap decision by block count, refused before any write.
    const std::uint64_t blocks = bufman::block_count(file, error);
    if (!error.empty()) {
        return false;
    }
    if (blocks > 1) {
        error = "bulk load: the file already holds " +
                std::to_string(blocks) + " block(s)";
        return false;
    }
    if (blocks == 1) {
        std::array<char, bufman::kBlockSize> block{};
        if (!bufman::read_block(file, 0, block.data(), block.size(), error)) {
            return false;
        }
        if (!check_empty_header(block.data(), schema, record_size, error)) {
            return false;
        }
        // Reuse the file as-is: the header already carries the right
        // identification and empty lists.
    }

    // The fill/link loop: one local page buffer at a time. Every page
    // that fills completely joins the full list in generation order (its
    // trailer links are the list's links); a partial final page joins the
    // free list as its only element. Each block is written exactly once,
    // except the full list's tail, whose optimistic forward link is
    // patched after the loop.
    std::array<char, bufman::kBlockSize> page{};
    std::size_t record_index = 0;
    std::uint32_t page_id = 1;
    std::uint32_t full_first = heapfile::kNoPage;
    std::uint32_t full_last = heapfile::kNoPage;
    std::uint32_t partial_page = heapfile::kNoPage;
    std::array<char, bufman::kBlockSize> record{};
    while (record_index < count) {
        const std::size_t to_fill =
            std::min(capacity, count - record_index);
        const bool fills_completely = to_fill == capacity;
        heapfile::SlottedPage view;
        view.bind(page.data(), record_size);
        if (fills_completely) {
            view.format(full_last, page_id + 1); // optimistic list link
        } else {
            view.format(heapfile::kNoPage, heapfile::kNoPage);
        }
        for (std::size_t slot = 0; slot < to_fill; ++slot) {
            tuple::Tuple tuple;
            std::string generator_error;
            if (!generator(record_index, tuple, generator_error)) {
                error = "bulk load: generator failed at row index " +
                        std::to_string(record_index);
                if (!generator_error.empty()) {
                    error += ": " + generator_error;
                }
                return false;
            }
            if (!serializer.serialize(tuple, record.data(), record.size(),
                                      error)) {
                error = "bulk load: serialize failed at row index " +
                        std::to_string(record_index) + ": " + error;
                return false;
            }
            if (!view.put_record(slot, record.data(), record_size, error)) {
                error = "bulk load: " + error;
                return false;
            }
            ++record_index;
        }
        if (!bufman::write_block(file, page_id, page.data(), page.size(),
                                 error)) {
            error = "bulk load: " + error;
            return false;
        }
        if (fills_completely) {
            if (full_first == heapfile::kNoPage) {
                full_first = page_id;
            }
            full_last = page_id;
        } else {
            partial_page = page_id;
        }
        ++page_id;
        page.fill('\0');
    }

    // The full list's tail carried an optimistic forward link; it ends
    // the list (the partial page, when one exists, is on the free list).
    if (full_last != heapfile::kNoPage) {
        std::array<char, bufman::kBlockSize> tail{};
        if (!bufman::read_block(file, full_last, tail.data(), tail.size(),
                                error)) {
            error = "bulk load: " + error;
            return false;
        }
        write_u32(tail.data(), heapfile::kNextPageIdOffset,
                  heapfile::kNoPage);
        if (!bufman::write_block(file, full_last, tail.data(), tail.size(),
                                 error)) {
            error = "bulk load: " + error;
            return false;
        }
    }

    // The header: both lists' ends and the allocation frontier. Written
    // in both bootstrap cases — a fresh file gets its identification, and
    // a reused one-block file's empty lists now describe the loaded pages.
    if (!write_header_page(file, schema, record_size, full_first, full_last,
                           partial_page != heapfile::kNoPage ? partial_page
                                                             : heapfile::kNoPage,
                           partial_page != heapfile::kNoPage ? partial_page
                                                             : heapfile::kNoPage,
                           page_id, error)) {
        return false;
    }

    stats.records = record_index;
    stats.pages = page_id - 1;
    stats.per_page_capacity = capacity;
    return true;
}

}
