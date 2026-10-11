// p3dbeng — the storage suite: p2dbeng's TUP2 heap checks carried
// forward unchanged (the hand-built list-surgery cases and the two-list
// heap file exercise, with the structural self-check after every
// operation and the randomized insert/erase shadow model).

#include "bufpool/BufferManager.h"
#include "heapfile/HeapFile.h"
#include "policy/LRUPolicy.h"
#include "schema/TableCsv.h"
#include "schema/TableGenerator.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"
#include "tuple/Column.h"
#include "tuple/Tuple.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <unistd.h>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("PASS: %s\n", what);
    } else {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
    std::fflush(stdout);
}

std::string temp_path() {
    return std::string("/tmp/p3-tuple-") + std::to_string(getpid()) + ".bin";
}

// --- storage: hand-built list-surgery cases ----------------------------------
//
// A three-page file walked through every detach/append shape the lists can
// take — head, middle, tail, and only element — with the structural check
// after every step and the scan order verified against the expected page
// sequence (the free list's pages, then the full list's).

void list_case_checks() {
    std::filesystem::remove("listcase.bin");
    schema::TableSchema schema("");
    std::string error;
    check(schema::parse_table_schema(
              R"({"table_name": "listcase",
                  "columns": [
                    {"column_name": "sid", "column_type": "Integer", "size": 4},
                    {"column_name": "pad", "column_type": "String",  "size": 900}]})",
              schema, error),
          "listcase: schema fixture parses");
    const auto make_row = [](int id) {
        tuple::Tuple row;
        row.add(id);
        row.add(std::string(900, static_cast<char>('a' + id % 26)));
        return row;
    };
    const auto scan_ids = [&](heapfile::HeapFile& file) {
        std::vector<int> ids;
        heapfile::HeapFileIterator it = file.scan();
        tuple::Tuple row;
        while (it.next(row, error)) {
            ids.push_back(std::get<int>(row.column(0).column_value()));
        }
        return ids;
    };

    bufman::BufferManager manager;
    std::string init_error;
    manager.init(8, std::make_unique<bufman::LRUPolicy>(), init_error);
    heapfile::HeapFile file(manager);
    check(file.create("listcase.bin", schema, error) &&
              file.open("listcase.bin", schema, error),
          "listcase: file created and opened");

    // One page fills: the full list's only element. Erasing unlinks that
    // only element and appends it to an empty free list.
    bool placed = true;
    for (int id = 1; id <= 4; ++id) {
        heapfile::RowId rid;
        placed = placed && file.insert(make_row(id), rid, error) &&
                 rid.page_id == 1 && rid.slot == static_cast<std::uint32_t>(id - 1);
    }
    check(placed && file.check_structure(error),
          "listcase: four rows fill page 1");
    check(file.erase(heapfile::RowId{1, 0}, error) &&
              file.check_structure(error),
          "listcase: the full list's only element unlinks");
    {
        const std::vector<int> expected = {2, 3, 4};
        check(scan_ids(file) == expected && error.empty(),
              "listcase: the freed page scans before the full pages");
    }
    heapfile::RowId rid;
    check(file.insert(make_row(5), rid, error) && rid.page_id == 1 &&
              rid.slot == 0 && file.check_structure(error),
          "listcase: the page refills and rejoins the full list");

    // Fill pages 2 and 3 completely: page 1 came back full, so ids 6..9
    // land on page 2 and 10..13 on page 3. The full list becomes
    // 1 -> 2 -> 3 and the free list is empty.
    placed = true;
    for (int id = 6; id <= 13; ++id) {
        heapfile::RowId at;
        placed = placed && file.insert(make_row(id), at, error) &&
                 at.page_id == static_cast<std::uint32_t>(id <= 9 ? 2 : 3);
    }
    check(placed && file.check_structure(error),
          "listcase: twelve live rows fill pages 1..3 in order");

    // Middle of the full list: erasing from page 2 unlinks it between its
    // neighbors and leaves it as the free list's only element.
    check(file.erase(heapfile::RowId{2, 0}, error) &&
              file.check_structure(error),
          "listcase: a middle page unlinks from the full list");
    {
        const std::vector<int> expected = {7, 8, 9, 5, 2, 3, 4, 10, 11, 12, 13};
        check(scan_ids(file) == expected && error.empty(),
              "listcase: the scan walks the free list before the full list");
    }
    check(file.insert(make_row(14), rid, error) && rid.page_id == 2 &&
              rid.slot == 0 && file.check_structure(error),
          "listcase: the middle page refills onto the full list's tail");
    {
        const std::vector<int> expected = {5, 2, 3, 4, 10, 11, 12, 13, 14, 7, 8, 9};
        check(scan_ids(file) == expected && error.empty(),
              "listcase: the refilled page scans at the full list's tail");
    }

    // Head of the full list (page 1 leads 1 -> 3 -> 2 now): unlink, refill,
    // and the page comes back at the tail.
    check(file.erase(heapfile::RowId{1, 0}, error) &&
              file.check_structure(error),
          "listcase: the full list's head unlinks");
    {
        const std::vector<int> expected = {2, 3, 4, 10, 11, 12, 13, 14, 7, 8, 9};
        check(scan_ids(file) == expected && error.empty(),
              "listcase: the freed head page scans before the full pages");
    }
    check(file.insert(make_row(15), rid, error) && rid.page_id == 1 &&
              rid.slot == 0 && file.check_structure(error),
          "listcase: the head page refills at the full list's tail");
    {
        const std::vector<int> expected = {10, 11, 12, 13, 14, 7, 8, 9, 15, 2, 3, 4};
        check(scan_ids(file) == expected && error.empty(),
              "listcase: the full list's order follows the refills");
    }

    // Tail of the full list (page 1 closes 3 -> 2 -> 1 now).
    check(file.erase(heapfile::RowId{1, 3}, error) &&
              file.check_structure(error),
          "listcase: the full list's tail unlinks");
    {
        const std::vector<int> expected = {15, 2, 3, 10, 11, 12, 13, 14, 7, 8, 9};
        check(scan_ids(file) == expected && error.empty(),
              "listcase: the freed tail page scans before the full pages");
    }
    check(file.insert(make_row(16), rid, error) && rid.page_id == 1 &&
              rid.slot == 3 && file.check_structure(error),
          "listcase: the tail page refills onto the full list");

    // The previous format is refused by name, not misread.
    check(file.close(error), "listcase: file closed for the format step");
    {
        bufman::File raw;
        check(bufman::open_for_append("listcase.bin", raw, error),
              "listcase: raw block access for the format step");
        std::array<char, bufman::kBlockSize> block{};
        block[0] = 'T';
        block[1] = 'U';
        block[2] = 'P';
        block[3] = '1';
        check(bufman::write_block(raw, 0, block.data(), block.size(), error),
              "listcase: TUP1 magic written over the header");
        bufman::close(raw);
    }
    {
        heapfile::HeapFile old_format(manager);
        const bool refused =
            !old_format.open("listcase.bin", schema, error) &&
            error.find("not a tuple heap file") != std::string::npos;
        check(refused, "listcase: a TUP1 file is refused by name");
    }

    manager.close_all(error);
    std::filesystem::remove("listcase.bin");
}

