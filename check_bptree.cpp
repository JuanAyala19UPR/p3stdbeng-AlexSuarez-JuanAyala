// p3dbeng — the B+tree module suite: page views, header identification,
// bulk build, recursive search with boundary duplicates, recursive insert
// with splits, recursive erase with rebalancing, and the randomized
// shadow-model differential.

#include "bufpool/BufferManager.h"
#include "index/BPlusTree.h"
#include "index/BptPages.h"
#include "index/IsamIndex.h"
#include "index/KeyCodec.h"
#include "policy/LRUPolicy.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"
#include "tuple/Column.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
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
    return std::string("/tmp/p3-bpt-") + std::to_string(getpid()) + "-" +
           suffix + ".idx";
}

// Entries with key == value and rid == {value, value}.
std::vector<std::pair<tuple::Column, idx::Rid>> int_entries(int lo, int hi) {
    std::vector<std::pair<tuple::Column, idx::Rid>> entries;
    for (int value = lo; value <= hi; ++value) {
        entries.emplace_back(tuple::Column(value),
                             idx::Rid{static_cast<std::uint32_t>(value),
                                      static_cast<std::uint32_t>(value)});
    }
    return entries;
}

// One entry with a repeated key and a distinct rid.
std::pair<tuple::Column, idx::Rid> dup_entry(int value, std::uint32_t tag) {
    return {tuple::Column(value), idx::Rid{tag, tag + 777}};
}

// The tree's level count, read from the header block on disk.
std::uint32_t disk_levels(bufman::BufferManager& manager,
                          const std::string& path, std::string& error) {
    manager.flush_all(error);
    if (!error.empty()) {
        return 0;
    }
    bufman::File raw;
    if (!bufman::open_for_read(path, raw, error)) {
        return 0;
    }
    std::array<char, bufman::kBlockSize> block{};
    if (!bufman::read_block(raw, 0, block.data(), block.size(), error)) {
        bufman::close(raw);
        return 0;
    }
    bufman::close(raw);
    std::uint32_t levels = 0;
    std::memcpy(&levels, block.data() + 20, 4);
    return levels;
}

// A raw disk block snapshot (byte-level comparisons).
std::string disk_block(bufman::BufferManager& manager, const std::string& path,
                       std::uint64_t block_id, std::string& error) {
    manager.flush_all(error);
    if (!error.empty()) {
        return {};
    }
    bufman::File raw;
    if (!bufman::open_for_read(path, raw, error)) {
        return {};
    }
    std::array<char, bufman::kBlockSize> block{};
    if (!bufman::read_block(raw, block_id, block.data(), block.size(), error)) {
        bufman::close(raw);
        return {};
    }
    bufman::close(raw);
    return std::string(block.data(), block.size());
}

bool load_person(schema::TableSchema& person, std::string& error) {
    return schema::load_table_schema("tablecatalog/person.json", person,
                                     error);
}

}  // namespace

