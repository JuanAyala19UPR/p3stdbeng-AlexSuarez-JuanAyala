#pragma once

// Executor — the statement dispatcher and the session state it runs
// against: one buffer manager (shared with every heap file and index
// file), the catalog cache, and the file table. The statement surface
// is the proposal's contract; everything the grammar accepts but the
// surface excludes (joins, GROUP BY, ORDER BY, LIMIT, aggregates,
// UPDATE, DELETE) is refused here with a named error — the subset
// contract lives in the dispatcher, not the grammar.
//
// Physical naming: a table's data file is `data/<table>.tbl` (the
// `data` directory is created on demand); an index file is
// `data/<table>.tbl.<column>.idx` beside it.
// Opening a data file builds any catalog-declared index file that is
// missing (lazy rebuild), and every write through a session file
// maintains that file's indexes.

#include "bufpool/BufferManager.h"
#include "exec/Planner.h"
#include "exec/Value.h"
#include "heapfile/HeapFile.h"
#include "index/BPlusTree.h"
#include "index/IsamIndex.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"
#include "sql/Ast.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace exec {

struct ResultSet {
    std::vector<std::string> columns;             // projected names
    std::vector<std::vector<Value>> rows;
    std::string access_path;                      // "index scan"/"heap scan"
};

struct StatementResult {
    std::string message;     // one-line report (DDL, INSERT)
    ResultSet rows;          // filled for SELECT
};

class Executor {
public:
    Executor(bufman::BufferManager& manager,
             const std::string& catalog_directory = "tablecatalog");

    // Dispatches one parsed statement. SELECT fills `result.rows`; other
    // statements fill `result.message`. Refusals and failures are named
    // errors; the session continues.
    bool execute(const sql::Statement& statement, StatementResult& result,
                 std::string& error);

    // The differential-oracle hook: when false, the planner's index path
    // is suppressed and every SELECT scans the heap.
    void set_allow_index(bool allow) { allow_index_ = allow; }

    // The table's schema and catalog indexes (loading the catalog on
    // first use). Null with a named error for an unknown table.
    const schema::TableSchema* table(const std::string& name,
                                     std::vector<schema::TableIndex>& indexes,
                                     std::string& error);

    // The session file for `path`, bound to `table_name`: opened on
    // first use, its catalog indexes opened or lazily built with it.
    // The escape-hatch commands and the SQL path share this.
    heapfile::HeapFile* file_for(const std::string& table_name,
                                 const std::string& path,
                                 const schema::TableSchema& schema,
                                 bool create_when_missing, std::string& error);

    // quit/EOF: close every session file exactly once. False iff any
    // close (flush) failed.
    bool close_all(std::string& error);

    // The escape-hatch write path: inserts one tuple through the session
    // file, maintaining that file's indexes (heap first, then index).
    bool insert_into(const std::string& table_name, const std::string& path,
                     const tuple::Tuple& tuple, heapfile::RowId& row_id,
                     std::string& error);

    // Counts the records of a session file by scanning it. With
    // create_when_missing, a missing data file counts as zero rows (and
    // is created) instead of failing — the bulk path's fresh-table case.
    bool count_rows(const std::string& table_name, const std::string& path,
                    std::size_t& count, std::string& error,
                    bool create_when_missing = false);

    // checkindex: verifies every catalog-declared index of `path`
    // against the data file; one report line per index, clean or named.
    bool check_indexes(const std::string& table_name, const std::string& path,
                       std::vector<std::string>& report, bool& clean,
                       std::string& error);

private:
    // The single dispatch point (design D6): one kind-tagged holder whose
    // methods forward to the declared index type, so INSERT/UPDATE/DELETE
    // maintenance, access-path execution, lazy rebuild, and checkindex
    // never branch on the type themselves.
    struct SessionIndex {
        enum class Kind { Isam, Btree };
        Kind kind = Kind::Isam;
        std::size_t column_index = 0;
        std::string index_name;   // catalog name
        std::string column_name;
        std::unique_ptr<isam::IsamIndex> isam;
        std::unique_ptr<bptree::BPlusTree> btree;

        // The dispatch forwards to the declared type; the holder itself
        // never mutates, so every call is const.
        bool open(const std::string& path, std::string& error) const;
        bool build(const std::string& path,
                   const std::vector<std::pair<tuple::Column, idx::Rid>>& entries,
                   std::string& error) const;
        bool close(std::string& error) const;
        bool lookup(const tuple::Column& key, std::vector<idx::Rid>& out,
                    std::string& error) const;
        bool range(const std::optional<isam::RangeBound>& lo,
                   const std::optional<isam::RangeBound>& hi,
                   std::vector<idx::Rid>& out, std::string& error) const;
        bool insert(const tuple::Column& key, idx::Rid rid,
                    std::string& error) const;
        bool erase(const tuple::Column& key, idx::Rid rid,
                   std::string& error) const;
        bool verify(const std::function<bool(const idx::Rid&)>& occupied,
                    std::size_t& entry_count,
                    std::vector<std::string>& findings,
                    std::string& error) const;
    };

    // The catalog's type word ("ISAM" default) as the dispatch kind.
    static bool parse_index_type(const std::string& type,
                                 SessionIndex::Kind& kind,
                                 std::string& error);

    struct SessionFile {
        std::string table_name;
        std::unique_ptr<heapfile::HeapFile> file;
        std::vector<SessionIndex> indexes;
    };

    struct CatalogEntry {
        CatalogEntry() : schema("") {}
        schema::TableSchema schema;
        std::vector<schema::TableIndex> indexes;
    };

    bool handle_create_table(const sql::CreateTableStatement& statement,
                             StatementResult& result, std::string& error);
    bool handle_drop_table(const sql::DropTableStatement& statement,
                           StatementResult& result, std::string& error);
    bool handle_create_index(const sql::CreateIndexStatement& statement,
                             StatementResult& result, std::string& error);
    bool handle_drop_index(const sql::DropIndexStatement& statement,
                           StatementResult& result, std::string& error);
    bool drop_index_in(const std::string& table_name, CatalogEntry& entry,
                       std::size_t position, StatementResult& result,
                       std::string& error);
    bool handle_insert(const sql::InsertStatement& statement,
                       StatementResult& result, std::string& error);
    bool handle_select(const sql::SelectStatement& statement,
                       StatementResult& result, std::string& error);
    bool handle_update(const sql::UpdateStatement& statement,
                       StatementResult& result, std::string& error);
    bool handle_delete(const sql::DeleteStatement& statement,
                       StatementResult& result, std::string& error);

    bool load_catalog(const std::string& name, std::string& error);
    std::string data_path(const std::string& table_name) const;
    std::string index_path(const std::string& data_file,
                           const std::string& column_name) const;
    bool save_catalog(const std::string& name, std::string& error);
    void forget_file(const std::string& path);

    bufman::BufferManager& manager_;
    std::string catalog_directory_;
    std::unordered_map<std::string, CatalogEntry> catalogs_;
    std::unordered_map<std::string, SessionFile> files_;
    bool allow_index_ = true;
};

}
