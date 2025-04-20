#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>
#include "kv/raft_kv_state_machine.hpp"
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <memory>
#include <cstdio>
#include <string>

using namespace Raft3D;

std::string test_db_path = "./test_raft_kv_state_machine_db";

struct RocksDBSMFixture {
    std::shared_ptr<rocksdb::DB> db;
    std::shared_ptr<rocksdb::ColumnFamilyHandle> state_cf;
    rocksdb::Options options;
    std::vector<rocksdb::ColumnFamilyHandle*> handles;

    RocksDBSMFixture() {
        options.create_if_missing = true;
        options.create_missing_column_families = true;
        std::vector<std::string> cfs = { "default", "state_cf" };
        std::vector<rocksdb::ColumnFamilyDescriptor> cf_descs;
        for (auto& name : cfs) {
            cf_descs.emplace_back(name, rocksdb::ColumnFamilyOptions());
        }
        rocksdb::DB* db_ptr = nullptr;
        rocksdb::Status s = rocksdb::DB::Open(options, test_db_path, cf_descs, &handles, &db_ptr);
        REQUIRE(s.ok());
        db.reset(db_ptr);
        // Use a custom deleter for state_cf
        state_cf = std::shared_ptr<rocksdb::ColumnFamilyHandle>(
            handles[1],
            [db=db](rocksdb::ColumnFamilyHandle* h) {
                if (db && h) db->DestroyColumnFamilyHandle(h);
            }
        );
    }

    ~RocksDBSMFixture() {
        // Only destroy handles[0] (default), state_cf will be destroyed by its shared_ptr
        if (!handles.empty() && handles[0]) db->DestroyColumnFamilyHandle(handles[0]);
        db.reset();
        rocksdb::DestroyDB(test_db_path, rocksdb::Options());
    }
};

// Helper to create a NuRaft buffer for commit
nuraft::ptr<nuraft::buffer> make_commit_buffer(const std::string& key, const std::string& value) {
    nuraft::ulong key_size = key.size();
    nuraft::ulong value_size = value.size();
    nuraft::ptr<nuraft::buffer> buf = nuraft::buffer::alloc(
        sizeof(nuraft::ulong) + key_size + 1 + sizeof(nuraft::ulong) + value_size
    );
    buf->pos(0);
    buf->put(key_size);
    buf->put_raw((const nuraft::byte*)key.data(), key_size);
    buf->put((uint8_t)1); // value_type: 1=string
    buf->put(value_size);
    buf->put_raw((const nuraft::byte*)value.data(), value_size);
    return buf;
}

TEST_CASE("RaftKVStateMachine basic operations", "[RaftKVStateMachine]") {
    RocksDBSMFixture rocksdb;
    auto sm = std::make_shared<RaftKVStateMachine>(rocksdb.db, rocksdb.state_cf);

    SECTION("commit and get") {
        std::string key = "foo";
        std::string value = R"({"bar":42})";
        auto buf = make_commit_buffer(key, value);
        sm->commit(1, *buf);

        std::string out;
        REQUIRE(sm->get(key, out));
        REQUIRE(out == value);
    }

    SECTION("commit with multiple keys and get") {
        std::string key1 = "k1";
        std::string val1 = "v1";
        std::string key2 = "k2";
        std::string val2 = "v2";
        sm->commit(2, *make_commit_buffer(key1, val1));
        sm->commit(3, *make_commit_buffer(key2, val2));

        std::string out1, out2;
        REQUIRE(sm->get(key1, out1));
        REQUIRE(sm->get(key2, out2));
        REQUIRE(out1 == val1);
        REQUIRE(out2 == val2);
    }

    SECTION("get returns false for missing key") {
        std::string out;
        REQUIRE_FALSE(sm->get("does_not_exist", out));
    }

    SECTION("last_commit_index updates") {
        sm->commit(10, *make_commit_buffer("a", "b"));
        REQUIRE(sm->last_commit_index() == 10);
    }

    SECTION("snapshot and apply_snapshot") {
        // Commit something
        sm->commit(5, *make_commit_buffer("snapkey", "snapval"));

        // Create a dummy snapshot object
        nuraft::snapshot snap(5, 2, nullptr);
        bool called = false;

        // Create the handler as a named variable
        nuraft::async_result<bool>::handler_type handler = [&](bool result, nuraft::ptr<std::exception> err) {
            REQUIRE(result);
            called = true;
        };
        sm->create_snapshot(snap, handler);
        REQUIRE(called);

        // Apply snapshot (simulate restore)
        REQUIRE(sm->apply_snapshot(snap));
        REQUIRE(sm->last_commit_index() == 5);
        auto last_snap = sm->last_snapshot();
        REQUIRE(last_snap->get_last_log_idx() == 5);
        REQUIRE(last_snap->get_last_log_term() == 2);
    }
}