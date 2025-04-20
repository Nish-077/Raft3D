#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>
#include "kv/raft_kv_log_store.hpp"
#include "libnuraft/log_entry.hxx"
#include "libnuraft/buffer.hxx"
#include "libnuraft/basic_types.hxx"
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <memory>
#include <cstdio>
#include <iostream>

using namespace Raft3D;

std::string test_db_path = "./test_raft_kv_log_store_db";

// Helper to create a RocksDB instance with a log column family
struct RocksDBFixture {
    std::string db_path;
    std::shared_ptr<rocksdb::DB> db;
    std::shared_ptr<rocksdb::ColumnFamilyHandle> log_cf;
    rocksdb::Options options;
    std::vector<rocksdb::ColumnFamilyHandle*> handles;

    RocksDBFixture(const std::string& path)
        : db_path(path)
    {
        rocksdb::DestroyDB(db_path, rocksdb::Options());
        options.create_if_missing = true;
        options.create_missing_column_families = true;
        std::vector<std::string> cfs = { "default", "log_cf" };
        std::vector<rocksdb::ColumnFamilyDescriptor> cf_descs;
        for (auto& name : cfs) {
            cf_descs.emplace_back(name, rocksdb::ColumnFamilyOptions());
        }
        rocksdb::DB* db_ptr = nullptr;
        rocksdb::Status s = rocksdb::DB::Open(options, db_path, cf_descs, &handles, &db_ptr);
        REQUIRE(s.ok());
        db.reset(db_ptr);
        log_cf = std::shared_ptr<rocksdb::ColumnFamilyHandle>(
            handles[1],
            [db=db](rocksdb::ColumnFamilyHandle* h) {
                if (db && h) db->DestroyColumnFamilyHandle(h);
            }
        );
    }

    ~RocksDBFixture() {
        if (!handles.empty() && handles[0]) db->DestroyColumnFamilyHandle(handles[0]);
        db.reset();
        rocksdb::DestroyDB(db_path, rocksdb::Options());
    }
};

nuraft::ptr<nuraft::log_entry> make_log_entry(nuraft::ulong term, const std::string& data) {
    auto buf = nuraft::buffer::alloc(data.size());
    buf->put_raw((const nuraft::byte*)data.data(), data.size());
    buf->pos(0);
    return nuraft::cs_new<nuraft::log_entry>(term, buf);
}

TEST_CASE("RaftKVLogStore basic operations", "[RaftKVLogStore]") {
    RocksDBFixture rocksdb("test_raft_kv_log_store_db1");
    REQUIRE(rocksdb.db != nullptr);
    REQUIRE(rocksdb.log_cf != nullptr);
    RaftKVLogStore store(rocksdb.db, rocksdb.log_cf);

    SECTION("append and entry_at") {
        auto entry1 = make_log_entry(1, "hello");
        auto idx1 = store.append(entry1);
        REQUIRE(idx1 == 1);
        auto fetched1 = store.entry_at(idx1);
        REQUIRE(fetched1 != nullptr);
        REQUIRE(fetched1->get_term() == 1);
        REQUIRE(std::string((char*)fetched1->get_buf().data_begin(), fetched1->get_buf().size()) == "hello");

        auto entry2 = make_log_entry(2, "world");
        auto idx2 = store.append(entry2);
        REQUIRE(idx2 == 2);
        auto fetched2 = store.entry_at(idx2);
        REQUIRE(fetched2 != nullptr);
        REQUIRE(fetched2->get_term() == 2);
        REQUIRE(std::string((char*)fetched2->get_buf().data_begin(), fetched2->get_buf().size()) == "world");
    }

    SECTION("write_at overwrites and truncates") {
        auto entry1 = make_log_entry(1, "a");
        auto entry2 = make_log_entry(2, "b");
        store.append(entry1);
        store.append(entry2);

        auto entry3 = make_log_entry(3, "c");
        store.write_at(2, entry3); // Overwrite index 2

        auto fetched2 = store.entry_at(2);
        REQUIRE(fetched2 != nullptr);
        REQUIRE(fetched2->get_term() == 3);
        REQUIRE(std::string((char*)fetched2->get_buf().data_begin(), fetched2->get_buf().size()) == "c");
        // There should be no entry at index 3
        REQUIRE(store.entry_at(3) == nullptr);
    }

    SECTION("log_entries returns correct range") {
        for (int i = 1; i <= 5; ++i) {
            auto entry = make_log_entry(i, std::to_string(i));
            store.append(entry);
        }
        auto entries = store.log_entries(2, 5);
        REQUIRE(entries->size() == 3);
        REQUIRE(std::string((char*)(*entries)[0]->get_buf().data_begin(), (*entries)[0]->get_buf().size()) == "2");
        REQUIRE(std::string((char*)(*entries)[2]->get_buf().data_begin(), (*entries)[2]->get_buf().size()) == "4");
    }

    SECTION("term_at returns correct term") {
        auto entry = make_log_entry(7, "x");
        store.append(entry);
        REQUIRE(store.term_at(1) == 7);
        REQUIRE(store.term_at(100) == 0); // Out of range
    }

    SECTION("pack and apply_pack") {
        for (int i = 1; i <= 3; ++i) {
            auto entry = make_log_entry(i, std::string(1, 'a' + i - 1));
            store.append(entry);
        }
        auto pack_buf = store.pack(1, 3);
        // Now apply to a new store with a different DB path
        RocksDBFixture rocksdb2("test_raft_kv_log_store_db2");
        RaftKVLogStore store2(rocksdb2.db, rocksdb2.log_cf);
        store2.apply_pack(1, *pack_buf);
        for (int i = 1; i <= 3; ++i) {
            auto e = store2.entry_at(i);
            REQUIRE(e != nullptr);
            REQUIRE(std::string((char*)e->get_buf().data_begin(), e->get_buf().size()) == std::string(1, 'a' + i - 1));
        }
    }

    SECTION("compact removes old entries") {
        for (int i = 1; i <= 5; ++i) {
            auto entry = make_log_entry(i, std::to_string(i));
            store.append(entry);
        }
        REQUIRE(store.compact(3));
        REQUIRE(store.entry_at(1) == nullptr);
        REQUIRE(store.entry_at(2) == nullptr);
        REQUIRE(store.entry_at(3) == nullptr);
        REQUIRE(store.entry_at(4) != nullptr);
    }

    SECTION("flush returns true") {
        REQUIRE(store.flush());
    }

    SECTION("append entry") {
        auto entry = make_log_entry(1, "test_data");
        store.append(entry);
    }
}