#include "libnuraft/nuraft.hxx"
#include <rocksdb/db.h>
#include "kv/raft_kv_state_mgr.hpp"
#include "kv/raft_kv_log_store.hpp" 
#include "raft3d_server.hpp"
#include <vector>
#include <string>
#include <iostream> // For system_exit logging
#include <memory>
#include <cstdlib>   // For std::exit
#include <stdexcept> // For exceptions

namespace Raft3D
{
    // --- Constants for Metadata Keys ---
    const std::string KEY_RAFT_CONFIG = "_raft_config_";
    const std::string KEY_RAFT_STATE = "_raft_state_";

    // --- Helper Functions (Placeholder - Implement using NuRaft methods) ---

    inline std::string serialize_config(const nuraft::cluster_config &config)
    {
        nuraft::ptr<nuraft::buffer> buf = config.serialize();
        return std::string(reinterpret_cast<const char *>(buf->data_begin()), buf->size());
    }

    inline nuraft::ptr<nuraft::cluster_config> deserialize_config(const rocksdb::Slice &slice)
    {
        if (slice.empty())
        {
            return nullptr;
        }
        nuraft::ptr<nuraft::buffer> buf = nuraft::buffer::alloc(slice.size());
        buf->put_raw(reinterpret_cast<const nuraft::byte *>(slice.data()), slice.size());
        buf->pos(0); // Reset position for deserialization
        return nuraft::cluster_config::deserialize(*buf);
    }

    inline std::string serialize_state(const nuraft::srv_state &state)
    {
        nuraft::ptr<nuraft::buffer> buf = state.serialize();
        return std::string(reinterpret_cast<const char *>(buf->data_begin()), buf->size());
    }

    inline nuraft::ptr<nuraft::srv_state> deserialize_state(const rocksdb::Slice &slice)
    {
        if (slice.empty())
        {
            return nullptr;
        }
        nuraft::ptr<nuraft::buffer> buf = nuraft::buffer::alloc(slice.size());
        buf->put_raw(reinterpret_cast<const nuraft::byte *>(slice.data()), slice.size());
        buf->pos(0); // Reset position for deserialization
        return nuraft::srv_state::deserialize(*buf);
    }

    // --- RaftKVStateManager Implementation ---

    RaftKVStateManager::RaftKVStateManager(int server_id,
                                           std::shared_ptr<rocksdb::DB> rocksdb_instance,
                                           std::shared_ptr<rocksdb::ColumnFamilyHandle> log_column_family_handle,
                                           const std::vector<Raft3DServer> &initial_peers)
        : my_server_id_(server_id),
          db_(rocksdb_instance),
          log_cf_handle_(log_column_family_handle),
          initial_peers_(initial_peers)
    {
        if (!db_)
        {
            throw std::invalid_argument("RocksDB instance cannot be null.");
        }
        if (!log_cf_handle_)
        {
            // Allow null only if log store creation is handled differently
            // throw std::invalid_argument("Log column family handle cannot be null.");
            std::cerr << "Warning: Log column family handle is null in RaftKVStateManager." << std::endl;
        }
    }

    nuraft::ptr<nuraft::cluster_config> RaftKVStateManager::load_config()
    {
        std::string config_str;
        rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(), KEY_RAFT_CONFIG, &config_str);

        if (s.ok())
        {
            nuraft::ptr<nuraft::cluster_config> loaded_config = deserialize_config(config_str);
            if (loaded_config)
            {
                std::cout << "Loaded existing Raft config." << std::endl;
                return loaded_config;
            }
            else
            {
                std::cerr << "Error deserializing existing Raft config. Creating initial." << std::endl;
                // Fall through to create initial config
            }
        }
        else if (!s.IsNotFound())
        {
            // Handle RocksDB read error other than NOT FOUND
            std::cerr << "RocksDB Get failed for config: " << s.ToString() << std::endl;
            throw std::runtime_error("Failed to load Raft config: " + s.ToString());
        }

