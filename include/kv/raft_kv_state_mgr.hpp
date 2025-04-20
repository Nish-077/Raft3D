#pragma once

#include "libnuraft/nuraft.hxx"
#include <rocksdb/db.h>
#include "raft3d_server.hpp"
#include <vector>
#include <string>
#include <memory>

namespace Raft3D
{
    class RaftKVStateManager : public nuraft::state_mgr
    {
        private:
            int my_server_id_;
            std::shared_ptr<rocksdb::DB> db_;
            std::vector<Raft3DServer> initial_peers_;
            nuraft::ptr<nuraft::log_store> log_store_;
            std::shared_ptr<rocksdb::ColumnFamilyHandle> log_cf_handle_;
        
        public:    
            RaftKVStateManager(int _server_id, std::shared_ptr<rocksdb::DB> rocksdb_instance, std::shared_ptr<rocksdb::ColumnFamilyHandle> log_column_family_handle, const std::vector<Raft3DServer>& initial_peers);
            ~RaftKVStateManager() override = default;

            nuraft::ptr<nuraft::cluster_config> load_config() override;
            nuraft::ptr<nuraft::srv_state> read_state() override;
            nuraft::ptr<nuraft::log_store> load_log_store() override;
            void save_config(const nuraft::cluster_config& config) override;
            void save_state(const nuraft::srv_state& state) override;
            void system_exit(const int exit_code) override;
            int server_id() override;
    };
} // namespace Raft3D
