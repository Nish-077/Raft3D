#include "libnuraft/nuraft.hxx"
#include <rocksdb/db.h>
#include "kv/raft_kv_state_mgr.hpp"
#include "kv/raft_kv_log_store.hpp"
#include "raft3d_server.hpp"
#include "raft3d_logger.hpp"
#include <vector>
#include <string>
#include <memory>
#include <cstdlib>   // For std::exit
#include <stdexcept> // For exceptions

namespace Raft3D
{
    // --- Constants for Metadata Keys ---
    const std::string KEY_RAFT_CONFIG = "_raft_config_";
    const std::string KEY_RAFT_STATE = "_raft_state_";

    // --- Helper Functions (Placeholder - Implement using NuRaft methods) ---

    inline std::string serialize_config(const nuraft::cluster_config &config, Raft3DLogger *logger = nullptr)
    {
        if (logger)
            logger->put_details(4, __FILE__, __func__, __LINE__, "Serializing cluster_config");
        nuraft::ptr<nuraft::buffer> buf = config.serialize();
        return std::string(reinterpret_cast<const char *>(buf->data_begin()), buf->size());
    }

    inline nuraft::ptr<nuraft::cluster_config> deserialize_config(const rocksdb::Slice &slice, Raft3DLogger *logger = nullptr)
    {
        if (logger)
            logger->put_details(4, __FILE__, __func__, __LINE__, "Deserializing cluster_config from slice of size: " + std::to_string(slice.size()));
        if (slice.empty())
        {
            if (logger)
                logger->put_details(3, __FILE__, __func__, __LINE__, "Empty slice for cluster_config deserialization");
            return nullptr;
        }
        nuraft::ptr<nuraft::buffer> buf = nuraft::buffer::alloc(slice.size());
        buf->put_raw(reinterpret_cast<const nuraft::byte *>(slice.data()), slice.size());
        buf->pos(0); // Reset position for deserialization
        return nuraft::cluster_config::deserialize(*buf);
    }

    inline std::string serialize_state(const nuraft::srv_state &state, Raft3DLogger *logger = nullptr)
    {
        if (logger)
            logger->put_details(4, __FILE__, __func__, __LINE__, "Serializing srv_state");
        nuraft::ptr<nuraft::buffer> buf = state.serialize();
        return std::string(reinterpret_cast<const char *>(buf->data_begin()), buf->size());
    }