// --- storage: the two-list heap file ----------------------------------------
//
// Exercises the free-space lists directly through the HeapFile API: the
// invariant checker runs after every single operation, so any relink
// mistake surfaces at the step that caused it, not as a mystery later.

void storage_checks() {
    // The suite may run repeatedly in one working directory.
    std::filesystem::remove("storage.bin");
    schema::TableSchema schema("");
    std::string error;
    // sid(4) + pad(900) = 904-byte records -> 4 slots per page: small
    // row counts exercise multi-page behavior.
    check(schema::parse_table_schema(
              R"({"table_name": "storage",
                  "columns": [
                    {"column_name": "sid", "column_type": "Integer", "size": 4},
                    {"column_name": "pad", "column_type": "String",  "size": 900}]})",
              schema, error),
          "storage: schema fixture parses");
    const auto make_row = [](int id) {
        tuple::Tuple row;
        row.add(id);
        row.add(std::string(900, static_cast<char>('a' + id % 26)));
        return row;
    };

    bufman::BufferManager manager;
    std::string init_error;
    manager.init(8, std::make_unique<bufman::LRUPolicy>(), init_error);
    heapfile::HeapFile file(manager);
    check(file.create("storage.bin", schema, error) &&
              file.open("storage.bin", schema, error),
          "storage: file created and opened");
    check(file.check_structure(error),
          "storage: an empty file passes the structural check");

    // Fill two pages completely. Tail-append keeps the full list in
    // allocation order, so the bulk load scans in insertion order.
    std::vector<heapfile::RowId> live;
    std::vector<int> keys; // parallel to live: the row's sid
    bool placement_ok = true;
    for (int id = 1; id <= 8; ++id) {
        heapfile::RowId rid;
        placement_ok = placement_ok && file.insert(make_row(id), rid, error) &&
                       rid.page_id == static_cast<std::uint32_t>((id - 1) / 4 + 1) &&
                       rid.slot == static_cast<std::uint32_t>((id - 1) % 4);
        live.push_back(rid);
        keys.push_back(id);
    }
    check(placement_ok && file.check_structure(error),
          "storage: eight rows fill pages 1 and 2 in order");
    {
        std::vector<int> scanned;
        heapfile::HeapFileIterator it = file.scan();
        tuple::Tuple row;
        while (it.next(row, error)) {
            scanned.push_back(std::get<int>(row.column(0).column_value()));
        }
        check(error.empty() && scanned == keys,
              "storage: a bulk load scans in allocation order");
    }

    // Fill/unfill transitions: erase a row of the full page 1 -> the page
    // moves to the free list; the next insert reuses exactly that slot and
    // the page fills back into the full list.
    check(file.erase(live[0], error) && file.check_structure(error),
          "storage: erasing from a full page frees it onto the free list");
    heapfile::RowId rid;
    check(file.insert(make_row(9), rid, error) && rid.page_id == 1 &&
              rid.slot == 0 && file.check_structure(error),
          "storage: an insert reuses the freed slot of the oldest page");
    live[0] = rid;
    keys[0] = 9;

    // Empty pages never die: erase everything and the structure keeps both
    // pages on the free list; the next insert reuses instead of allocating.
    // (The pages reach disk only at close, so the on-disk proof comes later.)
    bool erased_all = true;
    for (const heapfile::RowId& row_id : live) {
        erased_all = erased_all && file.erase(row_id, error);
    }
    check(erased_all && error.empty() && file.check_structure(error),
          "storage: erasing everything leaves both pages on the free list");
    live.clear();
    keys.clear();
    check(file.insert(make_row(10), rid, error) && rid.page_id == 1 &&
              rid.slot == 0,
          "storage: the emptied file reuses a page instead of allocating");

    // A randomized insert/erase sequence, checked after every operation.
    // The shadow model (live + keys) tracks exactly which rows must exist.
    std::mt19937 rng(42);
    int next_id = 11;
    live.push_back(rid);
    keys.push_back(10);
    bool sequence_ok = true;
    for (int step = 0; step < 200; ++step) {
        if (rng() % 100 < 55 || live.empty()) {
            tuple::Tuple row = make_row(next_id);
            heapfile::RowId inserted_at;
            sequence_ok = sequence_ok &&
                          file.insert(row, inserted_at, error) &&
                          file.check_structure(error);
            live.push_back(inserted_at);
            keys.push_back(next_id);
            ++next_id;
        } else {
            const std::size_t victim = rng() % live.size();
            sequence_ok = sequence_ok && file.erase(live[victim], error) &&
                          file.check_structure(error);
            live.erase(live.begin() + static_cast<long>(victim));
            keys.erase(keys.begin() + static_cast<long>(victim));
        }
        if (!sequence_ok) {
            break;
        }
    }
    check(sequence_ok,
          "storage: 200 random operations keep the structure consistent");
    {
        std::vector<int> scanned;
        heapfile::HeapFileIterator it = file.scan();
        tuple::Tuple row;
        while (it.next(row, error)) {
            scanned.push_back(std::get<int>(row.column(0).column_value()));
        }
        std::vector<int> expected = keys;
        std::sort(scanned.begin(), scanned.end());
        std::sort(expected.begin(), expected.end());
        check(error.empty() && scanned == expected,
              "storage: the final scan returns exactly the shadow model's rows");
    }

    // A corrupted link must be reported, not silently obeyed: point page
    // 1's list-next at itself (the raw block layer, after a flush).
    check(file.close(error), "storage: file closed for the damage step");
    {
        // The close flushed the dirty pages: the data pages are on disk,
        // and erases never shrank the file below its first two pages.
        bufman::File raw;
        check(bufman::open_for_read("storage.bin", raw, error) &&
                  bufman::block_count(raw, error) >= 3 && error.empty(),
              "storage: the closed file keeps its data pages on disk");
        bufman::close(raw);
    }
    {
        bufman::File raw;
        check(bufman::open_for_append("storage.bin", raw, error),
              "storage: raw block access for the damage step");
        std::array<char, bufman::kBlockSize> block{};
        check(bufman::read_block(raw, 1, block, error),
              "storage: data page read back for the damage step");
        block[bufman::kBlockSize - 4] = 1; // list-next := self -> a cycle
        check(bufman::write_block(raw, 1, block.data(), block.size(), error),
              "storage: damaged page written back");
        bufman::close(raw);
    }
    {
        heapfile::HeapFile damaged(manager);
        check(damaged.open("storage.bin", schema, error),
              "storage: the damaged file still opens");
        check(!damaged.check_structure(error) &&
                  error.find("cyclic") != std::string::npos,
              "storage: a cyclic list is reported by the structural check");
        damaged.close(error);
    }

    check(file.open("storage.bin", schema, error),
          "storage: file reopened for cleanup");
    manager.close_all(error);
    std::filesystem::remove("storage.bin");
}