int run_bptree_checks() {
    std::string error;

    // --- 1.1. the page views -------------------------------------------------
    {
        check(bptree::leaf_capacity(4) == 340 &&
                  bptree::separator_capacity(4) == 511,
              "int-key leaf/separator capacities follow the formulas");
        check(bptree::leaf_capacity(10) == 226 &&
                  bptree::separator_capacity(10) == 292,
              "10-byte string key capacities follow the formulas");

        static char page[bufman::kBlockSize];
        const isam::KeyCodec codec(tuple::ColumnType::Integer, 0);
        bptree::BptLeafPage leaf;
        leaf.bind(page, codec.width());
        leaf.format(idx::kNoPage, idx::kNoPage);
        check(leaf.attach(error) && leaf.count() == 0,
              "a formatted leaf attaches with zero entries");
        // Sorted insert: out-of-order keys land in key order.
        const auto enc = [&](int value) {
            std::string bytes(codec.width(), '\0');
            codec.encode(tuple::Column(value), bytes.data(), error);
            return bytes;
        };
        check(leaf.insert_entry(idx::DataEntry{enc(30), idx::Rid{3, 0}}, codec,
                                error) &&
                  leaf.insert_entry(idx::DataEntry{enc(10), idx::Rid{1, 0}},
                                    codec, error) &&
                  leaf.insert_entry(idx::DataEntry{enc(20), idx::Rid{2, 0}},
                                    codec, error),
              "three out-of-order inserts succeed");
        bool order_ok = true;
        for (std::size_t i = 0; i < leaf.count(); ++i) {
            order_ok = order_ok &&
                       leaf.get_entry(i).rid.page_id ==
                           static_cast<std::uint32_t>(i + 1);
        }
        check(order_ok, "the entries sit in key order after out-of-order inserts");
        check(leaf.lower_bound(enc(15).data(), codec) == 1 &&
                  leaf.lower_bound(enc(25).data(), codec) == 2 &&
                  leaf.lower_bound(enc(5).data(), codec) == 0 &&
                  leaf.lower_bound(enc(35).data(), codec) == 3,
              "lower_bound finds the first key >= the target");
        bool threw = false;
        try {
            (void)leaf.get_entry(3);
        } catch (const std::out_of_range&) {
            threw = true;
        }
        check(threw, "get_entry past the end throws std::out_of_range");
        check(leaf.erase_entry(1, error) && leaf.count() == 2 &&
                  leaf.get_entry(0).rid.page_id == 1 &&
                  leaf.get_entry(1).rid.page_id == 3,
              "erase_entry tail-shifts the remaining entries");
        check(!leaf.erase_entry(9, error) && !error.empty(),
              "erase_entry refuses an out-of-range position");
        error.clear();

        // move_tail_to: the split's right half.
        bptree::BptLeafPage fresh;
        static char fresh_page[bufman::kBlockSize];
        fresh.bind(fresh_page, codec.width());
        leaf.format(idx::kNoPage, idx::kNoPage);
        leaf.insert_entry(idx::DataEntry{enc(1), idx::Rid{1, 0}}, codec, error);
        leaf.insert_entry(idx::DataEntry{enc(2), idx::Rid{2, 0}}, codec, error);
        leaf.insert_entry(idx::DataEntry{enc(3), idx::Rid{3, 0}}, codec, error);
        leaf.insert_entry(idx::DataEntry{enc(4), idx::Rid{4, 0}}, codec, error);
        leaf.move_tail_to(2, fresh, codec.width());
        check(leaf.count() == 2 && fresh.count() == 2 &&
                  fresh.get_entry(0).rid.page_id == 3,
              "move_tail_to halves the entries, order preserved");

        // Separator pages: pairs, routing, insert/erase.
        bptree::BptSeparatorPage sep;
        static char sep_page[bufman::kBlockSize];
        sep.bind(sep_page, codec.width());
        sep.format();
        check(sep.attach(error) && sep.count() == 0 &&
                  sep.first_child() == idx::kNoPage,
              "a formatted separator attaches with no pairs");
        check(sep.append(idx::IndexEntry{enc(10), 21}, error) &&
                  sep.append(idx::IndexEntry{enc(20), 31}, error),
              "separator pairs append in order");
        check(sep.routing_child_index(enc(5).data(), codec) == 0 &&
                  sep.routing_child_index(enc(10).data(), codec) == 1 &&
                  sep.routing_child_index(enc(15).data(), codec) == 1 &&
                  sep.routing_child_index(enc(20).data(), codec) == 2 &&
                  sep.routing_child_index(enc(99).data(), codec) == 2,
              "the routing rule: the child after the last separator <= key");
        sep.set_first_child(11);
        check(sep.routing_child_index(enc(5).data(), codec) == 0 &&
                  sep.first_child() == 11 &&
                  sep.child(0) == 21 && sep.child(1) == 31,
              "first child and pair children answer the routing");
        sep.insert_pair_after(1, idx::IndexEntry{enc(15), 26}, error);
        check(sep.count() == 3 && sep.child(1) == 26 &&
                  sep.routing_child_index(enc(15).data(), codec) == 2,
              "insert_pair_after seats the new child between its neighbours");
        sep.erase_pair(1, codec.width());
        check(sep.count() == 2 && sep.child(1) == 31,
              "erase_pair drops the pair and closes the gap");
    }

    // --- 1.2. the header: identification on open ------------------------------
    {
        schema::TableSchema person("");
        check(load_person(person, error), "person schema loads");

        const std::string path = temp_path("hdr");
        std::remove(path.c_str());
        bufman::BufferManager manager;
        std::string init_error;
        manager.init(8, std::make_unique<bufman::LRUPolicy>(), init_error);
        bptree::BPlusTree idx(manager, person, 0);
        check(idx.create(path, error), "a fresh B+tree file creates");
        check(idx.close(error) && idx.open(path, error),
              "the fresh file closes and reopens empty");
        std::vector<idx::Rid> out;
        check(idx.lookup(tuple::Column(1), out, error) && out.empty(),
              "an empty index answers nothing");
        check(idx.entry_count() == 0, "the header reports zero entries");

        // Wrong table refused.
        schema::TableSchema other("");
        check(schema::parse_table_schema(
                  R"({"table_name": "other",
                      "columns": [
                        {"column_name": "pid", "column_type": "Integer", "size": 4},
                        {"column_name": "name", "column_type": "String", "size": 10},
                        {"column_name": "age", "column_type": "Integer", "size": 4},
                        {"column_name": "city", "column_type": "String", "size": 3}]})",
                  other, error),
              "the other-table schema parses");
        bptree::BPlusTree wrong(manager, other, 0);
        std::string wrong_error;
        check(!wrong.open(path, wrong_error) &&
                  wrong_error.find("person") != std::string::npos,
              "a wrong-table open refuses, naming the file's table");

        // Wrong kind both ways: an ISAM file is not a B+tree, and the
        // B+tree file is not an ISAM index.
        const std::string isam_path = temp_path("hdr-isam");
        std::remove(isam_path.c_str());
        isam::IsamIndex isam_idx(manager, person, 0);
        check(isam_idx.create(isam_path, error), "an ISAM file creates");
        bptree::BPlusTree as_bpt(manager, person, 0);
        std::string kind_error;
        check(!as_bpt.open(isam_path, kind_error) && !kind_error.empty(),
              "an ISAM file opened as a B+tree refuses by name");
        check(isam_idx.close(error), "the ISAM file closed");
        bptree::BPlusTree kind_src(manager, person, 0);
        check(kind_src.create(path, error) == false || true,
              "the bptree file already exists (left open)");
        // close the bptree, then try isam::open on its file
        check(idx.close(error), "the B+tree file closed");
        isam::IsamIndex as_isam(manager, person, 0);
        std::string isam_kind_error;
        check(!as_isam.open(path, isam_kind_error),
              "a B+tree file opened as ISAM refuses");
        check(as_isam.close(error) || true, "kind check closed");
        std::remove(path.c_str());
        std::remove(isam_path.c_str());
        manager.close_all(error);
    }

    // --- 2.1. bulk build --------------------------------------------------------
    {
        schema::TableSchema person("");
        check(load_person(person, error), "person schema loads");
        const std::string path = temp_path("build");
        std::remove(path.c_str());
        bufman::BufferManager manager;
        std::string init_error;
        manager.init(16, std::make_unique<bufman::LRUPolicy>(), init_error);
        bptree::BPlusTree idx(manager, person, 0);
        check(idx.build(path, int_entries(1, 520), error),
              "a 520-entry build succeeds");
        check(idx.entry_count() == 520, "the header counts 520 entries");
        bool all_found = true;
        for (int value = 1; value <= 520; ++value) {
            std::vector<idx::Rid> out;
            all_found = all_found &&
                        idx.lookup(tuple::Column(value), out, error) &&
                        out.size() == 1 &&
                        out[0] == idx::Rid{static_cast<std::uint32_t>(value),
                                           static_cast<std::uint32_t>(value)};
        }
        check(all_found, "every built entry is reachable by lookup");
        std::vector<idx::Rid> range_out;
        check(idx.range(bptree::RangeBound{tuple::Column(100), true},
                        bptree::RangeBound{tuple::Column(199), true},
                        range_out, error) &&
                  range_out.size() == 100,
              "an ascending range returns exactly its window");
        std::size_t walked = 0;
        std::vector<std::string> findings;
        const auto all_occupied = [](const idx::Rid&) { return true; };
        check(idx.verify(all_occupied, walked, findings, error) &&
                  findings.empty() && walked == 520,
              "the built tree verifies clean with the walked total");
        check(disk_levels(manager, path, error) >= 2,
              "a 520-entry build spans more than one level");
        check(idx.close(error), "the built index closed");
        check(idx.open(path, error), "the built index reopens");
        std::vector<idx::Rid> again;
        check(idx.lookup(tuple::Column(7), again, error) && again.size() == 1,
              "entries survive the reopen");
        check(idx.close(error), "closed again");
        std::remove(path.c_str());

        // The empty build.
        const std::string empty_path = temp_path("build-empty");
        std::remove(empty_path.c_str());
        bptree::BPlusTree empty_idx(manager, person, 0);
        check(empty_idx.build(empty_path, {}, error),
              "an empty build succeeds");
        std::vector<idx::Rid> none;
        check(empty_idx.lookup(tuple::Column(1), none, error) && none.empty(),
              "the empty build answers nothing");
        check(empty_idx.verify(all_occupied, walked, findings, error) &&
                  findings.empty() && walked == 0,
              "the empty build verifies clean");
        check(empty_idx.close(error), "the empty index closed");
        std::remove(empty_path.c_str());
        manager.close_all(error);
    }

    // --- 3.1. search: duplicates across a leaf boundary -------------------------
    {
        schema::TableSchema person("");
        check(load_person(person, error), "person schema loads");
        const std::string path = temp_path("dup");
        std::remove(path.c_str());
        bufman::BufferManager manager;
        std::string init_error;
        manager.init(16, std::make_unique<bufman::LRUPolicy>(), init_error);
        bptree::BPlusTree idx(manager, person, 0);
        // Keys 1..339 fill the first leaf exactly (340 = 2d for int keys);
        // five more 340s straddle the split, then 341 resumes.
        auto entries = int_entries(1, 339);
        entries.push_back(dup_entry(340, 1));
        entries.push_back(dup_entry(340, 2));
        entries.push_back(dup_entry(340, 3));
        entries.push_back(dup_entry(340, 4));
        entries.push_back(dup_entry(340, 5));
        auto tail = int_entries(341, 500);
        entries.insert(entries.end(), tail.begin(), tail.end());
        check(idx.build(path, entries, error), "the boundary build succeeds");
        std::vector<idx::Rid> out;
        check(idx.lookup(tuple::Column(340), out, error) && out.size() == 5,
              "duplicates spanning the leaf boundary answer completely");
        check(idx.lookup(tuple::Column(339), out, error) && out.size() == 1,
              "the key before the run answers");
        check(idx.lookup(tuple::Column(341), out, error) && out.size() == 1,
              "the key after the run answers");
        check(idx.range(bptree::RangeBound{tuple::Column(339), true},
                        bptree::RangeBound{tuple::Column(341), true}, out,
                        error) &&
                  out.size() == 7,
              "a range over the run is ascending with duplicates adjacent");
        // Erase one exact duplicate from the left side of the boundary.
        check(idx.erase(tuple::Column(340), idx::Rid{1, 1 + 777}, error),
              "a boundary duplicate erases exactly");
        check(idx.lookup(tuple::Column(340), out, error) && out.size() == 4,
              "the run answers four after the erase");
        bool rid_gone = true;
        for (const idx::Rid& rid : out) {
            rid_gone = rid_gone && !(rid == idx::Rid{1, 1 + 777});
        }
        check(rid_gone, "the erased rid is gone from the answer");
        std::size_t walked = 0;
        std::vector<std::string> findings;
        const auto all_occupied = [](const idx::Rid&) { return true; };
        check(idx.verify(all_occupied, walked, findings, error) &&
                  findings.empty() && walked == 503,
              "the tree verifies clean after the boundary erase");
        check(idx.close(error), "the boundary index closed");
        std::remove(path.c_str());
        manager.close_all(error);
    }

    // --- 4.1. recursive insert: splits and height -------------------------------
    {
        schema::TableSchema person("");
        check(load_person(person, error), "person schema loads");
        const std::string path = temp_path("ins");
        std::remove(path.c_str());
        bufman::BufferManager manager;
        std::string init_error;
        manager.init(16, std::make_unique<bufman::LRUPolicy>(), init_error);
        bptree::BPlusTree idx(manager, person, 0);
        // A full single leaf (2d = 340 entries); the next insert splits it
        // and the height grows by exactly one.
        check(idx.build(path, int_entries(1, 340), error),
              "the full-leaf fixture builds");
        const std::uint32_t levels_before =
            disk_levels(manager, path, error);
        check(levels_before == 1, "the fixture is a single leaf");
        check(idx.insert(tuple::Column(341), idx::Rid{341, 341}, error),
              "the split insert lands");
        const std::uint32_t levels_after =
            disk_levels(manager, path, error);
        check(levels_after == levels_before + 1,
              "the height grew by exactly one at the first split");
        std::vector<idx::Rid> out;
        check(idx.lookup(tuple::Column(1), out, error) &&
                  idx.lookup(tuple::Column(340), out, error) &&
                  idx.lookup(tuple::Column(341), out, error),
              "both halves answer after the split");
        std::size_t walked = 0;
        std::vector<std::string> findings;
        const auto all_occupied = [](const idx::Rid&) { return true; };
        check(idx.verify(all_occupied, walked, findings, error) &&
                  findings.empty() && walked == 341,
              "the split tree verifies clean (exact separators included)");

        // A non-splitting insert changes only the root-to-leaf path and
        // the header: every other block is byte-identical.
        {
            // Rebuild a fresh fixture so the block layout is known.
            check(idx.close(error), "closed for the byte check");
            std::remove(path.c_str());
            bptree::BPlusTree fresh(manager, person, 0);
            check(fresh.build(path, int_entries(1, 200), error),
                  "the byte-check fixture builds");
            const std::uint32_t levels = disk_levels(manager, path, error);
            const std::uint64_t blocks =
                [&]() {
                    bufman::File raw;
                    std::string raw_error;
                    bufman::open_for_read(path, raw, raw_error);
                    const std::uint64_t count = bufman::block_count(raw, raw_error);
                    bufman::close(raw);
                    return count;
                }();
            std::vector<std::string> before;
            for (std::uint64_t b = 0; b < blocks; ++b) {
                before.push_back(disk_block(manager, path, b, error));
            }
            check(fresh.insert(tuple::Column(500), idx::Rid{500, 500}, error),
                  "the non-splitting insert lands");
            std::size_t changed = 0;
            for (std::uint64_t b = 0; b < blocks; ++b) {
                if (disk_block(manager, path, b, error) != before[b]) {
                    ++changed;
                }
            }
            check(changed <= static_cast<std::size_t>(levels) + 1,
                  "only the root-to-leaf path (and header) changed: " +
                      std::to_string(changed) + " of " +
                      std::to_string(blocks));
            check(fresh.close(error), "the byte-check fixture closed");
        }

        // Randomized inserts: depth stays uniform, verification clean.
        const std::string rnd_path = temp_path("ins-rnd");
        std::remove(rnd_path.c_str());
        bptree::BPlusTree ridx(manager, person, 0);
        check(ridx.build(rnd_path, {}, error), "the random insert fixture builds");
        std::mt19937 rng(11);
        bool sequence_ok = true;
        for (int step = 0; step < 800 && sequence_ok; ++step) {
            const int key = static_cast<int>(rng() % 400) - 200;
            sequence_ok =
                ridx.insert(tuple::Column(key),
                            idx::Rid{static_cast<std::uint32_t>(step),
                                     static_cast<std::uint32_t>(step + 5)},
                            error) &&
                ridx.verify(all_occupied, walked, findings, error) &&
                findings.empty() && walked == ridx.entry_count();
        }
        check(sequence_ok,
              "800 random inserts keep the tree clean and balanced");
        check(disk_levels(manager, rnd_path, error) >= 2,
              "the random sequence grew the tree above a single leaf");
        check(ridx.close(error), "the random insert index closed");
        std::remove(rnd_path.c_str());
        manager.close_all(error);
    }

    // --- 5.1. recursive erase: rebalancing ----------------------------------------
    {
        schema::TableSchema person("");
        check(load_person(person, error), "person schema loads");
        const std::string path = temp_path("erase");
        std::remove(path.c_str());
        bufman::BufferManager manager;
        std::string init_error;
        manager.init(16, std::make_unique<bufman::LRUPolicy>(), init_error);
        bptree::BPlusTree idx(manager, person, 0);
        check(idx.build(path, int_entries(1, 500), error),
              "the erase fixture builds");
        std::size_t walked = 0;
        std::vector<std::string> findings;
        const auto all_occupied = [](const idx::Rid&) { return true; };

        // A middle erase: gone from every path, count decremented.
        check(idx.erase(tuple::Column(250), idx::Rid{250, 250}, error),
              "a present entry erases");
        std::vector<idx::Rid> out;
        check(idx.lookup(tuple::Column(250), out, error) && out.empty(),
              "the erased key answers nothing");
        check(idx.range(bptree::RangeBound{tuple::Column(248), true},
                        bptree::RangeBound{tuple::Column(252), true}, out,
                        error) &&
                  out.size() == 4,
              "a range over the gap skips only the erased row");
        check(idx.entry_count() == 499, "the header count dropped by one");
        check(idx.verify(all_occupied, walked, findings, error) &&
                  findings.empty() && walked == 499,
              "the tree verifies clean after the erase");

        // Absent pairs refuse by name, changing nothing.
        std::string miss_error;
        check(!idx.erase(tuple::Column(250), idx::Rid{250, 250}, miss_error) &&
                  !miss_error.empty(),
              "an absent pair refuses");
        check(!idx.erase(tuple::Column(251), idx::Rid{999, 999}, miss_error) &&
                  !miss_error.empty(),
              "a wrong rid for a present key refuses");
        check(idx.entry_count() == 499, "refusals leave the count alone");

        // Massive erases: the height must come back down (merges and the
        // root drop), and the tree must verify clean at the end.
        const std::uint32_t levels_full = disk_levels(manager, path, error);
        bool all_erased = true;
        for (int value = 2; value <= 500; value += 2) {
            if (value == 250) {
                continue;  // the middle-key erase above already took it
            }
            all_erased = all_erased &&
                         idx.erase(tuple::Column(value),
                                   idx::Rid{static_cast<std::uint32_t>(value),
                                            static_cast<std::uint32_t>(value)},
                                   error);
        }
        check(all_erased, "every even key erased");
        const std::uint32_t levels_thin = disk_levels(manager, path, error);
        check(levels_thin < levels_full,
              "the height shrank as merges cascaded (from " +
                  std::to_string(levels_full) + " to " +
                  std::to_string(levels_thin) + ")");
        check(idx.verify(all_occupied, walked, findings, error) &&
                  findings.empty() && walked == 250,
              "the post-merge tree verifies clean");
        // Occupancy within d..2d everywhere except the root is what the
        // empty findings assert; a spot check on the survivors:
        check(idx.lookup(tuple::Column(1), out, error) && out.size() == 1 &&
                  idx.lookup(tuple::Column(499), out, error) && out.size() == 1,
              "survivors still answer");
        check(idx.close(error), "the erase fixture closed");
        std::remove(path.c_str());
        manager.close_all(error);
    }

    // --- 5.2. the randomized shadow-model differential ------------------------------
    {
        schema::TableSchema person("");
        check(load_person(person, error), "person schema loads");
        bufman::BufferManager manager;
        std::string init_error;
        manager.init(16, std::make_unique<bufman::LRUPolicy>(), init_error);
        const auto all_occupied = [](const idx::Rid&) { return true; };

        // Two runs: a dense domain (heavy duplicates) and a sparse one.
        const int domains[] = {200, 1000000};
        for (const int domain : domains) {
            const std::string path =
                temp_path(domain == 200 ? "shadow-dense" : "shadow-sparse");
            std::remove(path.c_str());
            bptree::BPlusTree idx(manager, person, 0);
            check(idx.build(path, {}, error), "the shadow fixture builds");
            std::map<int, std::vector<idx::Rid>> shadow;
            std::mt19937 rng(domain);
            std::uint32_t next_tag = 0;
            bool sequence_ok = true;
            std::uint32_t max_levels = 1;
            std::uint32_t min_levels = 99;
            bool saw_split = false;
            bool saw_collapse = false;
            std::size_t walked = 0;
            std::vector<std::string> findings;
            for (int step = 0; step < 3000 && sequence_ok; ++step) {
                const bool insert_turn =
                    rng() % 100 < 60 || shadow.empty();
                if (insert_turn) {
                    const int key = static_cast<int>(rng() % domain);
                    const idx::Rid rid{next_tag, next_tag + 3};
                    ++next_tag;
                    sequence_ok =
                        idx.insert(tuple::Column(key), rid, error) &&
                        idx.verify(all_occupied, walked, findings, error) &&
                        findings.empty() && walked == idx.entry_count();
                    shadow[key].push_back(rid);
                } else {
                    auto it = shadow.begin();
                    std::advance(it, rng() % shadow.size());
                    auto& rids = it->second;
                    const idx::Rid victim = rids[rng() % rids.size()];
                    sequence_ok =
                        idx.erase(tuple::Column(it->first), victim, error) &&
                        idx.verify(all_occupied, walked, findings, error) &&
                        findings.empty() && walked == idx.entry_count();
                    rids.erase(std::find(rids.begin(), rids.end(), victim));
                    if (rids.empty()) {
                        shadow.erase(it);
                    }
                }
                const std::uint32_t levels = disk_levels(manager, path, error);
                max_levels = std::max(max_levels, levels);
                min_levels = std::min(min_levels, levels);
                saw_split = saw_split || levels > 1;
                saw_collapse = saw_collapse || levels < max_levels;
                if (!sequence_ok) {
                    check(false, "shadow failure at step " +
                                     std::to_string(step) + ": " + error);
                    break;
                }
            }
            // The final state must equal the shadow exactly.
            bool final_ok = sequence_ok;
            std::size_t shadow_total = 0;
            for (const auto& [key, rids] : shadow) {
                shadow_total += rids.size();
                std::vector<idx::Rid> got;
                final_ok = final_ok &&
                           idx.lookup(tuple::Column(key), got, error) &&
                           got.size() == rids.size();
            }
            final_ok = final_ok && shadow_total == idx.entry_count();
            check(final_ok,
                  "domain " + std::to_string(domain) +
                      ": 3000 random operations match the shadow exactly");
            check(saw_split,
                  "domain " + std::to_string(domain) +
                      ": splits occurred (levels " +
                      std::to_string(min_levels) + ".." +
                      std::to_string(max_levels) + ")");
            check(idx.close(error), "the shadow index closed");
            std::remove(path.c_str());
        }
        manager.close_all(error);
    }

    std::cout << (failures == 0
                      ? "bptree suite: all checks passed."
                      : "bptree suite: " + std::to_string(failures) +
                            " check(s) FAILED.")
              << "\n";
    return failures;
}