    inline nuraft::ptr<nuraft::srv_state> deserialize_state(const rocksdb::Slice &slice, Raft3DLogger *logger = nullptr)
    {
        if (logger)
            logger->put_details(4, __FILE__, __func__, __LINE__, "Deserializing srv_state from slice of size: " + std::to_string(slice.size()));
        if (slice.empty())
        {
            if (logger)
                logger->put_details(3, __FILE__, __func__, __LINE__, "Empty slice for srv_state deserialization");
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
                                           const std::vector<Raft3DServer> &initial_peers,
                                           std::shared_ptr<Raft3DLogger> logger)
        : my_server_id_(server_id),
          db_(rocksdb_instance),
          log_cf_handle_(log_column_family_handle),
          initial_peers_(initial_peers),
          logger_(logger)
    {
        if (!db_)
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "RocksDB instance cannot be null.");
            throw std::invalid_argument("RocksDB instance cannot be null.");
        }
        if (!log_cf_handle_)
        {
            if (logger_)
                logger_->put_details(3, __FILE__, __func__, __LINE__, "Log column family handle is null in RaftKVStateManager.");
        }
    }

    nuraft::ptr<nuraft::cluster_config> RaftKVStateManager::load_config()
    {
        std::string config_str;
        rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(), KEY_RAFT_CONFIG, &config_str);

        if (s.ok())
        {
            nuraft::ptr<nuraft::cluster_config> loaded_config = deserialize_config(config_str, logger_.get());
            if (loaded_config)
            {
                if (logger_)
                    logger_->put_details(4, __FILE__, __func__, __LINE__, "Loaded existing Raft config.");
                return loaded_config;
            }
            else
            {
                if (logger_)
                    logger_->put_details(2, __FILE__, __func__, __LINE__, "Error deserializing existing Raft config. Creating initial.");
            }
        }
        else if (!s.IsNotFound())
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "RocksDB Get failed for config: " + s.ToString());
            throw std::runtime_error("Failed to load Raft config: " + s.ToString());
        }

        // Not found or deserialization error: Create initial config
        if (logger_)
            logger_->put_details(4, __FILE__, __func__, __LINE__, "Creating initial Raft config.");
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
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "Current server ID not found in initial peer list.");
            throw std::runtime_error("Current server ID not found in initial peer list.");
        }

        // Add all servers to initial_config - multinode bootstrap
        for (const auto &peer : initial_peers_)
        {
            nuraft::ptr<nuraft::srv_config> peer_config = nuraft::cs_new<nuraft::srv_config>(peer.id, peer.endpoint);
            initial_config->get_servers().push_back(peer_config);
        }

        // Persist the initial config immediately
        save_config(*initial_config);
        return initial_config;
    }

    void RaftKVStateManager::save_config(const nuraft::cluster_config &config)
    {
        std::string config_str = serialize_config(config, logger_.get());
        rocksdb::Status s = db_->Put(rocksdb::WriteOptions(), db_->DefaultColumnFamily(), KEY_RAFT_CONFIG, config_str);
        if (!s.ok())
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "RocksDB Put failed for config: " + s.ToString());
            throw std::runtime_error("Failed to save Raft config: " + s.ToString());
        }
        if (logger_)
            logger_->put_details(4, __FILE__, __func__, __LINE__, "Saved Raft config.");
    }

    nuraft::ptr<nuraft::srv_state> RaftKVStateManager::read_state()
    {
        std::string state_str;
        rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(), KEY_RAFT_STATE, &state_str);

        if (s.ok())
        {
            nuraft::ptr<nuraft::srv_state> loaded_state = deserialize_state(state_str, logger_.get());
            if (loaded_state)
            {
                if (logger_)
                    logger_->put_details(4, __FILE__, __func__, __LINE__, "Loaded Raft state (Term: " + std::to_string(loaded_state->get_term()) + ", Voted for: " + std::to_string(loaded_state->get_voted_for()) + ")");
                return loaded_state;
            }
            else
            {
                if (logger_)
                    logger_->put_details(2, __FILE__, __func__, __LINE__, "Error deserializing existing Raft state. Returning null.");
                return nullptr;
            }
        }

        if (s.IsNotFound())
        {
            if (logger_)
                logger_->put_details(4, __FILE__, __func__, __LINE__, "No saved Raft state found.");
            return nullptr;
        }

        if (logger_)
            logger_->put_details(2, __FILE__, __func__, __LINE__, "RocksDB Get failed for state: " + s.ToString());
        throw std::runtime_error("Failed to read Raft state: " + s.ToString());
    }

    void RaftKVStateManager::save_state(const nuraft::srv_state &state)
    {
        std::string state_str = serialize_state(state, logger_.get());
        rocksdb::Status s = db_->Put(rocksdb::WriteOptions(), db_->DefaultColumnFamily(), KEY_RAFT_STATE, state_str);
        if (!s.ok())
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "RocksDB Put failed for state: " + s.ToString());
            throw std::runtime_error("Failed to save Raft state: " + s.ToString());
        }
        if (logger_)
            logger_->put_details(4, __FILE__, __func__, __LINE__, "Saved Raft state (Term: " + std::to_string(state.get_term()) + ", Voted for: " + std::to_string(state.get_voted_for()) + ")");
    }

    nuraft::ptr<nuraft::log_store> RaftKVStateManager::load_log_store()
    {
        if (!log_store_)
        {
            if (!log_cf_handle_)
            {
                if (logger_)
                    logger_->put_details(2, __FILE__, __func__, __LINE__, "Log column family handle is null, cannot create log store.");
                throw std::runtime_error("Log column family handle is null, cannot create log store.");
            }
            log_store_ = nuraft::cs_new<RaftKVLogStore>(db_, log_cf_handle_, logger_);
            if (logger_)
                logger_->put_details(4, __FILE__, __func__, __LINE__, "Created new RaftKVLogStore.");
        }
        return log_store_;
    }

    int RaftKVStateManager::server_id()
    {
        return my_server_id_;
    }

    void RaftKVStateManager::system_exit(const int exit_code)
    {
        if (logger_)
            logger_->put_details(1, __FILE__, __func__, __LINE__, "FATAL: Raft system exiting with code " + std::to_string(exit_code));
        std::exit(exit_code);
    }

} // namespace Raft3D