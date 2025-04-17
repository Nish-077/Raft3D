#pragma once

#include <libnuraft/nuraft.hxx>
#include <rocksdb/db.h>
#include <mutex>
#include <string>

class kv_state_machine : public nuraft::state_machine {
public:
    kv_state_machine();
    ~kv_state_machine();

    // Required Raft state machine methods
    nuraft::ptr<nuraft::buffer> commit(const uint64_t log_idx, nuraft::buffer& data) override;
    void create_snapshot(nuraft::snapshot& s, nuraft::async_result<bool>::handler_type& when_done) override;
    bool apply_snapshot(nuraft::snapshot& s) override;
    nuraft::ptr<nuraft::snapshot> last_snapshot() override;

    // Local-only read
    bool get(const std::string& key, std::string& value_out);

private:
    rocksdb::DB* db_;
    std::string db_path_ = "/tmp/rocksdb_kv_store";
    std::string snapshot_file_ = "/tmp/rocksdb_snapshot.txt";
    std::mutex db_mutex_;
    uint64_t last_applied_idx_ = 0;
    uint64_t last_snapshot_term_ = 0;
};
