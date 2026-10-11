// p3dbeng — the loader suite: the TUP2 bulk loader's fill/link read-back,
// exact-division generation order, partial-tail placement, refusal rules,
// generator-failure naming, empty and zero-record loads, the differential
// against per-row insertion, and the timing contract.

#include "bufpool/BufferManager.h"
#include "file/BlockFile.h"
#include "heapfile/HeapFile.h"
#include "policy/LRUPolicy.h"
#include "schema/TableGenerator.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"
#include "schema/TupleSerializer.h"
#include "tools/BulkLoader.h"
#include "tuple/Tuple.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "PASS: " << message << "\n";
    } else {
        std::cout << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string temp_path(const char* suffix) {
    return std::string("/tmp/p3-load-") + std::to_string(getpid()) + "-" +
           suffix + ".bin";
}

// A wide-record schema: sid(4) + pad(900) = 904 bytes, 4 slots per page —
// the same fixture the storage suite uses, so page math stays familiar.
bool wide_schema(schema::TableSchema& schema, std::string& error) {
    return schema::parse_table_schema(
        R"({"table_name": "bulkcase",
            "columns": [
              {"column_name": "sid", "column_type": "Integer", "size": 4},
              {"column_name": "pad", "column_type": "String",  "size": 900}]})",
        schema, error);
}

tuple::Tuple wide_row(int id) {
    tuple::Tuple row;
    row.add(id);
    row.add(std::string(900, static_cast<char>('a' + id % 26)));
    return row;
}

// The ids a scan reports, in scan order.
std::vector<int> scan_ids(heapfile::HeapFile& file, std::string& error) {
    std::vector<int> ids;
    heapfile::HeapFileIterator it = file.scan();
    tuple::Tuple row;
    while (it.next(row, error)) {
        ids.push_back(std::get<int>(row.column(0).column_value()));
    }
    return ids;
}

}  // namespace

