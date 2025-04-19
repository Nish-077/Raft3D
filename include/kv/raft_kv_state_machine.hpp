#pragma once

#include "libnuraft/nuraft.hxx"
#include "libnuraft/basic_types.hxx"
#include "rocksdb/db.h"
#include <mutex>
#include <string>
#include <memory>

namespace Raft3D
{
    class RaftKVStateMachine : public nuraft::state_machine
    {
    private:
        std::shared_ptr<rocksdb::DB> db_;
        std::mutex db_mutex_;
        std::shared_ptr<rocksdb::ColumnFamilyHandle> app_state_cf_handle_;
        nuraft::ulong last_committed_idx_ = 0;
        nuraft::ulong last_snapshot_idx_ = 0;
        nuraft::ulong last_snapshot_term_ = 0;

    public:
        RaftKVStateMachine(std::shared_ptr<rocksdb::DB> rocksdb_instance, std::shared_ptr<rocksdb::ColumnFamilyHandle> app_state_cf_handle);
        ~RaftKVStateMachine() override = default;

        nuraft::ptr<nuraft::buffer> commit(const uint64_t log_idx, nuraft::buffer &data) override;
        void create_snapshot(nuraft::snapshot &s, nuraft::async_result<bool>::handler_type &when_done) override;
        bool apply_snapshot(nuraft::snapshot &s) override;
        nuraft::ptr<nuraft::snapshot> last_snapshot() override;
        nuraft::ulong last_commit_index() override;

        bool get(const std::string &key, std::string &value_out);
    };

} // namespace Raft3D
