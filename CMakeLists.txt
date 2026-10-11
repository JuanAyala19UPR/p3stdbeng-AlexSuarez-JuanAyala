cmake_minimum_required(VERSION 4.3)
project(p3stdbeng)

set(CMAKE_CXX_STANDARD 20)

set(CORE_SOURCES
    file/BlockFile.cpp
    bufpool/BufferPool.cpp bufpool/DataFrame.h bufpool/BufferPool.h
    policy/ReplacementPolicy.h policy/LRUPolicy.cpp policy/LRUPolicy.h
    bufpool/BufferManager.cpp bufpool/BufferManager.h
    heapfile/SlottedPage.cpp heapfile/SlottedPage.h
    heapfile/HeapFile.cpp heapfile/HeapFile.h
    tuple/Column.h tuple/Tuple.cpp tuple/Tuple.h
    schema/ColumnSchema.h schema/TableSchema.cpp schema/TableSchema.h
    schema/TupleSerializer.cpp schema/TupleSerializer.h
    schema/TableSchemaJson.cpp schema/TableSchemaJson.h
    schema/TableCsv.cpp schema/TableCsv.h
    schema/TableGenerator.cpp schema/TableGenerator.h
    sql/Token.h
    sql/Lexer.h sql/Lexer.cpp
    sql/Ast.h
    sql/Parser.h sql/Parser.cpp
    sql/AstPrinter.h sql/AstPrinter.cpp
    sql/AstDraw.h sql/AstDraw.cpp
    index/KeyCodec.h index/KeyCodec.cpp
    index/IndexPages.h index/IndexPages.cpp
    index/Entries.h
    index/IndexPages.h index/IndexPages.cpp
    index/BptPages.h index/BptPages.cpp
    index/BPlusTree.h index/BPlusTree.cpp
    index/IsamIndex.h index/IsamIndex.cpp
    exec/Value.h exec/Value.cpp
    exec/Binder.h exec/Binder.cpp
    exec/Evaluator.h exec/Evaluator.cpp
    exec/Planner.h exec/Planner.cpp
    exec/Executor.h exec/Executor.cpp
    tools/BulkLoader.h tools/BulkLoader.cpp)

add_library(dbcore STATIC ${CORE_SOURCES})
target_include_directories(dbcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(p3stdbeng
    main.cpp
    check_storage.cpp
    check_sql.cpp
    check_index.cpp
    check_exec.cpp
    check_loader.cpp
    check_bptree.cpp)
target_link_libraries(p3stdbeng PRIVATE dbcore)

add_executable(p3stdbeng_cli p3stdbeng_cli.cpp)
target_link_libraries(p3stdbeng_cli PRIVATE dbcore)

add_executable(p3stdbeng_bench p3stdbeng_bench.cpp)
target_link_libraries(p3stdbeng_bench PRIVATE dbcore)

enable_testing()
add_test(NAME p3stdbeng_checks COMMAND p3stdbeng)
set_tests_properties(p3stdbeng_checks PROPERTIES TIMEOUT 600
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
