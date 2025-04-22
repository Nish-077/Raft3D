#include "libnuraft/nuraft.hxx"
#include "libnuraft/basic_types.hxx"
#include "rocksdb/db.h"
#include "kv/raft_kv_state_machine.hpp"
#include <mutex>
#include <iostream>
#include <string>
#include <memory>
#include "raft3d_logger.hpp"

namespace Raft3D
{
    // --- Helper Functions ---

    // Serialize a nuraft::ulong (for last_committed_idx_)
    inline nuraft::ptr<nuraft::buffer> serialize_last_applied_idx(nuraft::ulong idx, Raft3DLogger *logger = nullptr)
    {
        if (logger)
            logger->put_details(4, __FILE__, __func__, __LINE__, "Serializing last_applied_idx: " + std::to_string(idx));
        nuraft::ptr<nuraft::buffer> buf = nuraft::buffer::alloc(sizeof(idx));
        buf->pos(0);
        buf->put(idx);
        return buf;
    }

    // Deserialize a nuraft::ulong (for last_committed_idx_)
    inline nuraft::ulong deserialize_last_applied_idx(const std::string &str, Raft3DLogger *logger = nullptr)
    {
        if (logger)
            logger->put_details(4, __FILE__, __func__, __LINE__, "Deserializing last_applied_idx from string of size: " + std::to_string(str.size()));
        if (str.size() != sizeof(nuraft::ulong))
        {
            if (logger)
                logger->put_details(2, __FILE__, __func__, __LINE__, "Incorrect size for last_applied_idx deserialization");
            return 0;
        }
        nuraft::ptr<nuraft::buffer> buf = nuraft::buffer::alloc(str.size());
        buf->put_raw(reinterpret_cast<const nuraft::byte *>(str.data()), str.size());
        buf->pos(0);
        return buf->get_ulong();
    }

    // Serialize snapshot metadata (index, term) to nuraft::buffer
    inline nuraft::ptr<nuraft::buffer> serialize_snapshot_meta(nuraft::ulong idx, nuraft::ulong term, Raft3DLogger *logger = nullptr)
    {
        if (logger)
            logger->put_details(4, __FILE__, __func__, __LINE__, "Serializing snapshot meta idx=" + std::to_string(idx) + " term=" + std::to_string(term));
        nuraft::ptr<nuraft::buffer> buf = nuraft::buffer::alloc(sizeof(idx) + sizeof(term));
        buf->pos(0);
        buf->put(idx);
        buf->put(term);
        return buf;
    }

    // Deserialize snapshot metadata (index, term) from string
    inline void deserialize_snapshot_meta(const std::string &str, nuraft::ulong &idx, nuraft::ulong &term, Raft3DLogger *logger = nullptr)
    {
        if (logger)
            logger->put_details(4, __FILE__, __func__, __LINE__, "Deserializing snapshot meta from string of size: " + std::to_string(str.size()));
        if (str.size() != sizeof(nuraft::ulong) * 2)
        {
            if (logger)
                logger->put_details(2, __FILE__, __func__, __LINE__, "Incorrect size for snapshot meta deserialization");
            idx = 0;
            term = 0;
            return;
        }
        nuraft::ptr<nuraft::buffer> buf = nuraft::buffer::alloc(str.size());
        buf->put_raw(reinterpret_cast<const nuraft::byte *>(str.data()), str.size());
        buf->pos(0);
        idx = buf->get_ulong();
        term = buf->get_ulong();
    }

    // Metadata keys for snapshot info and last applied index
    const std::string KEY_LAST_APPLIED_IDX = "_sm_last_committed_idx_";
    const std::string KEY_LAST_SNAPSHOT_META = "_sm_last_snapshot_meta_";

    // --- RaftKVStateMachine Implementation ---

    RaftKVStateMachine::RaftKVStateMachine(
        std::shared_ptr<rocksdb::DB> rocksdb_instance,
        std::shared_ptr<rocksdb::ColumnFamilyHandle> app_state_cf_handle,
        std::shared_ptr<Raft3DLogger> logger) // Add logger parameter
        : db_(rocksdb_instance),
          app_state_cf_handle_(app_state_cf_handle),
          logger_(logger)
    {
        // Load last applied index from DB if present
        std::string idx_str;
        rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(), KEY_LAST_APPLIED_IDX, &idx_str);
        last_committed_idx_ = s.ok() ? deserialize_last_applied_idx(idx_str, logger_.get()) : 0;
        if (logger_)
            logger_->put_details(4, __FILE__, __func__, __LINE__, "Loaded last_committed_idx_ = " + std::to_string(last_committed_idx_));

