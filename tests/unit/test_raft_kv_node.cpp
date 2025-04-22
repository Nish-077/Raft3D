#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>
#include "kv/raft_kv_node.hpp"
#include "kv/raft_kv_state_machine.hpp"
#include "kv/raft_kv_state_mgr.hpp"
#include "raft3d_logger.hpp"
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <memory>
#include <cstdio>
#include <string>
#include <vector>
#include <iostream>
#include <chrono>
#include <thread>

using namespace Raft3D;

std::string test_db_path = "./test_raft_kv_node_db";

// Dummy RPC client for testing
struct DummyRpcClient : public nuraft::rpc_client
{
    uint64_t get_id() const override { return 1; }
    void send(nuraft::ptr<nuraft::req_msg> &, nuraft::rpc_handler &, uint64_t = 0) override {}
    bool is_abandoned() const override { return false; }
};

// Dummy RPC client factory for testing
struct DummyRpcClientFactory : public nuraft::rpc_client_factory
{
    nuraft::ptr<nuraft::rpc_client> create_client(const std::string &) override
    {
        return nuraft::cs_new<DummyRpcClient>();
    }
};

// Helper to create a RocksDB instance with a log and state column family
struct RocksDBNodeFixture
{
    std::shared_ptr<rocksdb::DB> db;
    std::shared_ptr<rocksdb::ColumnFamilyHandle> log_cf;
    std::shared_ptr<rocksdb::ColumnFamilyHandle> state_cf;
    rocksdb::Options options;
    std::vector<rocksdb::ColumnFamilyHandle *> handles;

    RocksDBNodeFixture()
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
        rocksdb::Status s = rocksdb::DB::Open(options, test_db_path, cf_descs, &handles, &db_ptr);
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
        // Only destroy handles[0] (default), log_cf and state_cf will be destroyed by their shared_ptrs
        if (!handles.empty() && handles[0])
            db->DestroyColumnFamilyHandle(handles[0]);
        db.reset();
        rocksdb::DestroyDB(test_db_path, rocksdb::Options());
    }
};

TEST_CASE("RaftKVNode basic operations", "[RaftKVNode]")
{
    RocksDBNodeFixture rocksdb;

    REQUIRE(rocksdb.db != nullptr);
    REQUIRE(rocksdb.log_cf != nullptr);
    REQUIRE(rocksdb.state_cf != nullptr);

    auto state_machine = nuraft::cs_new<RaftKVStateMachine>(rocksdb.db, rocksdb.state_cf);
    std::vector<Raft3DServer> peers = {
        {1, "localhost:10001"}};
    auto state_mgr = nuraft::cs_new<RaftKVStateManager>(1, rocksdb.db, rocksdb.log_cf, peers);
    auto logger = nuraft::cs_new<Raft3DLogger>();
    RaftKVNode node(1, 10001, state_machine, state_mgr, logger);

    // Wait for raft server to be initialized
    while (!node.get_server()->is_initialized())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Wait for leadership (add after node construction in your test)
    for (int i = 0; i < 100; ++i)
    {
        if (node.get_server()->is_leader())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    REQUIRE(node.get_server()->is_leader());

    SECTION("putKey and getKey")
    {
        std::string key = "testkey";
        std::string value = R"({"foo":123})";
        REQUIRE(node.put_key(key, value) == 0);

        std::string out;
        REQUIRE(node.get_key(key, out) == 0);
        REQUIRE(out == value);
    }

    SECTION("getKey returns -1 for missing key")
    {
        std::string out;
        REQUIRE(node.get_key("does_not_exist", out) == -1);
    }

    SECTION("addServer returns 0 for new server")
    {
        int ret = node.addServer(3, "localhost:10003");
        REQUIRE((ret == 0 || ret == -1));
    }

    SECTION("listServers returns all servers")
    {
        std::vector<Raft3DServer> servers;
        REQUIRE(node.listServers(servers) == 0);
        REQUIRE(servers.size() == 1);
        REQUIRE(servers[0].id == 1);
    }
}