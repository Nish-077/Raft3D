#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>
#include "kv/raft_kv_state_mgr.hpp"
#include "libnuraft/cluster_config.hxx"
#include "libnuraft/srv_state.hxx"
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <memory>
#include <cstdio>
#include <vector>

using namespace Raft3D;

std::string test_db_path = "./test_raft_kv_state_mgr_db";

// Helper to create a RocksDB instance with a log column family
struct RocksDBMgrFixture {
    std::shared_ptr<rocksdb::DB> db;
    std::shared_ptr<rocksdb::ColumnFamilyHandle> log_cf;
    rocksdb::Options options;
    std::vector<rocksdb::ColumnFamilyHandle*> handles;

    RocksDBMgrFixture() {
        rocksdb::DestroyDB(test_db_path, rocksdb::Options());
        options.create_if_missing = true;
        options.create_missing_column_families = true;
        std::vector<std::string> cfs = { "default", "log_cf" };
        std::vector<rocksdb::ColumnFamilyDescriptor> cf_descs;
        for (auto& name : cfs) {
            cf_descs.emplace_back(name, rocksdb::ColumnFamilyOptions());
        }
        rocksdb::DB* db_ptr = nullptr;
        rocksdb::Status s = rocksdb::DB::Open(options, test_db_path, cf_descs, &handles, &db_ptr);
        REQUIRE(s.ok());
        db.reset(db_ptr);
        // Use a custom deleter for log_cf
        log_cf = std::shared_ptr<rocksdb::ColumnFamilyHandle>(
            handles[1],
            [db=db](rocksdb::ColumnFamilyHandle* h) {
                if (db && h) db->DestroyColumnFamilyHandle(h);
            }
        );
    }

    ~RocksDBMgrFixture() {
        // Only destroy handles[0] (default), log_cf will be destroyed by its shared_ptr
        if (!handles.empty() && handles[0]) db->DestroyColumnFamilyHandle(handles[0]);
        db.reset();
        rocksdb::DestroyDB(test_db_path, rocksdb::Options());
    }
};

TEST_CASE("RaftKVStateManager config and state", "[RaftKVStateManager]") {
    RocksDBMgrFixture rocksdb;
    std::vector<Raft3DServer> peers = {
        {1, "localhost:10001"},
        {2, "localhost:10002"}
    };

    RaftKVStateManager mgr(1, rocksdb.db, rocksdb.log_cf, peers);

    SECTION("save_config and load_config") {
        nuraft::ptr<nuraft::cluster_config> config = nuraft::cs_new<nuraft::cluster_config>();
        config->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(1, "localhost:10001"));
        config->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(2, "localhost:10002"));
        mgr.save_config(*config);

        auto loaded = mgr.load_config();
        REQUIRE(loaded != nullptr);
        REQUIRE(loaded->get_servers().size() == 2);

        auto it = loaded->get_servers().begin();
        REQUIRE((*it)->get_id() == 1);
        ++it;
        REQUIRE((*it)->get_id() == 2);
    }

    SECTION("save_state and read_state") {
        nuraft::srv_state state(5, 2, false, false); // term=5, voted_for=2
        mgr.save_state(state);

        auto loaded = mgr.read_state();
        REQUIRE(loaded != nullptr);
        REQUIRE(loaded->get_term() == 5);
        REQUIRE(loaded->get_voted_for() == 2);
    }

    SECTION("load_log_store returns a log store") {
        auto log_store = mgr.load_log_store();
        REQUIRE(log_store != nullptr);
    }

    SECTION("server_id returns correct id") {
        REQUIRE(mgr.server_id() == 1);
    }
}