// --- schema and catalog: the vendored data-layout checks -----------------
//
// Tuple/Column construction, the record serializer (byte-for-byte), the
// JSON catalog loader, the writer with its indexes array and atomic
// install, CSV parsing and loading, and the deterministic row generator.
// Carried forward unchanged from the parent suites.

void schema_and_catalog_checks() {
    std::string error;
    // --- 1. Column construction: type and length mirror the value ---
    {
        const tuple::Column ci(42);
        check(ci.column_type() == tuple::ColumnType::Integer, "int column type");
        check(ci.column_length() == 4, "int column length is 4");
        check(std::get<int>(ci.column_value()) == 42, "int column value");

        const tuple::Column cd(3.14);
        check(cd.column_type() == tuple::ColumnType::Double, "double column type");
        check(cd.column_length() == 8, "double column length is 8");
        check(std::get<double>(cd.column_value()) == 3.14, "double column value");

        const tuple::Column cs(std::string("Ruth"));
        check(cs.column_type() == tuple::ColumnType::String, "string column type");
        check(cs.column_length() == 4, "string column length is its byte size");
        check(std::get<std::string>(cs.column_value()) == "Ruth", "string column value");

        const tuple::Column ce(std::string(""));
        check(ce.column_type() == tuple::ColumnType::String && ce.column_length() == 0,
              "empty string column has length 0");
    }

    // --- 2. Default column ---
    {
        const tuple::Column c;
        check(c.column_type() == tuple::ColumnType::Integer, "default column type");
        check(c.column_length() == 4, "default column length is 4");
        check(std::get<int>(c.column_value()) == 0, "default column value is 0");
    }

    // --- 3. Tuple API ---
    {
        tuple::Tuple t;
        check(t.column_count() == 0, "fresh tuple is empty");
        t.add(1);
        t.add(2.5);
        t.add(std::string("ab"));
        check(t.column_count() == 3, "add overloads build columns");
        check(t.column(0).column_type() == tuple::ColumnType::Integer &&
                  t.column(1).column_type() == tuple::ColumnType::Double &&
                  t.column(2).column_type() == tuple::ColumnType::String,
              "add overloads pick the right types");
        t.add_column(tuple::Column(9.5));
        check(t.column_count() == 4 &&
                  t.column(3).column_type() == tuple::ColumnType::Double,
              "add_column accepts a prebuilt Column");
        t.clear();
        check(t.column_count() == 0, "clear empties the tuple");
        bool threw = false;
        try {
            (void)t.column(99);
        } catch (const std::out_of_range&) {
            threw = true;
        }
        check(threw, "column(99) throws std::out_of_range");
    }

    // --- 4. Record serialization: values only, layout from the schema ---
    {
        schema::TableSchema person("");
        check(schema::load_table_schema("tablecatalog/person.json", person, error),
              "load person.json for the record tests");
        const schema::TupleSerializer serializer(person);
        tuple::Tuple t;
        t.add(42);
        t.add(std::string("Ruth"));
        t.add(7);
        t.add(std::string("HU"));
        std::array<char, 64> buf{};
        std::memset(buf.data(), 0x5A, buf.size());
        check(serializer.serialize(t, buf.data(), buf.size(), error),
              "serialize succeeds");
        check(error.empty(), "no error left behind on success");
        check(serializer.record_size() == 21,
              "record_size is 21 (4 + 10 + 4 + 3)");

        // Bytes past the record are untouched: max_len is a bound.
        bool marker_survives = true;
        for (std::size_t i = 21; i < buf.size(); ++i) {
            if (buf[i] != 0x5A) {
                marker_survives = false;
            }
        }
        check(marker_survives, "bytes past the record are untouched");

        // Round-trip over a non-empty target: old columns are gone.
        tuple::Tuple target = person.make_tuple();
        target.clear();
        target.add(999);
        target.add(std::string("stale"));
        target.add(999);
        target.add(std::string("stale"));
        check(serializer.deserialize(buf.data(), buf.size(), target, error),
              "deserialize over a non-empty tuple");
        check(target == t, "old columns are gone after deserialize");

        // The read needs the whole record; anything less refuses with the
        // out tuple empty.
        tuple::Tuple out;
        check(!serializer.deserialize(buf.data(), 20, out, error) &&
                  !error.empty() && out.column_count() == 0,
              "a truncated record fails");

        // Same for the write: the bound is checked before any byte moves.
        std::array<char, 20> small{};
        check(!serializer.serialize(t, small.data(), small.size(), error) &&
                  !error.empty(),
              "serialize refuses a too-small buffer");
    }

    // --- 5. Round-trips through the one serializer ---
    {
        schema::TableSchema person(""), car("");
        check(schema::load_table_schema("tablecatalog/person.json", person, error),
              "load person.json for the round-trips");
        check(schema::load_table_schema("tablecatalog/car.json", car, error),
              "load car.json for the round-trips");
        const auto round_trip = [&](const schema::TableSchema& schema,
                                    const tuple::Tuple& in) {
            const schema::TupleSerializer serializer(schema);
            std::array<char, 128> buf{};
            if (!serializer.serialize(in, buf.data(), buf.size(), error)) {
                return false;
            }
            tuple::Tuple out;
            if (!serializer.deserialize(buf.data(), buf.size(), out, error)) {
                return false;
            }
            return out == in;
        };
        tuple::Tuple short_str;
        short_str.add(1);
        short_str.add(std::string("Ruth"));
        short_str.add(42);
        short_str.add(std::string("HU"));
        check(round_trip(person, short_str), "round-trip: short strings");
        tuple::Tuple empty_str;
        empty_str.add(2);
        empty_str.add(std::string(""));
        empty_str.add(3);
        empty_str.add(std::string(""));
        check(round_trip(person, empty_str), "round-trip: empty strings");
        tuple::Tuple full_str;
        full_str.add(3);
        full_str.add(std::string("1234567890"));
        full_str.add(4);
        full_str.add(std::string("ABC"));
        check(round_trip(person, full_str), "round-trip: full-width strings");
        tuple::Tuple neg;
        neg.add(-5);
        neg.add(std::string("x"));
        neg.add(-2);
        neg.add(std::string("y"));
        check(round_trip(person, neg), "round-trip: negative ints");
        tuple::Tuple car_tuple;
        car_tuple.add(9);
        car_tuple.add(std::string("Volvo"));
        car_tuple.add(std::string("240"));
        car_tuple.add(1976);
        car_tuple.add(3);
        check(round_trip(car, car_tuple), "round-trip: a car record");
    }

    // --- 5b. The schema-bound TupleSerializer class ---
    {
        schema::TableSchema person(""), car(""), code_built("people");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the serializer class tests");
        check(schema::load_table_schema("tablecatalog/car.json", car, err),
              "load car.json for the serializer class tests");
        check(code_built.add_column("a", tuple::ColumnType::Integer, err) &&
                  code_built.add_column("b", tuple::ColumnType::String, err),
              "build a width-less schema for the serializer class tests");

        // The binding happens once at construction; every operation works
        // against the bound schema, with no per-call schema parameter.
        const schema::TupleSerializer serializer(person);
        check(serializer.record_size() == 21,
              "the bound serializer knows the record size (4 + 10 + 4 + 3)");

        tuple::Tuple t;
        t.add(7);
        t.add(std::string("Ruth"));
        t.add(42);
        t.add(std::string("HU"));

        // Byte-for-byte against the hand-built values-only layout: the
        // on-disk compatibility guard — these bytes are the format.
        std::array<char, 64> buf{};
        std::memset(buf.data(), 0x5A, buf.size());
        check(serializer.serialize(t, buf.data(), buf.size(), err),
              "the bound serializer packs the tuple");
        std::array<char, 21> expected{};
        const int pid = 7, age = 42;
        std::memcpy(expected.data() + 0, &pid, 4);
        std::memcpy(expected.data() + 4, "Ruth", 4);
        std::memcpy(expected.data() + 14, &age, 4);
        std::memcpy(expected.data() + 18, "HU", 2);
        check(std::memcmp(buf.data(), expected.data(), 21) == 0,
              "record bytes match the hand-built layout exactly");
        bool marker = true;
        for (std::size_t i = 21; i < buf.size(); ++i) {
            marker = marker && buf[i] == 0x5A;
        }
        check(marker, "bytes past the record are untouched");

        // Short strings are NUL-padded to their declared width.
        tuple::Tuple short_t;
        short_t.add(1);
        short_t.add(std::string("Ru"));
        short_t.add(2);
        short_t.add(std::string("H"));
        check(serializer.serialize(short_t, buf.data(), buf.size(), err),
              "pack the short-string tuple");
        bool padded = buf[4] == 'R' && buf[5] == 'u';
        for (std::size_t i = 6; i < 14; ++i) {
            padded = padded && buf[i] == '\0';
        }
        padded = padded && buf[18] == 'H' && buf[19] == '\0' && buf[20] == '\0';
        check(padded, "short strings are NUL-padded to their declared width");

        // Round trip through the bound serializer: old columns are gone.
        tuple::Tuple target = person.make_tuple();
        target.clear();
        target.add(999);
        target.add(std::string("stale"));
        target.add(999);
        target.add(std::string("stale"));
        check(serializer.deserialize(buf.data(), buf.size(), target, err),
              "unpack over a non-empty tuple");
        check(target == short_t, "the unpacked tuple replaces the old columns");

        // All-or-nothing pack: a tuple the schema refuses never touches the
        // buffer, and the bytes behind a refusal stay exactly as they were.
        std::memset(buf.data(), 0x5A, buf.size());
        tuple::Tuple arity;
        arity.add(1);
        arity.add(std::string("x"));
        check(!serializer.serialize(arity, buf.data(), buf.size(), err) &&
                  !err.empty(),
              "wrong arity is refused by the bound serializer");
        tuple::Tuple car_tuple;
        car_tuple.add(9);
        car_tuple.add(std::string("Volvo"));
        car_tuple.add(std::string("240"));
        car_tuple.add(1976);
        car_tuple.add(3);
        check(!serializer.serialize(car_tuple, buf.data(), buf.size(), err) &&
                  !err.empty(),
              "a tuple for another schema is refused by the binding");
        bool untouched = true;
        for (const char c : buf) {
            untouched = untouched && c == 0x5A;
        }
        check(untouched, "refused packs leave the buffer untouched");
        std::array<char, 20> small{};
        check(!serializer.serialize(t, small.data(), small.size(), err) &&
                  !err.empty(),
              "an undersized buffer is refused before any write");

        // All-or-nothing unpack: damage is a named error, the out tuple empty.
        check(serializer.serialize(t, buf.data(), buf.size(), err),
              "pack for the unpack refusal tests");
        buf[9] = 'X'; // name-field padding (value ends at 7, NUL at 8)
        tuple::Tuple out;
        check(!serializer.deserialize(buf.data(), buf.size(), out, err) &&
                  !err.empty() && err.find("padding") != std::string::npos &&
                  out.column_count() == 0,
              "garbage in the string padding is refused");
        check(!serializer.deserialize(buf.data(), 20, out, err) && !err.empty() &&
                  out.column_count() == 0,
              "a truncated record is refused");

        // A schema without declared widths has no record layout.
        const schema::TupleSerializer loose(code_built);
        check(loose.record_size() == 0, "undeclared widths: record_size is 0");
        tuple::Tuple cb;
        cb.add(1);
        cb.add(std::string("x"));
        check(!loose.serialize(cb, buf.data(), buf.size(), err) && !err.empty() &&
                  err.find("no declared width") != std::string::npos,
              "the undeclared-width schema is refused, naming the column");
    }

    // --- 6. Through the buffer pool ---
    {
        const std::string path = temp_path();
        bufman::BufferManager mgr;
        mgr.init(1, std::make_unique<bufman::LRUPolicy>(), error);
        bufman::File file;
        check(mgr.open_file(path, file, error), "open scratch file through the manager");

        schema::TableSchema person("");
        check(schema::load_table_schema("tablecatalog/person.json", person, error),
              "load person.json for the pool test");
        const schema::TupleSerializer serializer(person);
        tuple::Tuple out;
        out.add(7);
        out.add(std::string("persist"));
        out.add(2);
        out.add(std::string("XY"));

        std::size_t f;
        check(mgr.alloc_page(file, 0, f, error), "alloc_page block 0");
        check(serializer.serialize(out, mgr.frame(f).buffer,
                                   bufman::kPageSize, error),
              "serialize into the frame buffer");
        mgr.unpin(f, true, error);
        check(error.empty(), "unpin dirty");

        std::size_t g;
        check(mgr.alloc_page(file, 1, g, error),
              "alloc_page block 1 evicts the dirty frame");
        check(mgr.frame(g).buffer[0] == 0 && mgr.frame(g).dirty,
              "new page is zeroed and dirty");
        check(mgr.frame(g).pin_count == 1, "new page comes back pinned");
        mgr.unpin(g, false, error);
        check(error.empty(), "unpin clean");

        std::size_t h;
        check(mgr.pin(file, 0, h, error), "re-pin block 0 (miss: back from disk)");
        tuple::Tuple in;
        check(serializer.deserialize(mgr.frame(h).buffer,
                                     bufman::kPageSize, in, error),
              "deserialize from the frame buffer");
        check(in == out, "tuple survives the pool/disk path unchanged");

        mgr.close_all(error);
        std::remove(path.c_str());
    }

    // --- Schema test 1: ColumnSchema ---
    {
        const schema::ColumnSchema c("age", tuple::ColumnType::Integer);
        check(c.column_name() == "age" &&
                  c.column_type() == tuple::ColumnType::Integer &&
                  c.column_size() == 0,
              "ColumnSchema holds name, type, and declared size");
        const schema::ColumnSchema same("age", tuple::ColumnType::Integer);
        const schema::ColumnSchema other("age", tuple::ColumnType::String, 4);
        check(c == same && !(c == other), "ColumnSchema equality");
    }

    // --- Schema test 2: building a schema ---
    {
        schema::TableSchema people("people");
        check(people.table_name() == "people", "table name stored");
        std::string err;
        check(people.add_column("name", tuple::ColumnType::String, err), "add name");
        check(people.add_column("age", tuple::ColumnType::Integer, err), "add age");
        check(people.add_column("height", tuple::ColumnType::Double, err), "add height");
        check(people.column_count() == 3, "column_count grows");
        check(people.column_info(0).column_name() == "name" &&
                  people.column_info(0).column_type() == tuple::ColumnType::String,
              "column_info(0) is name");
        check(people.column_info(1).column_name() == "age" &&
                  people.column_info(1).column_type() == tuple::ColumnType::Integer,
              "column_info(1) is age");
        check(people.column_info(2).column_name() == "height" &&
                  people.column_info(2).column_type() == tuple::ColumnType::Double,
              "column_info(2) is height");
        check(!people.add_column("age", tuple::ColumnType::Integer, err) && !err.empty(),
              "duplicate column name fails");
        check(!people.add_column("", tuple::ColumnType::Integer, err) && !err.empty(),
              "empty column name fails");
        bool threw = false;
        try {
            (void)people.column_info(99);
        } catch (const std::out_of_range&) {
            threw = true;
        }
        check(threw, "column_info(99) throws std::out_of_range");
    }

    // --- Schema test 3: column_name_map ---
    {
        schema::TableSchema people("people");
        std::string err;
        people.add_column("name", tuple::ColumnType::String, err);
        people.add_column("age", tuple::ColumnType::Integer, err);
        const auto idx = people.column_index("age");
        check(idx.has_value() && *idx == 1, "column_index finds age at 1");
        check(!people.column_index("nope").has_value(),
              "unknown name returns nullopt");
    }

    // --- Schema test 4: validate ---
    {
        schema::TableSchema people("people");
        std::string err;
        people.add_column("name", tuple::ColumnType::String, err);
        people.add_column("age", tuple::ColumnType::Integer, err);
        people.add_column("height", tuple::ColumnType::Double, err);
        check(people.validate(people.make_tuple(), err),
              "a make_tuple product validates");
        tuple::Tuple one;
        one.add(1);
        check(!people.validate(one, err) && !err.empty(),
              "wrong column count fails");
        tuple::Tuple wrong_type;
        wrong_type.add(std::string("Ruth"));
        wrong_type.add(2.5); // slot 1 must be Integer
        wrong_type.add(0.0);
        check(!people.validate(wrong_type, err) && !err.empty(),
              "wrong type at a position fails");
        check(err.find("1") != std::string::npos, "error names the position");
    }

    // --- Schema test 5: make_tuple ---
    {
        schema::TableSchema people("people");
        std::string err;
        people.add_column("name", tuple::ColumnType::String, err);
        people.add_column("age", tuple::ColumnType::Integer, err);
        people.add_column("height", tuple::ColumnType::Double, err);
        const tuple::Tuple t = people.make_tuple();
        check(t.column_count() == 3, "make_tuple column count");
        check(t.column(0).column_type() == tuple::ColumnType::String &&
                  std::get<std::string>(t.column(0).column_value()).empty(),
              "slot 0 defaults to an empty string");
        check(t.column(1).column_type() == tuple::ColumnType::Integer &&
                  std::get<int>(t.column(1).column_value()) == 0,
              "slot 1 defaults to 0");
        check(t.column(2).column_type() == tuple::ColumnType::Double &&
                  std::get<double>(t.column(2).column_value()) == 0.0,
              "slot 2 defaults to 0.0");
    }

    // --- Schema test 6: creation order, byte-for-byte ---
    {
        schema::TableSchema people("people");
        std::string err;
        people.add_column("name", tuple::ColumnType::String, err);
        people.add_column("age", tuple::ColumnType::Integer, err);
        people.add_column("height", tuple::ColumnType::Double, err);
        // Code-built schemas have no declared widths — load one that does.
        schema::TableSchema person("");
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the byte test");
        const schema::TupleSerializer serializer(person);
        tuple::Tuple logical;
        logical.add(7);
        logical.add(std::string("Ruth"));
        logical.add(42);
        logical.add(std::string("HU"));
        check(person.validate(logical, err), "hand-built logical tuple validates");
        std::array<char, 64> got{};
        check(serializer.serialize(logical, got.data(), got.size(), err),
              "serialize in creation order");
        // Hand-built values-only layout: pid@0, name@4 padded to 10, age@14,
        // city@18 padded to 3.
        std::array<char, 21> expected{};
        const int pid = 7, age = 42;
        std::memcpy(expected.data() + 0, &pid, 4);
        std::memcpy(expected.data() + 4, "Ruth", 4);
        std::memcpy(expected.data() + 14, &age, 4);
        std::memcpy(expected.data() + 18, "HU", 2);
        check(std::memcmp(got.data(), expected.data(), 21) == 0,
              "creation order: bytes match the value layout exactly");
    }

    // --- Schema test 7: value-layout corruption is caught on read ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the corruption test");
        const schema::TupleSerializer serializer(person);
        tuple::Tuple logical;
        logical.add(7);
        logical.add(std::string("Ruth"));
        logical.add(42);
        logical.add(std::string("HU"));
        std::array<char, 64> buf{};
        check(serializer.serialize(logical, buf.data(), buf.size(), err),
              "serialize for the padding corruption");

        // Byte 9 is name-field padding (value ends at 7, NUL at 8). With no
        // tags to validate, the padding check is what catches damage.
        buf[9] = 'X';
        tuple::Tuple out;
        check(!serializer.deserialize(buf.data(), buf.size(), out, err) &&
                  !err.empty() && out.column_count() == 0,
              "garbage in the string padding is refused");
    }

    // --- Schema test 8: failure discipline on serialize ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the refusal test");
        const schema::TupleSerializer serializer(person);
        tuple::Tuple wrong;
        wrong.add(1);
        std::array<char, 64> buf{};
        check(!serializer.serialize(wrong, buf.data(), buf.size(), err) &&
                  !err.empty(),
              "the serializer refuses a mismatched tuple");
    }

    // --- Schema test 9: by-name workflow ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the by-name workflow");
        const schema::TupleSerializer serializer(person);

        const std::size_t age_slot = *person.column_index("age");
        check(age_slot == 2, "by-name mapping lands on tuple slot 2");

        // Tuple columns are immutable values: filling by name means
        // rebuilding with the named slot replaced.
        tuple::Tuple filled;
        const tuple::Tuple fresh = person.make_tuple();
        for (std::size_t p = 0; p < fresh.column_count(); ++p) {
            filled.add_column(p == age_slot ? tuple::Column(42) : fresh.column(p));
        }
        check(person.validate(filled, err), "filled tuple validates");

        std::array<char, 64> buf{};
        check(serializer.serialize(filled, buf.data(), buf.size(), err),
              "serialize the by-name-filled tuple");
        tuple::Tuple back;
        check(serializer.deserialize(buf.data(), buf.size(), back, err),
              "deserialize it back");
        check(std::get<int>(back.column(age_slot).column_value()) == 42,
              "read 'age' back by name");
    }

    // --- Catalog test 1: loading person.json ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load tablecatalog/person.json");
        check(err.empty(), "no error left behind on a good catalog");
        check(person.table_name() == "person", "table name is 'person'");
        check(person.column_count() == 4, "four columns");
        const std::vector<std::string> names = {"pid", "name", "age", "city"};
        const std::vector<tuple::ColumnType> types = {
            tuple::ColumnType::Integer, tuple::ColumnType::String,
            tuple::ColumnType::Integer, tuple::ColumnType::String};
        const std::vector<std::size_t> sizes = {4, 10, 4, 3};
        bool names_ok = true, types_ok = true, sizes_ok = true;
        for (std::size_t i = 0; i < person.column_count(); ++i) {
            const schema::ColumnSchema& c = person.column_info(i);
            names_ok = names_ok && c.column_name() == names[i];
            types_ok = types_ok && c.column_type() == types[i];
            sizes_ok = sizes_ok && c.column_size() == sizes[i];
        }
        check(names_ok, "column names in file order");
        check(types_ok, "column types match the catalog");
        check(sizes_ok, "declared sizes match the catalog");
        check(person.column_index("age").has_value() &&
                  !person.column_index("nope").has_value(),
              "column_name_map works on a loaded schema");
        check(person.validate(person.make_tuple(), err),
              "a make_tuple product of the loaded schema validates");
    }

    // --- Catalog test 2: the catalog matches the hardwired Person layout ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the layout check");
        std::size_t total = 0;
        for (std::size_t i = 0; i < person.column_count(); ++i) {
            total += person.column_info(i).column_size();
        }
        check(total == 21,  // the legacy record width: 4 + 10 + 4 + 3
              "declared sizes sum to the legacy record width");
    }

    // --- Catalog test 3: loading car.json ---
    {
        schema::TableSchema car("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/car.json", car, err),
              "load tablecatalog/car.json");
        check(car.table_name() == "car" && car.column_count() == 5,
              "five columns");
        const std::vector<std::size_t> sizes = {4, 15, 10, 4, 4};
        bool sizes_ok = true;
        for (std::size_t i = 0; i < car.column_count(); ++i) {
            sizes_ok = sizes_ok && car.column_info(i).column_size() == sizes[i];
        }
        check(sizes_ok, "declared sizes 4/15/10/4/4");
        check(car.validate(car.make_tuple(), err), "a car tuple validates");
    }

    // --- Catalog test 4: the loaded schema drives the wire ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the wire test");
        const schema::TupleSerializer serializer(person);
        tuple::Tuple t;
        t.add(7);
        t.add(std::string("Ruth"));
        t.add(42);
        t.add(std::string("HU"));
        check(person.validate(t, err), "a hand-built person tuple validates");
        std::array<char, 64> buf{};
        check(serializer.serialize(t, buf.data(), buf.size(), err),
              "serialize a person tuple over the loaded schema");
        tuple::Tuple back;
        check(serializer.deserialize(buf.data(), buf.size(), back, err),
              "deserialize it back");
        check(back == t, "round-trip over the loaded schema");
        const std::size_t age_slot = *person.column_index("age");
        check(std::get<int>(back.column(age_slot).column_value()) == 42,
              "read 'age' back by name");
    }

    // --- Catalog test 5: overwrite semantics ---
    {
        const std::string person_text =
            "{\n"
            "  \"table_name\": \"person\",\n"
            "  \"columns\": [\n"
            "    { \"column_name\": \"pid\",  \"column_type\": \"Integer\", \"size\": 4 },\n"
            "    { \"column_name\": \"name\", \"column_type\": \"String\",  \"size\": 10 },\n"
            "    { \"column_name\": \"age\",  \"column_type\": \"Integer\", \"size\": 4 },\n"
            "    { \"column_name\": \"city\", \"column_type\": \"String\",  \"size\": 3 }\n"
            "  ]\n"
            "}\n";
        schema::TableSchema target("stale");
        std::string err;
        check(target.add_column("junk", tuple::ColumnType::Double, err),
              "stale column in place");
        check(schema::parse_table_schema(person_text, target, err),
              "parse over a non-empty schema");
        check(target.table_name() == "person" && target.column_count() == 4 &&
                  target.column_info(0).column_name() == "pid",
              "old columns are gone after a successful parse");
        check(!schema::parse_table_schema("{", target, err) && !err.empty() &&
                  target.column_count() == 0 && target.table_name().empty(),
              "a failed parse leaves the out-schema empty");
    }

    // --- Catalog test 6: parse-level errors name the line ---
    {
        schema::TableSchema out("");
        std::string err;
        check(!schema::parse_table_schema("", out, err) && !err.empty() &&
                  err.find("line 1") != std::string::npos,
              "empty text fails, naming line 1");
        check(!schema::parse_table_schema("{\n  \"table_name\": true\n}", out, err) &&
                  !err.empty() && err.find("line 2") != std::string::npos,
              "true as a value fails, naming line 2");
        check(!schema::parse_table_schema("{\"table_name\": \"per", out, err) &&
                  !err.empty(),
              "unterminated string fails");
        check(!schema::parse_table_schema("{\"table_name\": \"a\\qb\"}", out, err) &&
                  !err.empty(),
              "unknown escape fails");
        check(!schema::parse_table_schema(
                  "{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                  "\"column_type\": \"Integer\", \"size\": 4.5}]}",
                  out, err) &&
                  !err.empty(),
              "floating-point size fails");
        check(!schema::parse_table_schema("{} ", out, err) && !err.empty(),
              "missing keys fail");
        check(!schema::parse_table_schema(
                  "{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                  "\"column_type\": \"Integer\", \"size\": 4}]} x",
                  out, err) &&
                  !err.empty() && err.find("after") != std::string::npos,
              "trailing garbage fails");
    }

    // --- Catalog test 7: content errors name the key or column ---
    {
        schema::TableSchema out("");
        std::string err;
        const auto fails = [&](const std::string& text) {
            err.clear();
            return !schema::parse_table_schema(text, out, err) && !err.empty() &&
                   out.column_count() == 0;
        };
        check(fails("{\"columns\": []}"), "missing 'table_name' fails");
        check(fails("{\"table_name\": \"\", \"columns\": [{\"column_name\": \"a\", "
                    "\"column_type\": \"Integer\", \"size\": 4}]}"),
              "empty 'table_name' fails");
        check(fails("{\"table_name\": \"t\"}"), "missing 'columns' fails");
        check(fails("{\"table_name\": \"t\", \"columns\": 3}"),
              "'columns' not an array fails");
        check(fails("{\"table_name\": \"t\", \"columns\": []}"),
              "empty columns array fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [3]}"),
              "column entry not an object fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_type\": "
                    "\"Integer\", \"size\": 4}]}"),
              "missing 'column_name' fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                    "\"size\": 4}]}"),
              "missing 'column_type' fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                    "\"column_type\": \"Integer\"}]}"),
              "missing 'size' fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                    "\"column_type\": \"Float\", \"size\": 4}]}") &&
                  err.find("Float") != std::string::npos,
              "unknown type spelling fails, naming it");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                    "\"column_type\": \"Integer\", \"size\": 0}]}"),
              "size 0 fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"a\", "
                    "\"column_type\": \"Integer\", \"size\": 4}, "
                    "{\"column_name\": \"a\", \"column_type\": \"String\", "
                    "\"size\": 2}]}") &&
                  err.find("already exists") != std::string::npos,
              "duplicate column name fails with add_column's message");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"column_name\": \"\", "
                    "\"column_type\": \"Integer\", \"size\": 4}]}"),
              "empty column name fails");
        check(fails("{\"table_name\": \"t\", \"columns\": [{\"nope\": 1}]}"),
              "unexpected key fails");
    }

    // --- Catalog test 8: failure discipline across bad catalogs ---
    {
        const std::vector<std::pair<std::string, std::string>> bad = {
            {"", "empty text"},
            {"not json", "not json"},
            {"[1, 2]", "array instead of an object"},
            {"{", "truncated"},
            {"{\"table_name\": \"t\"}", "missing columns"},
            {"{\"table_name\": \"t\", \"columns\": [{}]}", "empty column entry"},
        };
        bool all_refused = true;
        schema::TableSchema out("");
        std::string err;
        for (const auto& entry : bad) {
            if (schema::parse_table_schema(entry.first, out, err) || err.empty() ||
                out.column_count() != 0) {
                all_refused = false;
            }
        }
        check(all_refused, "every bad catalog: refused, error set, out empty");
    }

    // --- Catalog test 9: missing file ---
    {
        schema::TableSchema out("");
        std::string err;
        check(!schema::load_table_schema("tablecatalog/no_such_table.json", out,
                                         err) &&
                  !err.empty() &&
                  err.find("no_such_table.json") != std::string::npos,
              "a missing file fails, naming the path");
        check(out.column_count() == 0, "the out-schema stays empty");
    }

    // --- CLI module test 1: CSV row parsing ---
    {
        schema::TableSchema person(""), code_built("mixed");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the CSV row tests");
        check(code_built.add_column("a", tuple::ColumnType::Integer, err) &&
                  code_built.add_column("b", tuple::ColumnType::Double, err) &&
                  code_built.add_column("c", tuple::ColumnType::String, err),
              "build a schema with a Double column");

        tuple::Tuple t;
        check(schema::parse_csv_row(person, "7,Ruth,42,HU", t, err),
              "a valid row parses");
        tuple::Tuple expected;
        expected.add(7);
        expected.add(std::string("Ruth"));
        expected.add(42);
        expected.add(std::string("HU"));
        check(t == expected, "the parsed tuple matches the row");

        check(schema::parse_csv_row(person, "1,,2,", t, err) &&
                  std::get<std::string>(t.column(1).column_value()) == "" &&
                  std::get<std::string>(t.column(3).column_value()) == "",
              "empty tokens are empty strings");
        check(schema::parse_csv_row(person, "3,1234567890,4,ABC", t, err),
              "a full-width string parses");

        check(!schema::parse_csv_row(person, "7,Ruth", t, err) &&
                  !err.empty() && err.find("2 column(s)") != std::string::npos,
              "too few tokens are refused, naming the arity");
        check(!schema::parse_csv_row(person, "7,Ruth,42,HU,x", t, err) &&
                  !err.empty(),
              "too many tokens are refused");
        check(!schema::parse_csv_row(person, "abc,Ruth,42,HU", t, err) &&
                  !err.empty() &&
                  err.find("column 'pid': expected an integer") !=
                      std::string::npos,
              "a bad integer names its column");
        check(!schema::parse_csv_row(person, "7,Ruth,4.5,HU", t, err) &&
                  !err.empty(),
              "a fractional age is refused");

        check(schema::parse_csv_row(code_built, "1,2.5,x", t, err),
              "a Double column parses");
        check(!schema::parse_csv_row(code_built, "1,2.5.3,x", t, err) &&
                  !err.empty() &&
                  err.find("column 'b': expected a number") !=
                      std::string::npos,
              "a bad double names its column");
        check(!schema::parse_csv_row(code_built, "1,,x", t, err) &&
                  !err.empty(),
              "an empty numeric token is refused");
    }

    // --- CLI module test 2: width rules surface at insert, not parse ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the width test");
        tuple::Tuple t;
        check(schema::parse_csv_row(person, "8,12345678901,9,AA", t, err),
              "a too-wide string parses fine (syntax is legal)");

        bufman::BufferManager mgr;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>(), error);
        const std::string path = temp_path() + "-csv-width";
        heapfile::HeapFile file(mgr);
        check(file.create(path, person, err) && file.open(path, person, err),
              "create and open for the width test");
        heapfile::RowId rid;
        check(!file.insert(t, rid, err) && !err.empty() &&
                  err.find("exceeds the declared width") != std::string::npos,
              "insert refuses it with the schema's named error");
        file.close(err);
        std::remove(path.c_str());
    }

    // --- CLI module test 3: CSV file loading ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the CSV file test");
        const std::string csv_path = temp_path() + "-load.csv";
        {
            std::ofstream out(csv_path, std::ios::binary);
            out << "pid,name,age,city\r\n";      // header, CRLF ending
            out << "1,Alice,30,PR\r\n";          // valid
            out << "\r\n";                       // blank line: silent skip
            out << "2,Bob,old,NY\n";             // bad age
            out << "3,Cara,21,PARIS\n";          // too-wide city
            out << "4,Dan,19,HU\n";              // valid
        }
        std::ostringstream diagnostics;
        schema::CsvLoadResult result;
        check(schema::load_csv(person, csv_path, result, diagnostics, err),
              "load_csv reads the file");
        // The too-wide city row parses fine (width is insert-time), so the
        // load yields 3 valid rows and skips the header and the bad age.
        check(result.tuples.size() == 3 && result.skipped == 2,
              "3 valid rows, 2 skipped");
        check(std::get<std::string>(result.tuples[0].column(1).column_value()) ==
                      "Alice" &&
                  std::get<std::string>(result.tuples[1].column(1).column_value()) ==
                      "Cara" &&
                  std::get<std::string>(result.tuples[2].column(1).column_value()) ==
                      "Dan",
              "the valid rows come back in file order");
        const std::string text = diagnostics.str();
        check(text.find("line 1: column 'pid': expected an integer") !=
                      std::string::npos &&
                  text.find("line 4: column 'age': expected an integer") !=
                      std::string::npos,
              "skipped rows are reported as line N: reason");

        check(!schema::load_csv(person, temp_path() + "-no-such.csv", result,
                                diagnostics, err) &&
                  !err.empty() && result.tuples.empty(),
              "a missing file is a named error with an empty result");
        std::remove(csv_path.c_str());
    }

    // --- CLI module test 4: generation ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the generator tests");
        const std::vector<tuple::Tuple> a = schema::generate_tuples(person, 10, 42);
        const std::vector<tuple::Tuple> b = schema::generate_tuples(person, 10, 42);
        const std::vector<tuple::Tuple> c = schema::generate_tuples(person, 10, 43);
        check(a == b, "same seed produces identical tuples");
        check(!(a == c), "a different seed produces different values");
        check(a.size() == 10, "the count is exact");
        bool all_valid = true;
        for (const tuple::Tuple& t : a) {
            all_valid = all_valid && person.validate(t, err);
        }
        check(all_valid, "every generated tuple passes validate");
    }

    // --- CLI module test 5: generator conventions ---
    {
        schema::TableSchema person("");
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person, err),
              "load person.json for the convention tests");
        const schema::TupleSerializer serializer(person);
        const std::vector<tuple::Tuple> tuples =
            schema::generate_tuples(person, 5, 7, 100);
        bool sequence_ok = true;
        bool strings_ok = true;
        for (std::size_t i = 0; i < tuples.size(); ++i) {
            sequence_ok = sequence_ok &&
                          std::get<int>(tuples[i].column(0).column_value()) ==
                              100 + static_cast<int>(i);
            const std::string& name =
                std::get<std::string>(tuples[i].column(1).column_value());
            strings_ok = strings_ok && !name.empty() && name.size() <= 10 &&
                         name.find('\0') == std::string::npos;
        }
        check(sequence_ok, "the first Integer column counts first_id upward");
        check(strings_ok, "strings stay within their declared width, no NUL");
        std::array<char, 64> buf{};
        check(serializer.serialize(tuples[0], buf.data(), buf.size(), err),
              "serialize a generated tuple");
        tuple::Tuple back;
        check(serializer.deserialize(buf.data(), buf.size(), back, err) &&
                  back == tuples[0],
              "generated rows round-trip through the serializer");
    }
    // --- ex13: catalog indexes — loader extension and the writer ---
    {
        schema::TableSchema person("");
        std::vector<schema::TableIndex> indexes;
        std::string err;
        check(schema::load_table_schema("tablecatalog/person.json", person,
                                        indexes, err),
              "load person.json through the indexes overload");
        check(indexes.empty(),
              "a catalog without 'indexes' loads with an empty index list");
        check(person.table_name() == "person" && person.column_count() == 4,
              "back-compat: person.json loads unchanged");

        schema::TableSchema parsed("");
        std::vector<schema::TableIndex> parsed_indexes;
        check(schema::parse_table_schema(
                  "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
                  "\"column_type\":\"Integer\",\"size\":4}],"
                  "\"indexes\":[{\"name\":\"t_a\",\"column\":\"a\"}]}",
                  parsed, parsed_indexes, err) &&
                  parsed_indexes.size() == 1 &&
                  parsed_indexes[0].name == "t_a" &&
                  parsed_indexes[0].column == "a",
              "an indexes array parses into name/column pairs");
        check(schema::parse_table_schema(
                  "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
                  "\"column_type\":\"Integer\",\"size\":4}],\"indexes\":[]}",
                  parsed, parsed_indexes, err) &&
                  parsed_indexes.empty(),
              "an empty indexes array parses to no indexes");

        const char* bad_catalogs[] = {
            "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
            "\"column_type\":\"Integer\",\"size\":4}],"
            "\"indexes\":[{\"name\":\"i\"}]}",
            "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
            "\"column_type\":\"Integer\",\"size\":4}],"
            "\"indexes\":[{\"column\":\"a\"}]}",
            "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
            "\"column_type\":\"Integer\",\"size\":4}],"
            "\"indexes\":[{\"name\":\"\",\"column\":\"a\"}]}",
            "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
            "\"column_type\":\"Integer\",\"size\":4}],"
            "\"indexes\":[{\"name\":\"i\",\"column\":\"a\"},"
            "{\"name\":\"i\",\"column\":\"a\"}]}",
            "{\"table_name\":\"t\",\"columns\":[{\"column_name\":\"a\","
            "\"column_type\":\"Integer\",\"size\":4}],"
            "\"indexes\":[{\"name\":\"i\",\"column\":\"a\",\"extra\":1}]}",
        };
        for (const char* bad : bad_catalogs) {
            schema::TableSchema bad_schema("");
            std::vector<schema::TableIndex> bad_indexes;
            std::string bad_error;
            check(!schema::parse_table_schema(bad, bad_schema, bad_indexes,
                                              bad_error) &&
                      bad_error.rfind("line ", 0) == 0 &&
                      bad_indexes.empty(),
                  "malformed index entry refused with a named error");
        }

        // The writer: round-trip through a temp catalog file.
        const std::string catalog_path = temp_path() + "-catalog.json";
        std::remove(catalog_path.c_str());
        schema::TableSchema written("");
        std::vector<schema::TableIndex> written_indexes;
        check(schema::save_table_schema(catalog_path, person,
                                        {{"person_pid", "pid"}}, err),
              "save_table_schema writes a catalog with an index");
        check(schema::load_table_schema(catalog_path, written,
                                        written_indexes, err) &&
                  written.table_name() == "person" &&
                  written.column_count() == person.column_count(),
              "a saved catalog loads again");
        bool columns_match = written.column_count() == person.column_count();
        for (std::size_t i = 0; columns_match && i < written.column_count(); ++i) {
            columns_match = written.column_info(i).column_name() ==
                                person.column_info(i).column_name() &&
                            written.column_info(i).column_type() ==
                                person.column_info(i).column_type() &&
                            written.column_info(i).column_size() ==
                                person.column_info(i).column_size();
        }
        check(columns_match, "saved columns round-trip exactly");
        check(written_indexes.size() == 1 &&
                  written_indexes[0].name == "person_pid" &&
                  written_indexes[0].column == "pid",
              "saved indexes round-trip exactly");
        check(schema::save_table_schema(catalog_path, person, {}, err),
              "saving with no indexes overwrites cleanly");
        check(schema::load_table_schema(catalog_path, written,
                                        written_indexes, err) &&
                  written_indexes.empty(),
              "the overwritten catalog has no indexes");

        // A refused save leaves the installed catalog byte-for-byte.
        std::ifstream before(catalog_path, std::ios::binary);
        std::ostringstream before_text;
        before_text << before.rdbuf();
        before.close();
        check(!schema::save_table_schema(catalog_path, person,
                                         {{"bad", "nosuch"}}, err) &&
                  !err.empty(),
              "an index naming an unknown column refuses to save");
        std::ifstream after(catalog_path, std::ios::binary);
        std::ostringstream after_text;
        after_text << after.rdbuf();
        after.close();
        check(before_text.str() == after_text.str(),
              "a failed save leaves the catalog unchanged");
        check(std::remove(catalog_path.c_str()) == 0,
              "catalog temp file cleaned up");
    }

}

} // namespace

int run_storage_checks() {
    schema_and_catalog_checks();
    list_case_checks();
    storage_checks();
    return failures;
}