int run_loader_checks() {
    std::string error;

    // --- fill, link, and read back through the engine ---
    {
        schema::TableSchema schema("");
        check(wide_schema(schema, error), "the wide fixture schema parses");
        const std::string path = temp_path("fill");
        std::remove(path.c_str());
        tools::BulkLoadStats stats;
        const bool loaded = tools::bulk_load(
            path, schema, 250,
            [](std::size_t index, tuple::Tuple& out, std::string&) {
                out = wide_row(static_cast<int>(index) + 1);
                return true;
            },
            stats, error);
        check(loaded && stats.records == 250 && stats.pages == 63 &&
                  stats.per_page_capacity == 4,
              "250 rows fill 63 pages at 4 slots (62 full, 1 partial)");
        error.clear();

        bufman::BufferManager manager;
        std::string init_error;
        manager.init(8, std::make_unique<bufman::LRUPolicy>(), init_error);
        heapfile::HeapFile file(manager);
        check(file.open(path, schema, error),
              "the loaded file opens through the engine");
        check(file.check_structure(error),
              "the loaded file passes the structural self-check");
        std::vector<int> ids = scan_ids(file, error);
        std::vector<int> sorted_ids = ids;
        std::sort(sorted_ids.begin(), sorted_ids.end());
        std::vector<int> expected(250);
        for (int i = 0; i < 250; ++i) {
            expected[i] = i + 1;
        }
        check(error.empty() && ids.size() == 250 && sorted_ids == expected,
              "every loaded record scans back exactly once (order per the\n"
              "       heap's two-list rule: the partial tail page scans first)");
        heapfile::RowId probe{2, 1};
        tuple::Tuple fetched;
        check(file.find(probe, fetched, error) &&
                  std::get<int>(fetched.column(0).column_value()) == 6,
              "a reported row id resolves to its record");
        check(file.close(error), "the loaded file closed");
        manager.close_all(error);
        std::remove(path.c_str());
    }

    // --- exact division: generation order across the whole file ---
    {
        schema::TableSchema schema("");
        check(wide_schema(schema, error), "the wide fixture schema parses");
        const std::string path = temp_path("exact");
        std::remove(path.c_str());
        tools::BulkLoadStats stats;
        check(tools::bulk_load(
                  path, schema, 12,
                  [](std::size_t index, tuple::Tuple& out, std::string&) {
                      out = wide_row(static_cast<int>(index) + 1);
                      return true;
                  },
                  stats, error) &&
                  stats.pages == 3,
              "12 rows fill exactly 3 pages");
        error.clear();
        bufman::BufferManager manager;
        std::string init_error;
        manager.init(8, std::make_unique<bufman::LRUPolicy>(), init_error);
        heapfile::HeapFile file(manager);
        check(file.open(path, schema, error) && file.check_structure(error),
              "the exact-division file opens and is consistent");
        std::vector<int> ids = scan_ids(file, error);
        std::vector<int> expected(12);
        for (int i = 0; i < 12; ++i) {
            expected[i] = i + 1;
        }
        check(ids == expected,
              "an exact-division load scans in generation order");
        check(file.close(error), "exact-division file closed");
        manager.close_all(error);
        std::remove(path.c_str());
    }

    // --- the partial tail page sits on the free list ---
    {
        schema::TableSchema schema("");
        check(wide_schema(schema, error), "the wide fixture schema parses");
        const std::string path = temp_path("partial");
        std::remove(path.c_str());
        tools::BulkLoadStats stats;
        check(tools::bulk_load(
                  path, schema, 10,
                  [](std::size_t index, tuple::Tuple& out, std::string&) {
                      out = wide_row(static_cast<int>(index) + 1);
                      return true;
                  },
                  stats, error) &&
                  stats.pages == 3,
              "10 rows fill 2 pages plus a 2-record tail");
        error.clear();
        // Raw header read: free list holds page 3, full list 1 -> 2.
        {
            bufman::File raw;
            check(bufman::open_for_read(path, raw, error),
                  "raw access for the header check");
            std::array<char, bufman::kBlockSize> header{};
            check(bufman::read_block(raw, 0, header.data(), header.size(),
                                     error),
                  "the header page read back");
            auto u32 = [&](std::size_t offset) {
                std::uint32_t value = 0;
                std::memcpy(&value, header.data() + offset, 4);
                return value;
            };
            constexpr std::uint32_t kNoPage = 0xFFFFFFFFu;
            check(u32(12) == 1 && u32(16) == 2,
                  "the full list is 1 -> 2 in generation order");
            check(u32(4) == 3 && u32(8) == 3,
                  "the partial tail page is the free list's only element");
            check(u32(20) == 4, "next_page_id covers every written page");
            bufman::close(raw);
        }
        bufman::BufferManager manager;
        std::string init_error;
        manager.init(8, std::make_unique<bufman::LRUPolicy>(), init_error);
        heapfile::HeapFile file(manager);
        check(file.open(path, schema, error) && file.check_structure(error),
              "the partial-tail file opens and is consistent");
        // The engine's scan-order rule: the free list first, then the
        // full list — the tail page's rows come first here.
        std::vector<int> ids = scan_ids(file, error);
        check(error.empty() && ids.size() == 10 && ids[0] == 9 && ids[1] == 10,
              "the partial tail page's rows scan first, as the heap scans");
        // A later insert lands in the freed-tail page without growing.
        heapfile::RowId rid;
        check(file.insert(wide_row(11), rid, error) && rid.page_id == 3,
              "a later insert reuses the partial tail page");
        check(file.check_structure(error),
              "the structure stays consistent after the refill");
        check(file.close(error), "partial-tail file closed");
        manager.close_all(error);
        std::remove(path.c_str());
    }

    // --- refusals ---
    {
        schema::TableSchema schema("");
        check(wide_schema(schema, error), "the wide fixture schema parses");

        // A non-empty file is refused, content unchanged.
        const std::string path = temp_path("refuse-full");
        std::remove(path.c_str());
        tools::BulkLoadStats stats;
        check(tools::bulk_load(
                  path, schema, 4,
                  [](std::size_t index, tuple::Tuple& out, std::string&) {
                      out = wide_row(static_cast<int>(index) + 1);
                      return true;
                  },
                  stats, error),
              "the refusal fixture loads once");
        error.clear();
        std::string refuse_error;
        check(!tools::bulk_load(
                  path, schema, 4,
                  [](std::size_t index, tuple::Tuple& out, std::string&) {
                      out = wide_row(static_cast<int>(index) + 100);
                      return true;
                  },
                  stats, refuse_error) &&
                  !refuse_error.empty(),
              "a non-empty file is refused");
        {
            bufman::BufferManager manager;
            std::string init_error;
            manager.init(4, std::make_unique<bufman::LRUPolicy>(),
                         init_error);
            heapfile::HeapFile file(manager);
            check(file.open(path, schema, error),
                  "the refused file still opens");
            std::vector<int> ids = scan_ids(file, error);
            check(ids.size() == 4 && ids[0] == 1,
                  "the refused file's records are unchanged");
            file.close(error);
            manager.close_all(error);
        }
        std::remove(path.c_str());

        // A foreign one-block header is refused by name.
        const std::string foreign = temp_path("refuse-foreign");
        std::remove(foreign.c_str());
        {
            bufman::File raw;
            check(bufman::open_for_append(foreign, raw, error),
                  "the foreign file created");
            std::array<char, bufman::kBlockSize> block{};
            block[0] = 'T';
            block[1] = 'U';
            block[2] = 'P';
            block[3] = '1';
            check(bufman::write_block(raw, 0, block.data(), block.size(),
                                      error),
                  "a TUP1 magic written");
            bufman::close(raw);
        }
        std::string foreign_error;
        check(!tools::bulk_load(
                  foreign, schema, 4,
                  [](std::size_t, tuple::Tuple&, std::string&) {
                      return true;
                  },
                  stats, foreign_error) &&
                  foreign_error.find("not a tuple heap file") !=
                      std::string::npos,
              "a foreign header is refused by name");
        std::remove(foreign.c_str());

        // A wrong-table header is refused by name.
        const std::string wrong = temp_path("refuse-wrong");
        std::remove(wrong.c_str());
        {
            schema::TableSchema other("");
            check(schema::parse_table_schema(
                      R"({"table_name": "other",
                          "columns": [
                            {"column_name": "sid", "column_type": "Integer", "size": 4},
                            {"column_name": "pad", "column_type": "String",  "size": 900}]})",
                      other, error),
                  "the other-table schema parses");
            bufman::BufferManager manager;
            std::string init_error;
            manager.init(4, std::make_unique<bufman::LRUPolicy>(),
                         init_error);
            heapfile::HeapFile file(manager);
            check(file.create(wrong, other, error),
                  "the other table's empty file created");
            manager.close_all(error);
        }
        std::string wrong_error;
        check(!tools::bulk_load(
                  wrong, schema, 4,
                  [](std::size_t, tuple::Tuple&, std::string&) {
                      return true;
                  },
                  stats, wrong_error) &&
                  wrong_error.find("other") != std::string::npos,
              "a wrong-table header is refused by name");
        std::remove(wrong.c_str());

        // No record layout: refused.
        schema::TableSchema loose("loose");
        std::string loose_error;
        check(loose.add_column("a", tuple::ColumnType::Integer, loose_error) &&
                  loose.add_column("s", tuple::ColumnType::String,
                                   loose_error),
              "the width-less schema builds");
        std::string no_layout;
        check(!tools::bulk_load(
                  temp_path("refuse-loose"), loose, 4,
                  [](std::size_t, tuple::Tuple&, std::string&) {
                      return true;
                  },
                  stats, no_layout) &&
                  no_layout.find("no record layout") != std::string::npos,
              "a schema without record layout is refused");
    }

    // --- generator failure surfaces the index ---
    {
        schema::TableSchema schema("");
        check(wide_schema(schema, error), "the wide fixture schema parses");
        const std::string path = temp_path("genfail");
        std::remove(path.c_str());
        tools::BulkLoadStats stats;
        std::string gen_error;
        check(!tools::bulk_load(
                  path, schema, 10,
                  [](std::size_t index, tuple::Tuple& out, std::string& err) {
                      if (index == 7) {
                          err = "no more rows";
                          return false;
                      }
                      out = wide_row(static_cast<int>(index) + 1);
                      return true;
                  },
                  stats, gen_error) &&
                  gen_error.find("row index 7") != std::string::npos,
              "a generator failure names the row index");
        std::remove(path.c_str());
    }

    // --- empty and zero-record loads ---
    {
        schema::TableSchema schema("");
        check(wide_schema(schema, error), "the wide fixture schema parses");

        // A zero-record load into a missing file: a valid empty heap.
        const std::string zero = temp_path("zero");
        std::remove(zero.c_str());
        tools::BulkLoadStats stats;
        check(tools::bulk_load(
                  zero, schema, 0,
                  [](std::size_t, tuple::Tuple&, std::string&) {
                      return false;
                  },
                  stats, error) &&
                  stats.records == 0 && stats.pages == 0,
              "a zero-record load succeeds with no pages");
        error.clear();
        bufman::BufferManager manager;
        std::string init_error;
        manager.init(4, std::make_unique<bufman::LRUPolicy>(), init_error);
        heapfile::HeapFile file(manager);
        check(file.open(zero, schema, error) && file.check_structure(error),
              "the zero-record file opens as a valid empty heap");
        std::vector<int> ids = scan_ids(file, error);
        check(ids.empty(), "the zero-record file scans to nothing");
        check(file.close(error), "the zero-record file closed");
        manager.close_all(error);
        std::remove(zero.c_str());

        // A zero-record load into an existing empty (created) file: reused.
        const std::string reused = temp_path("reused");
        std::remove(reused.c_str());
        {
            bufman::BufferManager manager;
            std::string init_error;
            manager.init(4, std::make_unique<bufman::LRUPolicy>(),
                         init_error);
            heapfile::HeapFile file(manager);
            check(file.create(reused, schema, error),
                  "the to-be-reused file created");
            manager.close_all(error);
        }
        check(tools::bulk_load(
                  reused, schema, 0,
                  [](std::size_t, tuple::Tuple&, std::string&) {
                      return false;
                  },
                  stats, error),
              "an existing empty file is reused for a zero-record load");
        std::remove(reused.c_str());
    }

    // --- the differential: tool vs per-row insertion, byte-level twins ---
    {
        schema::TableSchema schema("");
        check(wide_schema(schema, error), "the wide fixture schema parses");
        const std::string tool_path = temp_path("twin-tool");
        const std::string row_path = temp_path("twin-row");
        std::remove(tool_path.c_str());
        std::remove(row_path.c_str());
        tools::BulkLoadStats stats;
        const std::size_t count = 30; // 8 pages: full pages + a partial tail
        check(tools::bulk_load(
                  tool_path, schema, count,
                  [](std::size_t index, tuple::Tuple& out, std::string&) {
                      out = wide_row(static_cast<int>(index) + 1);
                      return true;
                  },
                  stats, error),
              "the tool-loaded twin loaded");
        error.clear();
        {
            bufman::BufferManager manager;
            std::string init_error;
            manager.init(8, std::make_unique<bufman::LRUPolicy>(),
                         init_error);
            heapfile::HeapFile file(manager);
            check(file.create(row_path, schema, error) &&
                      file.open(row_path, schema, error),
                  "the per-row twin file created");
            for (std::size_t i = 0; i < count; ++i) {
                heapfile::RowId rid;
                check(file.insert(wide_row(static_cast<int>(i) + 1), rid,
                                  error),
                      "per-row insert");
                if (i == 0 && rid.page_id != 1) {
                    break;
                }
            }
            check(file.check_structure(error),
                  "the per-row twin is consistent");
            check(file.close(error), "the per-row twin closed");
            manager.close_all(error);
        }
        // Both files, byte for byte: identical layout means identical
        // blocks — header lists, occupancy, and record bytes alike.
        bool twins_match = true;
        {
            bufman::BufferManager manager;
            std::string init_error;
            manager.init(8, std::make_unique<bufman::LRUPolicy>(),
                         init_error);
            heapfile::HeapFile tool_file(manager);
            heapfile::HeapFile row_file(manager);
            check(tool_file.open(tool_path, schema, error) &&
                      row_file.open(row_path, schema, error),
                  "both twins open");
            std::vector<int> tool_ids = scan_ids(tool_file, error);
            std::vector<int> row_ids = scan_ids(row_file, error);
            twins_match = twins_match && tool_ids == row_ids;
            for (std::size_t i = 0; i < count; ++i) {
                tuple::Tuple a;
                tuple::Tuple b;
                twins_match = twins_match &&
                              tool_file.find(heapfile::RowId{
                                  static_cast<std::uint32_t>(i / 4 + 1),
                                  static_cast<std::uint32_t>(i % 4)},
                                             a, error) &&
                              row_file.find(heapfile::RowId{
                                  static_cast<std::uint32_t>(i / 4 + 1),
                                  static_cast<std::uint32_t>(i % 4)},
                                             b, error) &&
                              a == b;
            }
            tool_file.close(error);
            row_file.close(error);
            manager.close_all(error);
        }
        check(twins_match,
              "tool-loaded and per-row-loaded files are engine-equivalent");
        std::remove(tool_path.c_str());
        std::remove(row_path.c_str());
    }

    std::cout << (failures == 0
                      ? "loader suite: all checks passed."
                      : "loader suite: " + std::to_string(failures) +
                            " check(s) FAILED.")
              << "\n";
    return failures;
}