        // Load snapshot meta from DB if present
        std::string snap_str;
        s = db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(), KEY_LAST_SNAPSHOT_META, &snap_str);
        if (s.ok())
        {
            deserialize_snapshot_meta(snap_str, last_snapshot_idx_, last_snapshot_term_, logger_.get());
            if (logger_)
                logger_->put_details(4, __FILE__, __func__, __LINE__, "Loaded snapshot meta idx=" + std::to_string(last_snapshot_idx_) + " term=" + std::to_string(last_snapshot_term_));
        }
    }

    // --- Commit (Put) ---

    nuraft::ptr<nuraft::buffer> RaftKVStateMachine::commit(const uint64_t log_idx, nuraft::buffer &data)
    {
        std::lock_guard<std::mutex> lock(db_mutex_);

        if (logger_)
            logger_->put_details(5, __FILE__, __func__, __LINE__, "Committing log_idx " + std::to_string(log_idx) + ", buffer size: " + std::to_string(data.size()));
        data.pos(0);

        // [key_size][key][value_type][value_size][value]
        if (data.size() < 9)
        { // 4+1+4 minimum
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "Buffer too small for commit");
            return nullptr;
        }

        auto key_size = data.get_ulong();
        if (logger_)
            logger_->put_details(5, __FILE__, __func__, __LINE__, "key_size: " + std::to_string(key_size) + ", pos: " + std::to_string(data.pos()));

        if (data.pos() + key_size + 1 + sizeof(nuraft::ulong) > data.size())
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "Buffer key size mismatch");
            return nullptr;
        }

        std::string key(reinterpret_cast<const char *>(data.data_begin() + data.pos()), key_size);
        if (logger_)
            logger_->put_details(5, __FILE__, __func__, __LINE__, "key: " + key);

        data.pos(data.pos() + key_size);

        auto value_type = data.get_byte();
        if (logger_)
            logger_->put_details(5, __FILE__, __func__, __LINE__, "value_type: " + std::to_string((int)value_type) + ", pos: " + std::to_string(data.pos()));

        auto value_size = data.get_ulong();
        if (logger_)
            logger_->put_details(5, __FILE__, __func__, __LINE__, "value_size: " + std::to_string(value_size) + ", pos: " + std::to_string(data.pos()));

        if (data.pos() + value_size > data.size())
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "Buffer value size mismatch");
            return nullptr;
        }

        std::string value;
        if (value_type == 1)
        { // string (JSON or any string)
            value.assign(reinterpret_cast<const char *>(data.data_begin() + data.pos()), value_size);
            if (logger_)
                logger_->put_details(5, __FILE__, __func__, __LINE__, "value: " + value);
        }
        else
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "Only string values are supported in this implementation.");
            return nullptr;
        }

        // Write to RocksDB
        rocksdb::Status s = db_->Put(rocksdb::WriteOptions(), app_state_cf_handle_.get(), key, value);
        if (!s.ok())
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "RocksDB Put failed: " + s.ToString());
            return nullptr;
        }

        // Update last applied index
        last_committed_idx_ = log_idx;
        nuraft::ptr<nuraft::buffer> idx_buf = serialize_last_applied_idx(last_committed_idx_, logger_.get());
        std::string idx_str(reinterpret_cast<const char *>(idx_buf->data_begin()), idx_buf->size());
        s = db_->Put(rocksdb::WriteOptions(), db_->DefaultColumnFamily(), KEY_LAST_APPLIED_IDX, idx_str);
        if (!s.ok())
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "Failed to update last_committed_idx_: " + s.ToString());
        }
        else
        {
            if (logger_)
                logger_->put_details(4, __FILE__, __func__, __LINE__, "Committed key: " + key + " at log_idx: " + std::to_string(log_idx));
        }

        return nullptr; // No return value needed for simple KV
    }

    // --- Create Snapshot ---

    void RaftKVStateMachine::create_snapshot(nuraft::snapshot &s, nuraft::async_result<bool>::handler_type &when_done)
    {
        std::lock_guard<std::mutex> lock(db_mutex_);

        nuraft::ulong snap_idx = s.get_last_log_idx();
        nuraft::ulong snap_term = s.get_last_log_term();
        nuraft::ptr<nuraft::buffer> snap_meta = serialize_snapshot_meta(snap_idx, snap_term, logger_.get());

        std::string snap_str(reinterpret_cast<const char *>(snap_meta->data_begin()), snap_meta->size());
        rocksdb::Status st = db_->Put(rocksdb::WriteOptions(), db_->DefaultColumnFamily(), KEY_LAST_SNAPSHOT_META, snap_str);

        bool result = true;
        nuraft::ptr<std::exception> err = nullptr;
        if (!st.ok())
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "Failed to save snapshot meta: " + st.ToString());
            result = false;
            when_done(result, err);
            return;
        }
        last_snapshot_term_ = snap_term;
        if (logger_)
            logger_->put_details(4, __FILE__, __func__, __LINE__, "Created snapshot meta idx=" + std::to_string(snap_idx) + " term=" + std::to_string(snap_term));

        // In a real system, you would also persist a copy of the state at this point.

        when_done(result, err);
    }

    // --- Apply Snapshot ---

    bool RaftKVStateMachine::apply_snapshot(nuraft::snapshot &s)
    {
        std::lock_guard<std::mutex> lock(db_mutex_);

        last_snapshot_idx_ = s.get_last_log_idx();
        last_snapshot_term_ = s.get_last_log_term();
        last_committed_idx_ = last_snapshot_idx_;

        nuraft::ptr<nuraft::buffer> idx_buf = serialize_last_applied_idx(last_committed_idx_, logger_.get());
        std::string idx_str(reinterpret_cast<const char *>(idx_buf->data_begin()), idx_buf->size());
        rocksdb::Status st = db_->Put(rocksdb::WriteOptions(), db_->DefaultColumnFamily(), KEY_LAST_APPLIED_IDX, idx_str);
        if (!st.ok())
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "Failed to update last_applied_idx_: " + st.ToString());
            return false;
        }

        nuraft::ptr<nuraft::buffer> snap_meta = serialize_snapshot_meta(last_snapshot_idx_, last_snapshot_term_, logger_.get());
        std::string snap_str(reinterpret_cast<const char *>(snap_meta->data_begin()), snap_meta->size());
        st = db_->Put(rocksdb::WriteOptions(), db_->DefaultColumnFamily(), KEY_LAST_SNAPSHOT_META, snap_str);
        if (!st.ok())
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "Failed to update snapshot meta: " + st.ToString());
            return false;
        }

        if (logger_)
            logger_->put_details(4, __FILE__, __func__, __LINE__, "Applied snapshot idx=" + std::to_string(last_snapshot_idx_) + " term=" + std::to_string(last_snapshot_term_));
        return true;
    }

    // --- Last Snapshot ---

    nuraft::ptr<nuraft::snapshot> RaftKVStateMachine::last_snapshot()
    {
        std::lock_guard<std::mutex> lock(db_mutex_);
        if (logger_)
            logger_->put_details(5, __FILE__, __func__, __LINE__, "Returning last snapshot idx=" + std::to_string(last_snapshot_idx_) + " term=" + std::to_string(last_snapshot_term_));
        return nuraft::cs_new<nuraft::snapshot>(last_snapshot_idx_, last_snapshot_term_, nullptr);
    }

    // --- Last Commit Index ---

    nuraft::ulong RaftKVStateMachine::last_commit_index()
    {
        std::lock_guard<std::mutex> lock(db_mutex_);
        if (logger_)
            logger_->put_details(5, __FILE__, __func__, __LINE__, "Returning last_committed_idx_ = " + std::to_string(last_committed_idx_));
        return last_committed_idx_;
    }

    // --- Get (Read) ---

    bool RaftKVStateMachine::get(const std::string &key, std::string &value_out)
    {
        std::lock_guard<std::mutex> lock(db_mutex_);
        rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), app_state_cf_handle_.get(), key, &value_out);
        if (!s.ok())
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "RocksDB Get failed for key '" + key + "': " + s.ToString());
            return false;
        }
        if (logger_)
            logger_->put_details(5, __FILE__, __func__, __LINE__, "Get succeeded for key: " + key);
        return true;
    }

} // namespace Raft3D