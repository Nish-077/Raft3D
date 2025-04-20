#pragma once

#include "libnuraft/nuraft.hxx"
#include "rocksdb/db.h"
#include <vector>
#include <mutex>
#include <memory>

namespace Raft3D
{
    class RaftKVLogStore : public nuraft::log_store
    {
        private:
            std::shared_ptr<rocksdb::DB> db_;
            std::shared_ptr<rocksdb::ColumnFamilyHandle> log_cf_handle_;
            nuraft::ulong start_idx_;
            nuraft::ulong last_idx_;
            std::mutex mutable log_mutex_;

            nuraft::ptr<nuraft::log_entry> entry_at_internal(nuraft::ulong index) const;

        public:
            RaftKVLogStore(std::shared_ptr<rocksdb::DB> rocksdb_instance, std::shared_ptr<rocksdb::ColumnFamilyHandle> log_column_family_handle);
            ~RaftKVLogStore() override = default;

            nuraft::ulong next_slot() const override;
            nuraft::ulong start_index() const override;
            nuraft::ptr<nuraft::log_entry> last_entry() const override;
            nuraft::ulong append(nuraft::ptr<nuraft::log_entry>& entry) override;
            void write_at(nuraft::ulong index, nuraft::ptr<nuraft::log_entry>& entry) override;
            nuraft::ptr<std::vector<nuraft::ptr<nuraft::log_entry>>> log_entries(nuraft::ulong start, nuraft::ulong end) override;
            nuraft::ptr<nuraft::log_entry> entry_at(nuraft::ulong index) override;
            nuraft::ulong term_at(nuraft::ulong index) override;
            nuraft::ptr<nuraft::buffer> pack(nuraft::ulong index, nuraft::int32 cnt) override;
            void apply_pack(nuraft::ulong index, nuraft::buffer& pack) override;
            bool compact(nuraft::ulong last_log_index) override;
            bool flush() override;
    };
} // namespace Raft3D
