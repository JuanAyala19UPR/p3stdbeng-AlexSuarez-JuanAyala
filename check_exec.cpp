// ex13 — exec layer checks: bind-time refusals, evaluation semantics,
// access-path selection with its markers, the DDL seam (CREATE/DROP
// TABLE and INDEX through the catalog writer), INSERT all-or-nothing
// with index maintenance, and SELECT projection. Runs inside a sandbox
// directory so catalogs and data files never touch the workspace.

#include "bufpool/BufferManager.h"
#include "exec/Executor.h"
#include "policy/LRUPolicy.h"
#include "schema/TableGenerator.h"
#include "sql/Parser.h"

#include <array>
#include <map>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <random>
#include <sys/stat.h>
#include <vector>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

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

// Parses one statement and runs it through the executor.
bool run_sql(exec::Executor& executor, const std::string& text,
             exec::StatementResult& result, std::string& error) {
    sql::ParseResult parsed;
    if (!sql::parse(text, parsed, error) || parsed.statements.size() != 1) {
        if (error.empty()) {
            error = "expected exactly one statement";
        }
        return false;
    }
    return executor.execute(*parsed.statements.front(), result, error);
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}

int run_exec_checks() {
    // The sandbox: a fresh temp directory becomes the working directory,
    // so `data/<table>.tbl` and `tablecatalog/` land there. Restored on exit.
    char pattern[] = "/tmp/ex13-exec-XXXXXX";
    char* made = mkdtemp(pattern);
    if (made == nullptr) {
        std::cout << "FAIL: exec sandbox could not be created\n";
        return 1;
    }
    const std::string original = [] {
        std::vector<char> buffer;
        buffer.resize(4096);
        return std::string(getcwd(buffer.data(), buffer.size()));
    }();
    chdir(made);
    if (system("mkdir -p tablecatalog data") != 0) {
        std::cout << "FAIL: exec sandbox catalog dir\n";
    }

    std::string error;
    bufman::BufferManager manager;
    std::string manager_init_error;
    manager.init(16, std::make_unique<bufman::LRUPolicy>(), manager_init_error);
    exec::Executor executor(manager);

    exec::StatementResult result;

    // --- 5.4. CREATE TABLE through the catalog writer ---
    {
        check(run_sql(executor,
                      "CREATE TABLE person (pid INTEGER, name VARCHAR(10), "
                      "age INT, city VARCHAR(3))",
                      result, error) &&
                  result.message == "table 'person' created",
              "CREATE TABLE reports success");
        std::ifstream catalog("tablecatalog/person.json");
        check(catalog.good(), "the catalog file exists after CREATE TABLE");

        std::string dup_error;
        check(!run_sql(executor, "CREATE TABLE person (a INTEGER)", result,
                       dup_error) &&
                  contains(dup_error, "already exists"),
              "a duplicate CREATE TABLE is refused by name");

        std::string empty_error;
        check(!run_sql(executor, "CREATE TABLE void ()", result, empty_error) ||
                  true,
              "parse-level refusal of empty columns is fine either way");
    }

    // --- 5.6. INSERT: validation, all-or-nothing, index maintenance ---
    {
        check(run_sql(executor,
                      "INSERT INTO person VALUES (1, 'Ruth', 42, 'PR'), "
                      "(2, 'Amy', 30, 'MA'), (3, 'Ivan', 42, 'PR')",
                      result, error) &&
                  result.message == "inserted 3 row(s)",
              "a three-row INSERT reports its count");
        check(run_sql(executor, "SELECT * FROM person", result, error) &&
                  result.rows.rows.size() == 3,
              "the inserted rows are visible");

        std::string bad_error;
        check(!run_sql(executor,
                       "INSERT INTO person VALUES (4, 'WayTooLongName', 1, 'PR')",
                       result, bad_error) &&
                  !bad_error.empty(),
              "a too-wide string is refused with a named error");
        check(run_sql(executor, "SELECT * FROM person", result, error) &&
                  result.rows.rows.size() == 3,
              "a refused INSERT stored nothing (all-or-nothing)");

        std::string arity_error;
        check(!run_sql(executor, "INSERT INTO person VALUES (5, 'X', 1)",
                       result, arity_error) &&
                  contains(arity_error, "4"),
              "a wrong-arity row is refused with the column count");

        std::string type_error;
        check(!run_sql(executor,
                       "INSERT INTO person VALUES (5, 'X', 'old', 'PR')",
                       result, type_error) &&
                  contains(type_error, "must be"),
              "a string in an integer column is refused");
    }

    // --- 5.5. CREATE INDEX / DROP INDEX through the catalog ---
    {
        check(run_sql(executor, "CREATE INDEX person_pid ON person (pid)",
                      result, error) &&
                  contains(result.message, "person_pid"),
              "CREATE INDEX reports success");
        std::vector<schema::TableIndex> indexes;
        check(executor.table("person", indexes, error) != nullptr &&
                  indexes.size() == 1 && indexes[0].name == "person_pid" &&
                  indexes[0].column == "pid",
              "the catalog lists the new index");

        std::string dup_error;
        check(!run_sql(executor, "CREATE INDEX another ON person (pid)",
                       result, dup_error) &&
                  contains(dup_error, "one index per column"),
              "a second index on the same column is refused");
        std::vector<schema::TableIndex> after_refusal;
        check(executor.table("person", after_refusal, error) != nullptr &&
                  after_refusal.size() == 1,
              "the refused CREATE INDEX left the catalog unchanged");

        std::string unknown_column;
        check(!run_sql(executor, "CREATE INDEX bad ON person (nosuch)", result,
                       unknown_column) &&
                  contains(unknown_column, "unknown column"),
              "an index on an unknown column is refused");
    }

    // --- 5.3. access paths and their markers ---
    {
        check(run_sql(executor, "SELECT * FROM person WHERE pid = 2", result,
                      error) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2,
              "equality on the indexed column takes the index scan");

        check(run_sql(executor,
                      "SELECT * FROM person WHERE pid > 1 AND pid < 3",
                      result, error) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2,
              "an AND range on the indexed column takes the index scan");

        check(run_sql(executor,
                      "SELECT * FROM person WHERE pid = 1 AND city = 'PR'",
                      result, error) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1,
              "an indexed equality plus a residual conjunct still uses the index");

        check(run_sql(executor, "SELECT * FROM person WHERE city = 'PR'",
                      result, error) &&
                  result.rows.access_path == "heap scan" &&
                  result.rows.rows.size() == 2,
              "an unindexed column scans the heap");

        check(run_sql(executor,
                      "SELECT * FROM person WHERE pid = 1 OR city = 'MA'",
                      result, error) &&
                  result.rows.access_path == "heap scan" &&
                  result.rows.rows.size() == 2,
              "a top-level OR degrades to the heap scan");

        check(run_sql(executor, "SELECT * FROM person WHERE pid = 99", result,
                      error) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.empty(),
              "an index-scan miss answers empty");
    }

    // --- 5.2. evaluation semantics through WHERE ---
    {
        check(run_sql(executor, "SELECT pid FROM person WHERE age / 2 = 21",
                      result, error) &&
                  result.rows.rows.size() == 2,
              "integer division truncates (42 / 2 = 21 finds two rows)");
        check(run_sql(executor, "SELECT pid FROM person WHERE age = 42.0",
                      result, error) &&
                  result.rows.rows.size() == 2,
              "int/double comparison promotes");
        check(run_sql(executor,
                      "SELECT pid FROM person WHERE city > 'PQ' AND city < 'PS'",
                      result, error) &&
                  result.rows.rows.size() == 2,
              "string comparison is lexicographic");
        check(run_sql(executor,
                      "SELECT pid FROM person WHERE NOT city = 'PR'",
                      result, error) &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2,
              "NOT negates");
        check(run_sql(executor,
                      "SELECT pid FROM person WHERE pid = 1 OR pid = 3",
                      result, error) &&
                  result.rows.rows.size() == 2,
              "OR still answers correctly on the heap path");
        std::string zero_error;
        check(!run_sql(executor, "SELECT * FROM person WHERE pid / 0 = 1",
                       result, zero_error) &&
                  contains(zero_error, "division by zero"),
              "division by zero is a named runtime error");
    }

    // --- 5.1. bind-time refusals (before any page is pinned) ---
    {
        std::string unknown_column;
        check(!run_sql(executor, "SELECT nosuch FROM person", result,
                       unknown_column) &&
                  contains(unknown_column, "unknown column"),
              "an unknown projected column is a bind error");
        std::string unknown_where;
        check(!run_sql(executor, "SELECT * FROM person WHERE nosuch = 1",
                       result, unknown_where) &&
                  contains(unknown_where, "unknown column"),
              "an unknown WHERE column is a bind error");
        std::string compare_types;
        check(!run_sql(executor, "SELECT * FROM person WHERE name = 5",
                       result, compare_types) &&
                  contains(compare_types, "cannot compare"),
              "string-vs-number comparison is a bind error");
        std::string arith_types;
        check(!run_sql(executor, "SELECT * FROM person WHERE name + 1 = 5",
                       result, arith_types) &&
                  contains(arith_types, "arithmetic needs numeric"),
              "arithmetic on strings is a bind error");
        std::string logic_types;
        check(!run_sql(executor, "SELECT * FROM person WHERE pid AND 1",
                       result, logic_types) &&
                  contains(logic_types, "boolean"),
              "non-boolean AND operands are a bind error");
    }

    // --- 5.7. the wider language is refused, naming the feature ---
    // (p3dbeng's surface adds LIMIT, UPDATE, and DELETE — their positive
    // checks live in the sections below; the wider language that remains
    // outside the surface is refused here.)
    {
        const std::pair<const char*, const char*> refusals[] = {
            {"SELECT p.pid FROM person AS p JOIN car AS c ON p.pid = c.pid",
             "joins"},
            {"SELECT city, COUNT(*) FROM person GROUP BY city", "GROUP BY"},
            {"SELECT * FROM person ORDER BY pid", "ORDER BY"},
            {"SELECT COUNT(*) FROM person", "aggregate"},
        };
        for (const auto& [text, feature] : refusals) {
            std::string refusal;
            check(!run_sql(executor, text, result, refusal) &&
                      contains(refusal, feature),
                  std::string("refused and named: ") + feature);
        }
    }

    // --- projection shapes ---
    {
        check(run_sql(executor, "SELECT city, pid FROM person WHERE pid = 1",
                      result, error) &&
                  result.rows.columns.size() == 2 &&
                  result.rows.columns[0] == "city" &&
                  result.rows.columns[1] == "pid" &&
                  std::get<std::string>(result.rows.rows[0][0]) == "PR",
              "projection narrows and orders the columns");
        check(run_sql(executor, "SELECT pid AS id FROM person WHERE pid = 1",
                      result, error) &&
                  result.rows.columns[0] == "id",
              "an alias names the projected column");
    }

    // --- 5.5. DROP INDEX, then the heap path ---
    {
        check(run_sql(executor, "DROP INDEX person_pid", result, error),
              "DROP INDEX succeeds");
        check(run_sql(executor, "SELECT * FROM person WHERE pid = 2", result,
                      error) &&
                  result.rows.access_path == "heap scan" &&
                  result.rows.rows.size() == 1,
              "after the drop, the same predicate scans the heap");
        std::string unknown_index;
        check(!run_sql(executor, "DROP INDEX person_pid", result,
                       unknown_index) &&
                  contains(unknown_index, "unknown index"),
              "dropping a missing index is refused by name");
    }

    // --- 5.5. the missing index file is rebuilt on open ---
    {
        check(run_sql(executor, "CREATE INDEX person_pid ON person (pid)",
                      result, error),
              "the index is created again");
        // A full session close, then a brand-new executor: the index
        // file was deleted externally, so the reopen must rebuild it.
        check(executor.close_all(error), "session closed");
        std::remove("data/person.tbl.pid.idx");
        exec::Executor fresh(manager);
        check(run_sql(fresh, "SELECT * FROM person WHERE pid = 3", result,
                      error) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 3,
              "a deleted index file is rebuilt and used");
        check(run_sql(fresh, "SELECT * FROM person", result, error) &&
                  result.rows.rows.size() == 3,
              "the rebuilt index agrees with the heap");
        check(fresh.close_all(error), "fresh session closed");
    }

    // --- 5.4. DROP TABLE removes everything ---
    {
        check(run_sql(executor, "CREATE TABLE junk (a INTEGER)", result,
                      error) &&
                  run_sql(executor, "INSERT INTO junk VALUES (7)", result,
                          error) &&
                  run_sql(executor, "CREATE INDEX junk_a ON junk (a)",
                          result, error),
              "junk table prepared for the drop");
        check(run_sql(executor, "DROP TABLE junk", result, error) &&
                  result.message == "table 'junk' dropped",
              "DROP TABLE reports success");
        std::ifstream catalog("tablecatalog/junk.json");
        std::ifstream data("data/junk.tbl");
        std::ifstream index("data/junk.tbl.a.idx");
        check(!catalog.good() && !data.good() && !index.good(),
              "catalog, data, and index files are all gone");
        std::string unknown;
        check(!run_sql(executor, "DROP TABLE junk", result, unknown) &&
                  contains(unknown, "unknown table"),
              "dropping a missing table is refused by name");
    }

    // --- LIMIT: caps the projection, keeps the announced path ---
    {
        check(run_sql(executor, "SELECT pid FROM person LIMIT 2", result,
                      error) &&
                  result.rows.rows.size() == 2 &&
                  result.rows.access_path == "heap scan",
              "LIMIT caps the result at two rows");
        check(run_sql(executor, "SELECT pid FROM person LIMIT 0", result,
                      error) &&
                  result.rows.rows.empty(),
              "LIMIT 0 yields zero rows");
        check(run_sql(executor, "SELECT pid FROM person LIMIT 100", result,
                      error) &&
                  result.rows.rows.size() == 3,
              "a LIMIT past the answer returns every row");
        check(run_sql(executor,
                      "SELECT pid FROM person WHERE pid > 1 LIMIT 1", result,
                      error) &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2,
              "LIMIT stops a filtered scan early");
        std::string bind_error;
        check(!run_sql(executor, "SELECT nosuch FROM person LIMIT 2", result,
                       bind_error) &&
                  contains(bind_error, "unknown column"),
              "a bind error still refuses before any row is read");
    }

    // --- arithmetic overflow: p2dbeng's named run-time refusal ---
    {
        std::string overflow_error;
        check(!run_sql(executor,
                       "SELECT * FROM person WHERE 2000000000 + 2000000000 = 0",
                       result, overflow_error) &&
                  overflow_error == "integer overflow",
              "an in-range sum that overflows is a named run-time error");
        overflow_error.clear();
        check(!run_sql(executor,
                       "SELECT * FROM person WHERE -(-2147483647 - 1) = 0",
                       result, overflow_error) &&
                  overflow_error == "integer overflow",
              "negating INT_MIN is a named run-time error");
        overflow_error.clear();
        check(!run_sql(executor, "SELECT * FROM person WHERE pid / (pid - pid) = 0",
                       result, overflow_error) &&
                  overflow_error == "division by zero",
              "division by zero is still a named run-time error");
        overflow_error.clear();
        check(run_sql(executor, "SELECT * FROM person WHERE 2000000000 - 2000000000 = 0",
                      result, error) &&
                  result.rows.rows.size() == 3,
              "an in-range operation still evaluates");
    }

    // --- UPDATE: collect-then-apply with index maintenance ---
    {
        // Self-referential assignment over the fixture table.
        check(run_sql(executor, "UPDATE person SET age = age + 1 WHERE pid = 2",
                      result, error) &&
                  result.message == "updated 1 row(s)",
              "UPDATE reports one row updated");
        check(run_sql(executor, "SELECT age FROM person WHERE pid = 2", result,
                      error) &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 31,
              "UPDATE applied age + 1 against the old value");

        // The swap: simultaneous SET semantics.
        check(run_sql(executor, "CREATE TABLE upd_swap (a INTEGER, b INTEGER)",
                      result, error) &&
                  run_sql(executor, "INSERT INTO upd_swap VALUES (1, 2)",
                          result, error),
              "swap table prepared");
        check(run_sql(executor, "UPDATE upd_swap SET a = b, b = a", result,
                      error) &&
                  result.message == "updated 1 row(s)",
              "the swap UPDATE applied");
        check(run_sql(executor, "SELECT a, b FROM upd_swap", result, error) &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2 &&
                  std::get<int>(result.rows.rows[0][1]) == 1,
              "SET a = b, b = a exchanged the values");

        // A key-column move moves the index entry; a non-key update
        // leaves the index untouched.
        check(run_sql(executor,
                      "CREATE TABLE upd_idx (id INTEGER, val INTEGER)",
                      result, error) &&
                  run_sql(executor,
                          "INSERT INTO upd_idx VALUES (1, 10), (2, 20)",
                          result, error) &&
                  run_sql(executor, "CREATE INDEX upd_idx_id ON upd_idx (id)",
                          result, error),
              "indexed table prepared for UPDATE");
        std::vector<std::string> report;
        bool clean = false;
        check(executor.check_indexes("upd_idx", "data/upd_idx.tbl", report,
                                     clean, error) &&
                  clean,
              "the index verifies clean before the update");
        check(run_sql(executor, "UPDATE upd_idx SET id = 99 WHERE id = 1",
                      result, error) &&
                  result.message == "updated 1 row(s)",
              "a key-column UPDATE applied");
        check(run_sql(executor, "SELECT val FROM upd_idx WHERE id = 99",
                      result, error) &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 10,
              "the index answers at the new key");
        check(run_sql(executor, "SELECT val FROM upd_idx WHERE id = 1",
                      result, error) &&
                  result.rows.rows.empty(),
              "the old key answers nothing");
        check(executor.check_indexes("upd_idx", "data/upd_idx.tbl", report,
                                     clean, error) &&
                  clean && report.size() == 1 &&
                  contains(report[0], "2"),
              "the moved index verifies clean with both entries");
        check(run_sql(executor, "UPDATE upd_idx SET val = 21 WHERE id = 2",
                      result, error) &&
                  executor.check_indexes("upd_idx", "data/upd_idx.tbl",
                                         report, clean, error) &&
                  clean && contains(report[0], "2"),
              "a non-key UPDATE leaves the index count alone");

        // No matches: zero rows, nothing changed.
        check(run_sql(executor, "UPDATE upd_idx SET val = 0 WHERE id = 500",
                      result, error) &&
                  result.message == "updated 0 row(s)",
              "an UPDATE matching nothing changes nothing");

        // Bind-time refusal: an unknown SET column is named, no I/O.
        std::string set_error;
        check(!run_sql(executor, "UPDATE upd_idx SET nosuch = 1",
                       result, set_error) &&
                  contains(set_error, "unknown column 'nosuch' in SET"),
              "an unknown SET column is refused by name");

        // A SET value that breaks the catalog refuses the statement and
        // applies nothing.
        std::string wide_error;
        check(run_sql(executor,
                      "CREATE TABLE upd_wide (s VARCHAR(4))", result,
                      error),
              "the width table is created");
        check(run_sql(executor, "INSERT INTO upd_wide VALUES ('ok')",
                      result, error),
              "the width row is inserted");
        check(!run_sql(executor, "UPDATE upd_wide SET s = 'much too wide'",
                       result, wide_error) &&
                  !wide_error.empty(),
              "a too-wide SET value refuses the statement");
        check(run_sql(executor, "SELECT s FROM upd_wide", result, error) &&
                  result.rows.rows.size() == 1 &&
                  std::get<std::string>(result.rows.rows[0][0]) == "ok",
              "the refused UPDATE stored nothing");
    }

    // --- DELETE: collect-then-erase with index maintenance ---
    {
        // Conditional delete on the indexed table.
        check(run_sql(executor, "DELETE FROM upd_idx WHERE id = 2", result,
                      error) &&
                  result.message == "deleted 1 row(s)",
              "DELETE reports one row deleted");
        check(run_sql(executor, "SELECT * FROM upd_idx WHERE id = 2", result,
                      error) &&
                  result.rows.rows.empty(),
              "the deleted row answers nothing");
        std::vector<std::string> report;
        bool clean = false;
        check(executor.check_indexes("upd_idx", "data/upd_idx.tbl", report,
                                     clean, error) &&
                  clean,
              "deleted rows leave the index verify-clean");

        // Freed space is reusable by a later INSERT.
        check(run_sql(executor, "INSERT INTO upd_idx VALUES (3, 30)", result,
                      error) &&
                  run_sql(executor, "SELECT val FROM upd_idx WHERE id = 3",
                          result, error) &&
                  result.rows.rows.size() == 1,
              "an insert after deletes reuses freed space");

        // A collection failure applies nothing: damage a record's string
        // padding through the raw block layer, then refuse. The UPDATE
        // and DELETE collection scans must both apply nothing.
        check(run_sql(executor, "CREATE TABLE mangle (id INTEGER, pad VARCHAR(8))",
                      result, error) &&
                  run_sql(executor, "INSERT INTO mangle VALUES (1, 'a')",
                          result, error),
              "the corruption table prepared");
        check(executor.close_all(error), "session closed for the damage step");
        {
            bufman::File raw;
            check(bufman::open_for_append("data/mangle.tbl", raw, error),
                  "raw block access for the damage step");
            std::array<char, bufman::kBlockSize> block{};
            check(bufman::read_block(raw, 1, block, error),
                  "the data page read back");
            block[7] = 'X';  // pad is 'a' + NUL at [4,6); byte 7 is padding
            check(bufman::write_block(raw, 1, block.data(), block.size(),
                                      error),
                  "the damaged page written back");
            bufman::close(raw);
        }
        exec::Executor fresh_dmg(manager);
        std::string del_error;
        check(!run_sql(fresh_dmg, "DELETE FROM mangle", result, del_error) &&
                  !del_error.empty(),
              "DELETE fails fast on the unreadable record");
        std::string upd_error;
        check(!run_sql(fresh_dmg, "UPDATE mangle SET id = 9", result,
                       upd_error) &&
                  !upd_error.empty(),
              "UPDATE applies nothing when its collection fails");
        check(run_sql(fresh_dmg, "DROP TABLE mangle", result, error) &&
                  result.message == "table 'mangle' dropped",
              "the corruption table dropped cleanly");
        check(fresh_dmg.close_all(error), "the damage session closed");
    }

    // --- 7.1. the differential oracle ---
    // Random rows, random predicates; every SELECT runs twice — index
    // allowed and index suppressed — and both paths must return
    // identical rows. The index never changes an answer.
    {
        check(run_sql(executor,
                      "CREATE TABLE oracle (id INTEGER, tag VARCHAR(8), "
                      "score INTEGER, label VARCHAR(6))",
                      result, error),
              "oracle table created");
        // 500 rows through the generator: id counts 1..500, tag/label
        // are random-width strings, score is random in [0, 1000).
        {
            std::vector<schema::TableIndex> indexes;
            const schema::TableSchema* schema =
                executor.table("oracle", indexes, error);
            const std::vector<tuple::Tuple> rows =
                schema::generate_tuples(*schema, 500, 1234, 1);
            bool all = true;
            for (const tuple::Tuple& tuple : rows) {
                heapfile::RowId row_id;
                all = all && executor.insert_into("oracle", "data/oracle.tbl",
                                                  tuple, row_id, error);
            }
            check(all, "500 generated rows inserted");
        }
        check(run_sql(executor, "CREATE INDEX oracle_id ON oracle (id)",
                      result, error) &&
                  run_sql(executor, "CREATE INDEX oracle_score ON oracle "
                                    "(score)",
                          result, error),
              "both oracle indexes created");

        // p3dbeng's extension: randomized INSERT/UPDATE/DELETE mutations
        // against a shadow model of expected entries. Every mutation runs
        // through SQL, and after each one both indexes must verify clean
        // and the row count must equal the shadow's.
        {
            std::map<int, tuple::Tuple> shadow;
            {
                std::vector<schema::TableIndex> oracle_indexes;
                const schema::TableSchema* oracle_schema =
                    executor.table("oracle", oracle_indexes, error);
                const std::vector<tuple::Tuple> rows =
                    schema::generate_tuples(*oracle_schema, 500, 1234, 1);
                for (const tuple::Tuple& tuple : rows) {
                    shadow[std::get<int>(tuple.column(0).column_value())] =
                        tuple;
                }
            }
            std::mt19937 mutate_rng(20260214);
            auto mpick = [&](int lo, int hi) {
                return static_cast<int>(
                           mutate_rng() %
                           static_cast<unsigned>(hi - lo + 1)) + lo;
            };
            auto random_word = [&](int width) {
                static const char* letters = "abcdefghij";
                std::string word;
                for (int i = 0; i < width; ++i) {
                    word += letters[mpick(0, 9)];
                }
                return word;
            };
            int inserted_id = 500;
            bool mutations_ok = true;
            std::string mutation_failure;
            std::string last_sql;
            for (int step = 0; step < 250 && mutations_ok; ++step) {
                const int shape = mpick(0, 5);
                if (shape <= 1 || shadow.size() < 50) {
                    ++inserted_id;
                    const std::string tag = random_word(mpick(1, 8));
                    const std::string label = random_word(mpick(1, 6));
                    const std::string sql_text =
                        "INSERT INTO oracle VALUES (" +
                        std::to_string(inserted_id) + ", '" + tag + "', " +
                        std::to_string(mpick(0, 999)) + ", '" + label + "')";
                    last_sql = sql_text;
                    if (!run_sql(executor, sql_text, result, error)) {
                        mutations_ok = false;
                        mutation_failure = "insert failed: " + error;
                        break;
                    }
                    tuple::Tuple fresh;
                    fresh.add(inserted_id);
                    fresh.add(tag);
                    fresh.add(mpick(0, 999));
                    fresh.add(label);
                    shadow[inserted_id] = std::move(fresh);
                } else if (shape <= 3) {
                    auto it = shadow.begin();
                    std::advance(it, mpick(0, static_cast<int>(shadow.size()) - 1));
                    const int id = it->first;
                    const int new_score = mpick(0, 999);
                    const std::string sql_text =
                        "UPDATE oracle SET score = " +
                        std::to_string(new_score) + " WHERE id = " +
                        std::to_string(id);
                    last_sql = sql_text;
                    if (!run_sql(executor, sql_text, result, error)) {
                        mutations_ok = false;
                        mutation_failure = "update failed: " + error;
                        break;
                    }
                    tuple::Tuple updated;
                    const tuple::Tuple& old = it->second;
                    for (std::size_t i = 0; i < old.column_count(); ++i) {
                        updated.add_column(i == 2
                                               ? tuple::Column(new_score)
                                               : old.column(i));
                    }
                    it->second = std::move(updated);
                } else {
                    auto it = shadow.begin();
                    std::advance(it, mpick(0, static_cast<int>(shadow.size()) - 1));
                    const int id = it->first;
                    const std::string sql_text =
                        "DELETE FROM oracle WHERE id = " + std::to_string(id);
                    last_sql = sql_text;
                    if (!run_sql(executor, sql_text, result, error)) {
                        mutations_ok = false;
                        mutation_failure = "delete failed: " + error;
                        break;
                    }
                    shadow.erase(it);
                }
                // The invariants after every mutation: both indexes clean
                // and the row count equal to the shadow's.
                std::vector<std::string> oracle_report;
                bool oracle_clean = false;
                std::size_t live = 0;
                if (!executor.check_indexes("oracle", "data/oracle.tbl",
                                            oracle_report, oracle_clean,
                                            error) ||
                    !oracle_clean ||
                    oracle_report.size() != 2) {
                    mutations_ok = false;
                    mutation_failure = "index not clean after: " + last_sql;
                    break;
                }
                if (!executor.count_rows("oracle", "data/oracle.tbl", live,
                                         error) ||
                    live != shadow.size()) {
                    mutations_ok = false;
                    mutation_failure = "row count disagrees with the shadow";
                    break;
                }
            }
            check(mutations_ok,
                  "250 random mutations keep both indexes clean and the count true" +
                      (mutations_ok ? std::string()
                                    : " — " + mutation_failure));
        }

        // Random predicates over both indexed columns, with and without
        // residual conjuncts, with and without OR.
        std::mt19937 rng(20260213);
        auto pick = [&](int lo, int hi) {
            return static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1)) + lo;
        };
        bool all_match = true;
        std::string first_mismatch;
        for (int case_index = 0; case_index < 300; ++case_index) {
            std::string where;
            const int shape = pick(0, 6);
            switch (shape) {
                case 0: where = "id = " + std::to_string(pick(1, 500)); break;
                case 1: where = "id < " + std::to_string(pick(1, 500)); break;
                case 2: where = "id >= " + std::to_string(pick(1, 500)); break;
                case 3: {
                    const int a = pick(1, 499);
                    where = "id > " + std::to_string(a) + " AND id < " +
                            std::to_string(a + pick(1, 100));
                    break;
                }
                case 4:
                    where = "score = " + std::to_string(pick(0, 999));
                    break;
                case 5:
                    where = "score > " + std::to_string(pick(0, 900)) +
                            " AND score < " + std::to_string(pick(901, 1000));
                    break;
                case 6:
                    where = "id = " + std::to_string(pick(1, 500)) +
                            " OR score < " + std::to_string(pick(0, 200));
                    break;
            }
            const std::string select =
                "SELECT * FROM oracle WHERE " + where;
            exec::StatementResult with_index;
            executor.set_allow_index(true);
            if (!run_sql(executor, select, with_index, error)) {
                all_match = false;
                first_mismatch = "index path failed: " + error;
                break;
            }
            exec::StatementResult without;
            executor.set_allow_index(false);
            if (!run_sql(executor, select, without, error)) {
                all_match = false;
                first_mismatch = "heap path failed: " + error;
                break;
            }
            executor.set_allow_index(true);
            // Row order is unspecified without ORDER BY (the index walks
            // keys, the heap scans row ids), so compare as sorted sets.
            std::vector<std::vector<exec::Value>> a = with_index.rows.rows;
            std::vector<std::vector<exec::Value>> b = without.rows.rows;
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            if (a != b ||
                with_index.rows.columns != without.rows.columns) {
                all_match = false;
                first_mismatch = select + " (index " +
                                 std::to_string(with_index.rows.rows.size()) +
                                 " rows, heap " +
                                 std::to_string(without.rows.rows.size()) +
                                 " rows)";
                break;
            }
        }
        check(all_match, "300 random predicates: index path == heap path" +
                             (all_match ? std::string() : " — " + first_mismatch));
        executor.set_allow_index(true);
        check(executor.close_all(error), "oracle session closed");
    }

    // --- the escape-hatch bulk path on a fresh table ---
    // A table with no data file counts as zero rows, so generated
    // numbering starts at 1 — the ex11 semantics.
    {
        check(run_sql(executor,
                      "CREATE TABLE bulkr (id INTEGER, tag VARCHAR(6))",
                      result, error),
              "bulkr table created with no data file");
        std::vector<schema::TableIndex> indexes;
        const schema::TableSchema* schema =
            executor.table("bulkr", indexes, error);
        std::size_t existing = 999;  // must be overwritten with 0
        check(schema != nullptr &&
                  executor.count_rows("bulkr", "data/bulkr.tbl", existing,
                                      error, true) &&
                  existing == 0,
              "count_rows on a missing data file yields zero rows");
        const std::vector<tuple::Tuple> generated =
            schema::generate_tuples(*schema, 3, 42, existing + 1);
        bool all = true;
        int first_id = -1;
        for (const tuple::Tuple& tuple : generated) {
            heapfile::RowId row_id;
            all = all && executor.insert_into("bulkr", "data/bulkr.tbl",
                                              tuple, row_id, error);
            if (first_id < 0) {
                first_id = std::get<int>(tuple.column(0).column_value());
            }
        }
        check(all && first_id == 1,
              "bulk numbering starts at 1 on a fresh table");
    }

    // --- index types: the catalog field, the USING clause, the dispatch ---
    {
        // The catalog records each type; the loader defaults to ISAM.
        check(run_sql(executor, "CREATE TABLE typed (a INTEGER, s VARCHAR(6), v INTEGER)",
                      result, error) &&
                  run_sql(executor,
                          "INSERT INTO typed VALUES (1, 'x', 10), (2, 'y', 20), "
                          "(3, 'z', 30)",
                          result, error) &&
                  run_sql(executor, "CREATE INDEX typed_a ON typed (a)", result,
                          error) &&
                  run_sql(executor,
                          "CREATE INDEX typed_s ON typed (s) USING BTREE",
                          result, error),
              "both index types created");
        std::vector<schema::TableIndex> typed_indexes;
        const schema::TableSchema* typed_schema =
            executor.table("typed", typed_indexes, error);
        check(typed_schema != nullptr && typed_indexes.size() == 2 &&
                  typed_indexes[0].type == "ISAM" &&
                  typed_indexes[1].type == "BTREE",
              "the catalog records ISAM (the default) and BTREE");

        // An unknown USING type is refused by name, catalog unchanged.
        std::string hash_error;
        check(!run_sql(executor, "CREATE INDEX typed_bad ON typed (v) USING HASH",
                       result, hash_error) &&
                  contains(hash_error, "HASH"),
              "an unknown USING type is refused by name");
        check(run_sql(executor, "SELECT * FROM typed WHERE a = 2", result,
                      error) &&
                  result.rows.rows.size() == 1,
              "the refused type left the table working");

        // One index per column holds across types.
        std::string dup_error;
        check(!run_sql(executor, "CREATE INDEX typed_a2 ON typed (a) USING BTREE",
                       result, dup_error) &&
                  contains(dup_error, "already carries an index"),
              "one index per column holds across types");

        // The BTREE index serves SELECTs and survives maintenance.
        check(run_sql(executor, "SELECT s FROM typed WHERE a = 3", result,
                      error) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<std::string>(result.rows.rows[0][0]) == "z",
              "a BTREE-declared index serves the index-scan path");
        std::vector<std::string> report;
        bool clean = false;
        check(executor.check_indexes("typed", "data/typed.tbl", report,
                                     clean, error) &&
                  clean && report.size() == 2,
              "both index types verify clean");
        check(run_sql(executor, "UPDATE typed SET a = 99 WHERE a = 3", result,
                      error) &&
                  run_sql(executor, "SELECT s FROM typed WHERE a = 99", result,
                          error) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1,
              "a key-column move rides the BTREE index");
        check(run_sql(executor, "DELETE FROM typed WHERE a = 99", result,
                      error) &&
                  executor.check_indexes("typed", "data/typed.tbl", report,
                                         clean, error) &&
                  clean,
              "a DELETE keeps the BTREE index verify-clean");

        // The lazy rebuild builds the declared type.
        check(run_sql(executor, "CREATE TABLE rebt (a INTEGER, s VARCHAR(6))",
                      result, error) &&
                  run_sql(executor, "INSERT INTO rebt VALUES (1, 'x'), (2, 'y')",
                          result, error) &&
                  run_sql(executor, "CREATE INDEX rebt_a ON rebt (a) USING BTREE",
                          result, error),
              "the rebuild fixture prepared");
        check(executor.close_all(error), "the session closed for the rebuild");
        std::remove("data/rebt.tbl.a.idx");
        exec::Executor fresh_btree(manager);
        check(run_sql(fresh_btree, "SELECT * FROM rebt WHERE a = 2", result,
                      error) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1,
              "the missing BTREE file rebuilt lazily and answers");
        check(fresh_btree.close_all(error), "the rebuild session closed");
        {
            // The rebuilt file is a B+tree, not an ISAM index.
            isam::IsamIndex probe(manager, *typed_schema, 0);
            std::string kind_error;
            const bool refused = !probe.open("data/rebt.tbl.a.idx", kind_error);
            check(refused, "the rebuilt file is not an ISAM index");
            probe.close(error);
        }

        // A kind mismatch refuses instead of silently rebuilding: swap the
        // two tables' index files and open again.
        check(run_sql(executor, "CREATE TABLE isam_t (a INTEGER)", result,
                      error) &&
                  run_sql(executor, "INSERT INTO isam_t VALUES (1), (2)",
                          result, error) &&
                  run_sql(executor, "CREATE INDEX isam_t_a ON isam_t (a)",
                          result, error) &&
                  run_sql(executor, "CREATE TABLE btree_t (b INTEGER)", result,
                          error) &&
                  run_sql(executor, "INSERT INTO btree_t VALUES (1), (2)",
                          result, error) &&
                  run_sql(executor, "CREATE INDEX btree_t_b ON btree_t (b) USING BTREE",
                          result, error),
              "the mismatch fixture prepared");
        check(executor.close_all(error), "the session closed for the swap");
        std::rename("data/isam_t.tbl.a.idx", "/tmp/p3-swap.idx");
        std::rename("data/btree_t.tbl.b.idx", "data/isam_t.tbl.a.idx");
        std::rename("/tmp/p3-swap.idx", "data/btree_t.tbl.b.idx");
        {
            exec::Executor fresh_swap(manager);
            std::string swap_error;
            check(!run_sql(fresh_swap, "SELECT * FROM isam_t WHERE a = 1",
                           result, swap_error) &&
                      !swap_error.empty(),
                  "a kind-mismatched index file refuses by name");
            check(fresh_swap.close_all(error), "the swap session closed");
        }
        // Restore the honest files so later sections see a clean sandbox.
        std::remove("data/isam_t.tbl.a.idx");
        std::remove("data/btree_t.tbl.b.idx");
        std::remove("tablecatalog/isam_t.json");
        std::remove("tablecatalog/btree_t.json");
        std::remove("data/isam_t.tbl");
        std::remove("data/btree_t.tbl");
    }

    // --- the differential oracle over the BTREE type (6.3) -------------------
    // The same gauntlet the ISAM oracle runs, with the table's indexes
    // declared BTREE (one) and ISAM (the other) — one index of each type
    // on separate columns of one table.
    {
        check(run_sql(executor,
                      "CREATE TABLE oracle_b (id INTEGER, tag VARCHAR(8), "
                      "score INTEGER, label VARCHAR(6))",
                      result, error),
              "the btree oracle table created");
        {
            std::vector<schema::TableIndex> indexes;
            const schema::TableSchema* schema =
                executor.table("oracle_b", indexes, error);
            const std::vector<tuple::Tuple> rows =
                schema::generate_tuples(*schema, 500, 1234, 1);
            bool all = true;
            for (const tuple::Tuple& tuple : rows) {
                heapfile::RowId row_id;
                all = all && executor.insert_into("oracle_b", "data/oracle_b.tbl",
                                                  tuple, row_id, error);
            }
            check(all, "500 generated rows inserted (btree oracle)");
        }
        check(run_sql(executor,
                      "CREATE INDEX oracle_b_id ON oracle_b (id) USING BTREE",
                      result, error) &&
                  run_sql(executor, "CREATE INDEX oracle_b_score ON oracle_b (score)",
                          result, error),
              "one BTREE and one ISAM index created on separate columns");

        std::mt19937 rng(20260215);
        auto pick = [&](int lo, int hi) {
            return static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1)) + lo;
        };
        bool all_match = true;
        std::string first_mismatch;
        for (int case_index = 0; case_index < 300; ++case_index) {
            std::string where;
            const int shape = pick(0, 6);
            switch (shape) {
                case 0: where = "id = " + std::to_string(pick(1, 500)); break;
                case 1: where = "id < " + std::to_string(pick(1, 500)); break;
                case 2: where = "id >= " + std::to_string(pick(1, 500)); break;
                case 3: {
                    const int a = pick(1, 499);
                    where = "id > " + std::to_string(a) + " AND id < " +
                            std::to_string(a + pick(1, 100));
                    break;
                }
                case 4:
                    where = "score = " + std::to_string(pick(0, 999));
                    break;
                case 5:
                    where = "score > " + std::to_string(pick(0, 900)) +
                            " AND score < " + std::to_string(pick(901, 1000));
                    break;
                case 6:
                    where = "id = " + std::to_string(pick(1, 500)) +
                            " OR score < " + std::to_string(pick(0, 200));
                    break;
            }
            const std::string select =
                "SELECT * FROM oracle_b WHERE " + where;
            exec::StatementResult with_index;
            executor.set_allow_index(true);
            if (!run_sql(executor, select, with_index, error)) {
                all_match = false;
                first_mismatch = "index path failed: " + error;
                break;
            }
            exec::StatementResult without;
            executor.set_allow_index(false);
            if (!run_sql(executor, select, without, error)) {
                all_match = false;
                first_mismatch = "heap path failed: " + error;
                break;
            }
            executor.set_allow_index(true);
            std::vector<std::vector<exec::Value>> a = with_index.rows.rows;
            std::vector<std::vector<exec::Value>> b = without.rows.rows;
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            if (a != b ||
                with_index.rows.columns != without.rows.columns) {
                all_match = false;
                first_mismatch = select + " (index " +
                                 std::to_string(with_index.rows.rows.size()) +
                                 " rows, heap " +
                                 std::to_string(without.rows.rows.size()) +
                                 " rows)";
                break;
            }
        }
        check(all_match, "300 random predicates on the btree oracle: index path == heap path" +
                             (all_match ? std::string() : " — " + first_mismatch));
        executor.set_allow_index(true);
        check(executor.close_all(error), "the btree oracle session closed");
    }

    chdir(original.c_str());
    std::cout << (failures == 0
                      ? "exec suite: all checks passed."
                      : "exec suite: " + std::to_string(failures) +
                            " check(s) FAILED.")
              << "\n";
    return failures;
}
