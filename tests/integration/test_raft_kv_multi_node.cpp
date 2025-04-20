#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>
#include "kv/raft_kv_node.hpp"
#include "kv/raft_kv_state_machine.hpp"
#include "kv/raft_kv_state_mgr.hpp"
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <filesystem>

using namespace Raft3D;

std::string db_path1 = "./test_raft_kv_node_db1";
std::string db_path2 = "./test_raft_kv_node_db2";

struct RocksDBNodeFixture
{
    std::shared_ptr<rocksdb::DB> db;
    std::shared_ptr<rocksdb::ColumnFamilyHandle> log_cf;
    std::shared_ptr<rocksdb::ColumnFamilyHandle> state_cf;
    std::vector<rocksdb::ColumnFamilyHandle *> handles;
    rocksdb::Options options;

    RocksDBNodeFixture(const std::string &path)
    {
        options.create_if_missing = true;
        options.create_missing_column_families = true;
        std::vector<std::string> cfs = {"default", "log_cf", "state_cf"};
        std::vector<rocksdb::ColumnFamilyDescriptor> cf_descs;
        for (auto &name : cfs)
        {
            cf_descs.emplace_back(name, rocksdb::ColumnFamilyOptions());
        }
        rocksdb::DB *db_ptr = nullptr;
        rocksdb::Status s = rocksdb::DB::Open(options, path, cf_descs, &handles, &db_ptr);
        REQUIRE(s.ok());
        db.reset(db_ptr);
        log_cf = std::shared_ptr<rocksdb::ColumnFamilyHandle>(
            handles[1],
            [db = db](rocksdb::ColumnFamilyHandle *h)
            {
                if (db && h)
                    db->DestroyColumnFamilyHandle(h);
            });
        state_cf = std::shared_ptr<rocksdb::ColumnFamilyHandle>(
            handles[2],
            [db = db](rocksdb::ColumnFamilyHandle *h)
            {
                if (db && h)
                    db->DestroyColumnFamilyHandle(h);
            });
    }

    ~RocksDBNodeFixture()
    {
        if (!handles.empty() && handles[0])
            db->DestroyColumnFamilyHandle(handles[0]);
        db.reset();
    }
};

TEST_CASE("RaftKVNode multi-node cluster", "[RaftKVNode][multi-node]")
{
    std::filesystem::remove_all(db_path1);
    std::filesystem::remove_all(db_path2);

    {
        // Node 1 setup
        RocksDBNodeFixture rocksdb1(db_path1);
        auto state_machine1 = nuraft::cs_new<RaftKVStateMachine>(rocksdb1.db, rocksdb1.state_cf);
        std::vector<Raft3DServer> peers1 = {
            {1, "localhost:10001"},
            {2, "localhost:10002"}};
        auto state_mgr1 = nuraft::cs_new<RaftKVStateManager>(1, rocksdb1.db, rocksdb1.log_cf, peers1);
        RaftKVNode node1(1, 10001, state_machine1, state_mgr1, nullptr);

        // Node 2 setup
        RocksDBNodeFixture rocksdb2(db_path2);
        auto state_machine2 = nuraft::cs_new<RaftKVStateMachine>(rocksdb2.db, rocksdb2.state_cf);
        std::vector<Raft3DServer> peers2 = {
            {1, "localhost:10001"},
            {2, "localhost:10002"}};
        auto state_mgr2 = nuraft::cs_new<RaftKVStateManager>(2, rocksdb2.db, rocksdb2.log_cf, peers2);
        RaftKVNode node2(2, 10002, state_machine2, state_mgr2, nullptr);

        // Wait for both servers to initialize
        for (int i = 0; i < 100; ++i)
        {
            if (node1.get_server()->is_initialized() && node2.get_server()->is_initialized())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        REQUIRE(node1.get_server()->is_initialized());
        REQUIRE(node2.get_server()->is_initialized());

        // Add each other as peers (if not already present)
        node1.addServer(2, "localhost:10002");
        node2.addServer(1, "localhost:10001");

        // Wait for leader election
        int leader_id = -1;
        for (int i = 0; i < 100; ++i)
        {
            if (node1.get_server()->is_leader())
            {
                leader_id = 1;
                break;
            }
            if (node2.get_server()->is_leader())
            {
                leader_id = 2;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        REQUIRE((leader_id == 1 || leader_id == 2));

        RaftKVNode *leader = (leader_id == 1) ? &node1 : &node2;
        RaftKVNode *follower = (leader_id == 1) ? &node2 : &node1;

        // Write a key to the leader
        std::string key = "foo";
        std::string value = "bar";
        REQUIRE(leader->put_key(key, value) == 0);

        // Wait for log replication
        bool replicated = false;
        for (int i = 0; i < 50; ++i)
        {
            std::string out;
            if (follower->get_key(key, out) == 0 && out == value)
            {
                replicated = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        REQUIRE(replicated);
    }

    // Now cleanup
    std::filesystem::remove_all(db_path1);
    std::filesystem::remove_all(db_path2);
}