        // Not found or deserialization error: Create initial config
        std::cout << "Creating initial Raft config." << std::endl;
        nuraft::ptr<nuraft::cluster_config> initial_config = nuraft::cs_new<nuraft::cluster_config>();

        std::string my_endpoint;
        for (const auto &peer : initial_peers_)
        {
            if (peer.id == my_server_id_)
            {
                my_endpoint = peer.endpoint;
                break;
            }
        }
        if (my_endpoint.empty())
        {
            throw std::runtime_error("Current server ID not found in initial peer list.");
        }

        // Add all servers to initial_config - multinode bootstrap
        for (const auto &peer : initial_peers_)
        {
            nuraft::ptr<nuraft::srv_config> peer_config = nuraft::cs_new<nuraft::srv_config>(peer.id, peer.endpoint);
            initial_config->get_servers().push_back(peer_config);
        }

        // Persist the initial config immediately?
        save_config(*initial_config);
        return initial_config;
    }

    void RaftKVStateManager::save_config(const nuraft::cluster_config &config)
    {
        std::string config_str = serialize_config(config);
        rocksdb::Status s = db_->Put(rocksdb::WriteOptions(), db_->DefaultColumnFamily(), KEY_RAFT_CONFIG, config_str);
        if (!s.ok())
        {
            std::cerr << "RocksDB Put failed for config: " << s.ToString() << std::endl;
            // Decide if this is fatal - throwing might be appropriate
            throw std::runtime_error("Failed to save Raft config: " + s.ToString());
        }
        std::cout << "Saved Raft config." << std::endl;
    }

    nuraft::ptr<nuraft::srv_state> RaftKVStateManager::read_state()
    {
        std::string state_str;
        rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(), KEY_RAFT_STATE, &state_str);
        
        if (s.ok())
        {
            nuraft::ptr<nuraft::srv_state> loaded_state = deserialize_state(state_str);
            if (loaded_state)
            {
                std::cout << "Loaded Raft state (Term: " << loaded_state->get_term() << ", Voted for: " << loaded_state->get_voted_for() << ")" << std::endl;
                return loaded_state;
            }
            else
            {
                std::cerr << "Error deserializing existing Raft state. Returning null." << std::endl;
                return nullptr;
            }
        }
        
        if (s.IsNotFound())
        {
            // First boot or state lost
            std::cout << "No saved Raft state found." << std::endl;
            return nullptr;
        }
        
        // Handle other RocksDB read errors
        std::cerr << "RocksDB Get failed for state: " << s.ToString() << std::endl;
        throw std::runtime_error("Failed to read Raft state: " + s.ToString());
    }
    
    void RaftKVStateManager::save_state(const nuraft::srv_state &state)
    {
        std::string state_str = serialize_state(state);
        rocksdb::Status s = db_->Put(rocksdb::WriteOptions(), db_->DefaultColumnFamily(), KEY_RAFT_STATE, state_str);
        if (!s.ok())
        {
            std::cerr << "RocksDB Put failed for state: " << s.ToString() << std::endl;
            // Decide if this is fatal
            throw std::runtime_error("Failed to save Raft state: " + s.ToString());
        }
        std::cout << "Saved Raft state (Term: " << state.get_term() << ", Voted for: " << state.get_voted_for() << ")" << std::endl;
    }

    nuraft::ptr<nuraft::log_store> RaftKVStateManager::load_log_store()
    {
        if (!log_store_)
        {
            if (!log_cf_handle_)
            {
                throw std::runtime_error("Log column family handle is null, cannot create log store.");
            }
            log_store_ = nuraft::cs_new<RaftKVLogStore>(db_, log_cf_handle_);
        }
        return log_store_;
    }

    int RaftKVStateManager::server_id()
    {
        return my_server_id_;
    }

    void RaftKVStateManager::system_exit(const int exit_code)
    {
        // Implement custom exit logic if needed (e.g., cleanup)
        std::cerr << "FATAL: Raft system exiting with code " << exit_code << std::endl;
        std::exit(exit_code);
    }

} // namespace Raft3D