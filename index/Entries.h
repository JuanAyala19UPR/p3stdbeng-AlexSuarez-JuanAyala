#pragma once

// Entries — the shared value vocabulary of the index layer: the row
// address, the two entry forms (the lecture's "data entries" and "index
// entries"), and the no-page sentinel. Both index modules speak this
// vocabulary; each module owns its own page formats and file kinds.

#include <cstdint>
#include <string>

namespace idx {

// An index-local row address: the (page, slot) of the record in the
// heap file. Kept index-local so index/ never needs heapfile/ headers.
struct Rid {
    std::uint32_t page_id = 0;
    std::uint32_t slot = 0;
};

inline bool operator==(const Rid& a, const Rid& b) {
    return a.page_id == b.page_id && a.slot == b.slot;
}

inline bool operator!=(const Rid& a, const Rid& b) {
    return !(a == b);
}

constexpr std::uint32_t kNoPage = 0xFFFFFFFFu;

// One DATA entry in its in-flight form — the lecture's "data entries":
// a pair of search key and rid, living at the leaf level. The encoded
// key is exactly codec.width() bytes (pages hold and compare bytes,
// only KeyCodec decodes); on a page the pair is laid out at the entry's
// offset, and this struct is what callers hand in and get back.
struct DataEntry {
    std::string key;   // encoded key bytes, exactly codec.width() long
    Rid rid;
};

// One INDEX entry — the lecture's "index entries": a (search key, child
// page) pair that guides traffic in the separator pages above the
// leaves. A separator page is packed with these plus its first child:
// P0 | K1 | P1 | ... | Km | Pm.
struct IndexEntry {
    std::string key;   // encoded separator key, exactly codec.width() long
    std::uint32_t child = kNoPage;
};

}
