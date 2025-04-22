#include "libnuraft/nuraft.hxx" // Includes log_entry, buffer, etc.
#include "rocksdb/db.h"
#include <rocksdb/write_batch.h>
#include "kv/raft_kv_log_store.hpp"
#include <vector>
#include <mutex>
#include <stdexcept>
#include <memory>
#include <iostream>
#include <algorithm> // Required for std::min/std::max
#include "raft3d_logger.hpp"

namespace Raft3D
{
    // --- Helper Functions ---

    // Simple serialization for ulong (adjust endianness if needed)
    inline std::string serialize_ulong(nuraft::ulong val, Raft3DLogger *logger = nullptr)
    {
        if (logger)
            logger->put_details(5, __FILE__, __func__, __LINE__, "Serializing ulong: " + std::to_string(val)); // DEBUG level
        return std::string(reinterpret_cast<const char *>(&val), sizeof(val));
    }

    inline nuraft::ulong deserialize_ulong(const rocksdb::Slice &slice, Raft3DLogger *logger = nullptr)
    {
        if (logger)
            logger->put_details(5, __FILE__, __func__, __LINE__, "Deserializing ulong from slice of size: " + std::to_string(slice.size())); // DEBUG level
        if (slice.size() != sizeof(nuraft::ulong))
        {
            if (logger)
                logger->put_details(2, __FILE__, __func__, __LINE__, "Incorrect slice size for ulong deserialization");
            return 0;
        }
        return *reinterpret_cast<const nuraft::ulong *>(slice.data());
    }

    inline std::string serialize_log_entry(const nuraft::ptr<nuraft::log_entry> &entry, Raft3DLogger *logger = nullptr)
    {
        if (logger)
            logger->put_details(5, __FILE__, __func__, __LINE__, "Serializing log_entry"); // DEBUG level
        nuraft::ptr<nuraft::buffer> buf = entry->serialize();
        return std::string(reinterpret_cast<const char *>(buf->data_begin()), buf->size());
    }

    inline nuraft::ptr<nuraft::log_entry> deserialize_log_entry(const rocksdb::Slice &slice, Raft3DLogger *logger = nullptr)
    {
        if (logger)
            logger->put_details(5, __FILE__, __func__, __LINE__, "Deserializing log_entry from slice of size: " + std::to_string(slice.size())); // DEBUG level
        if (slice.empty())
        {
            if (logger)
                logger->put_details(3, __FILE__, __func__, __LINE__, "Empty slice for log_entry deserialization");
            return nullptr;
        }

        nuraft::ptr<nuraft::buffer> buf = nuraft::buffer::alloc(slice.size());
        buf->put_raw(reinterpret_cast<const nuraft::byte *>(slice.data()), slice.size());
        buf->pos(0); // Reset position for deserialization
        return nuraft::log_entry::deserialize(*buf);
    }

    // Internal helper to avoid repeated lock acquisition
    nuraft::ptr<nuraft::log_entry> RaftKVLogStore::entry_at_internal(nuraft::ulong index) const
    {
        if (index == 0)
        {
            if (logger_)
                logger_->put_details(4, __FILE__, __func__, __LINE__, "Returning dummy entry for index 0");
            nuraft::ptr<nuraft::buffer> buf = nuraft::buffer::alloc(sizeof(nuraft::ulong));
            return nuraft::cs_new<nuraft::log_entry>(0, buf);
        }

        if (index < start_idx_ || index > last_idx_)
        {
            if (logger_)
                logger_->put_details(3, __FILE__, __func__, __LINE__, "Index out of bounds in entry_at_internal: " + std::to_string(index));
            return nullptr;
        }

        std::string key = serialize_ulong(index, logger_.get());
        std::string value_str;
        rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), log_cf_handle_.get(), rocksdb::Slice(key), &value_str);

        if (s.IsNotFound())
        {
            if (logger_)
                logger_->put_details(3, __FILE__, __func__, __LINE__, "Log entry not found at index: " + std::to_string(index));
            return nullptr;
        }
        if (!s.ok())
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "RocksDB Get failed for index " + std::to_string(index) + ": " + s.ToString());
            return nullptr;
        }

        return deserialize_log_entry(value_str, logger_.get());
    }

    // --- Constants for Metadata Keys ---
    const std::string KEY_START_INDEX = "_log_start_index_";
    const std::string KEY_LAST_INDEX = "_log_last_index_";

    // --- RaftKVLogStore Implementation ---

    RaftKVLogStore::RaftKVLogStore(std::shared_ptr<rocksdb::DB> rocksdb_instance,
                                   std::shared_ptr<rocksdb::ColumnFamilyHandle> log_column_family_handle,
                                   std::shared_ptr<Raft3DLogger> logger)
        : db_(rocksdb_instance),
          log_cf_handle_(log_column_family_handle),
          logger_(logger),
          start_idx_(1),
          last_idx_(0)
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
                logger_->put_details(2, __FILE__, __func__, __LINE__, "Log column family handle cannot be null.");
            throw std::invalid_argument("Log column family handle cannot be null.");
        }

        // Load persistent start and last indices on initialization
        std::string val_str;
        rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(), rocksdb::Slice(KEY_START_INDEX), &val_str);
        if (s.ok())
        {
            start_idx_ = deserialize_ulong(val_str, logger_.get());
            if (logger_)
                logger_->put_details(4, __FILE__, __func__, __LINE__, "Loaded start_idx_ = " + std::to_string(start_idx_));
        }
        else if (!s.IsNotFound() && logger_)
        {
            logger_->put_details(3, __FILE__, __func__, __LINE__, "Failed to load start_idx_: " + s.ToString());
        }

        s = db_->Get(rocksdb::ReadOptions(), db_->DefaultColumnFamily(), rocksdb::Slice(KEY_LAST_INDEX), &val_str);
        if (s.ok())
        {
            last_idx_ = deserialize_ulong(val_str, logger_.get());
            if (logger_)
                logger_->put_details(4, __FILE__, __func__, __LINE__, "Loaded last_idx_ = " + std::to_string(last_idx_));
        }
        else if (!s.IsNotFound() && logger_)
        {
            logger_->put_details(3, __FILE__, __func__, __LINE__, "Failed to load last_idx_: " + s.ToString());
        }
    }

    nuraft::ulong RaftKVLogStore::next_slot() const
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        return last_idx_ + 1;
    }

    nuraft::ulong RaftKVLogStore::start_index() const
    {
        // Return the cached value, assuming it's updated correctly by compact()
        std::lock_guard<std::mutex> lock(log_mutex_);
        return start_idx_;
    }

    nuraft::ptr<nuraft::log_entry> RaftKVLogStore::last_entry() const
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (last_idx_ < start_idx_)
        { // Log is empty or only contains dummy index 0
            return entry_at_internal(0);
        }
        return entry_at_internal(last_idx_);
    }

    nuraft::ulong RaftKVLogStore::append(nuraft::ptr<nuraft::log_entry> &entry)
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        nuraft::ulong idx_to_append = last_idx_ + 1;

        if (logger_)
            logger_->put_details(5, __FILE__, __func__, __LINE__, "Appending log entry at index " + std::to_string(idx_to_append));

        rocksdb::WriteBatch batch;
        std::string key = serialize_ulong(idx_to_append, logger_.get());
        std::string val = serialize_log_entry(entry, logger_.get());
        batch.Put(log_cf_handle_.get(), rocksdb::Slice(key), rocksdb::Slice(val));

        // Update last index metadata
        std::string last_idx_val = serialize_ulong(idx_to_append, logger_.get());
        batch.Put(db_->DefaultColumnFamily(), rocksdb::Slice(KEY_LAST_INDEX), rocksdb::Slice(last_idx_val));

        rocksdb::Status s = db_->Write(rocksdb::WriteOptions(), &batch);
        if (!s.ok())
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "Failed to append log entry at index " + std::to_string(idx_to_append) + ": " + s.ToString());
            throw std::runtime_error("Failed to append log entry: " + s.ToString());
        }
        else
        {
            last_idx_ = idx_to_append;
            if (logger_)
                logger_->put_details(4, __FILE__, __func__, __LINE__, "Appended log entry at index " + std::to_string(idx_to_append));
        }

        return idx_to_append;
    }

    void RaftKVLogStore::write_at(nuraft::ulong index, nuraft::ptr<nuraft::log_entry> &entry)
    {
        std::lock_guard<std::mutex> lock(log_mutex_);

        if (logger_)
            logger_->put_details(5, __FILE__, __func__, __LINE__, "write_at called for index " + std::to_string(index));

        nuraft::ulong current_last = last_idx_;
        nuraft::ulong new_last = index;

        rocksdb::WriteBatch batch;

        if (index <= current_last)
        {
            std::string start_key = serialize_ulong(index, logger_.get());
            std::string end_key = serialize_ulong(current_last + 1, logger_.get());
            batch.DeleteRange(log_cf_handle_.get(), rocksdb::Slice(start_key), rocksdb::Slice(end_key));
            if (logger_)
                logger_->put_details(4, __FILE__, __func__, __LINE__, "Deleted log entries from index " + std::to_string(index) + " to " + std::to_string(current_last));
        }

        std::string key = serialize_ulong(index, logger_.get());
        std::string val = serialize_log_entry(entry, logger_.get());
        batch.Put(log_cf_handle_.get(), rocksdb::Slice(key), rocksdb::Slice(val));

        std::string last_idx_val = serialize_ulong(new_last, logger_.get());
        batch.Put(db_->DefaultColumnFamily(), rocksdb::Slice(KEY_LAST_INDEX), rocksdb::Slice(last_idx_val));

        rocksdb::Status s = db_->Write(rocksdb::WriteOptions(), &batch);
        if (!s.ok())
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "RocksDB write_at failed at index " + std::to_string(index) + ": " + s.ToString());
            throw std::runtime_error("Failed to write log entry at index " + std::to_string(index) + ": " + s.ToString());
        }
        // Only update last_idx_ if batch succeeded!
        last_idx_ = new_last;
        if (logger_)
            logger_->put_details(4, __FILE__, __func__, __LINE__, "Wrote log entry at index " + std::to_string(index));
    }

    nuraft::ptr<std::vector<nuraft::ptr<nuraft::log_entry>>>
    RaftKVLogStore::log_entries(nuraft::ulong start, nuraft::ulong end)
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        auto entries = nuraft::cs_new<std::vector<nuraft::ptr<nuraft::log_entry>>>();

        // Adjust range based on available logs
        start = std::max(start, start_idx_);
        end = std::min(end, last_idx_ + 1); // end is exclusive

        if (start >= end)
        {
            return entries; // Return empty vector if range is invalid or empty
        }

        entries->reserve(end - start);

        rocksdb::ReadOptions read_options;
        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_options, log_cf_handle_.get()));
        std::string start_key = serialize_ulong(start, logger_.get());

        for (it->Seek(start_key); it->Valid(); it->Next())
        {
            nuraft::ulong current_idx = deserialize_ulong(it->key(), logger_.get());
            if (current_idx >= end)
            {
                break; // Reached end of requested range
            }

            nuraft::ptr<nuraft::log_entry> entry = deserialize_log_entry(it->value(), logger_.get());
            if (entry)
            {
                entries->push_back(entry);
            }
            else
            {
                // Handle deserialization error - log might be corrupted
                if (logger_)
                    logger_->put_details(2, __FILE__, __func__, __LINE__, "Failed to deserialize log entry at index " + std::to_string(current_idx));
                // Maybe return partial results or throw? For now, skip.
            }
        }

        // Check iterator status
        rocksdb::Status s = it->status();
        if (!s.ok())
        {
            // Handle iterator error
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "RocksDB iterator error in log_entries: " + s.ToString());
            // Maybe return partial results or throw?
        }

        return entries;
    }

    nuraft::ptr<nuraft::log_entry> RaftKVLogStore::entry_at(nuraft::ulong index)
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        return entry_at_internal(index);
    }

    nuraft::ulong RaftKVLogStore::term_at(nuraft::ulong index)
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        // entry_at_internal handles index 0 and bounds checks
        nuraft::ptr<nuraft::log_entry> entry = entry_at_internal(index);
        if (!entry)
        {
            // Entry not found or error occurred (entry_at_internal returns nullptr)
            // NuRaft expects 0 if entry doesn't exist (e.g., compacted)
            return 0;
        }
        return entry->get_term();
    }

    nuraft::ptr<nuraft::buffer> RaftKVLogStore::pack(nuraft::ulong index, nuraft::int32 cnt)
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        std::vector<nuraft::ptr<nuraft::buffer>> serialized_entries;
        size_t total_data_size = 0;

        nuraft::ulong end_index = index + cnt;
        // Adjust range based on available logs
        index = std::max(index, start_idx_);
        end_index = std::min(end_index, last_idx_ + 1);

        if (index >= end_index)
        {
            cnt = 0; // No entries to pack
        }
        else
        {
            cnt = end_index - index; // Adjust count based on available range
        }

        serialized_entries.reserve(cnt);

        rocksdb::ReadOptions read_options;
        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_options, log_cf_handle_.get()));
        std::string start_key = serialize_ulong(index, logger_.get());

        for (it->Seek(start_key); it->Valid() && serialized_entries.size() < (size_t)cnt; it->Next())
        {
            nuraft::ulong current_idx = deserialize_ulong(it->key(), logger_.get());
            if (current_idx >= end_index)
                break; // Should not happen if cnt is correct, but safety check

            // Directly use the raw value from RocksDB, assuming it's the serialized log_entry buffer
            nuraft::ptr<nuraft::buffer> buf = nuraft::buffer::alloc(it->value().size());
            buf->put_raw(reinterpret_cast<const nuraft::byte *>(it->value().data()), it->value().size());
            buf->pos(0); // Reset position

            total_data_size += buf->size();
            serialized_entries.push_back(buf);
        }

        // Check iterator status
        rocksdb::Status s = it->status();
        if (!s.ok())
        {
            // Handle iterator error - maybe return empty buffer?
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "RocksDB iterator error in pack: " + s.ToString());
            return nuraft::buffer::alloc(0);
        }

        // Check if we got the expected number of entries
        if (serialized_entries.size() != (size_t)cnt)
        {
            // This might indicate missing entries in the expected range
            if (logger_)
                logger_->put_details(3, __FILE__, __func__, __LINE__, "Warning: pack expected " + std::to_string(cnt) + " entries, but found " + std::to_string(serialized_entries.size()));
            cnt = serialized_entries.size(); // Adjust count to actual number found
        }

        // Allocate buffer for the packed data: count + (size + data) for each entry
        nuraft::ptr<nuraft::buffer> buf_out = nuraft::buffer::alloc(
            sizeof(nuraft::int32) +       // Count
            cnt * sizeof(nuraft::int32) + // Sizes
            total_data_size               // Data
        );

        buf_out->pos(0);
        buf_out->put(cnt); // Number of entries packed

        for (const auto &entry_buf : serialized_entries)
        {
            buf_out->put(static_cast<nuraft::int32>(entry_buf->size()));
            buf_out->put(*entry_buf);
        }

        return buf_out;
    }

    void RaftKVLogStore::apply_pack(nuraft::ulong index, nuraft::buffer &pack)
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        pack.pos(0);
        nuraft::int32 num_logs = pack.get_int();
        if (num_logs <= 0)
            return;

        rocksdb::WriteBatch batch;
        nuraft::ulong last_idx_in_pack = index + num_logs - 1;

        for (nuraft::int32 i = 0; i < num_logs; ++i)
        {
            nuraft::ulong current_idx = index + i;
            nuraft::int32 buf_size = pack.get_int();
            if (pack.pos() + buf_size > pack.size())
            {
                // Error: buffer underflow
                if (logger_)
                    logger_->put_details(2, __FILE__, __func__, __LINE__, "Error applying pack: buffer underflow at index " + std::to_string(current_idx));
                throw std::runtime_error("Buffer underflow during apply_pack");
            }

            // Create Slice directly from pack buffer without extra copy
            rocksdb::Slice val_slice(reinterpret_cast<const char *>(pack.data_begin() + pack.pos()), buf_size);
            std::string key = serialize_ulong(current_idx, logger_.get());

            batch.Put(log_cf_handle_.get(), rocksdb::Slice(key), val_slice);
            pack.pos(pack.pos() + buf_size); // Advance buffer position
        }

        // Update metadata - potentially update both start and last index
        bool metadata_updated = false;            // Tracks if last_idx_ needs update
        bool start_idx_needs_update = false;      // Track if start_idx_ needs update
        nuraft::ulong new_start_idx = start_idx_; // Keep track of potential new start index

        if (index < start_idx_)
        {
            // This case implies the pack contains entries for indices
            // that were previously thought to be compacted. This might
            // happen during snapshot installation if compact() wasn't
            // called first, or in complex recovery scenarios.
            // We should update start_idx_ to the beginning of the pack.
            if (logger_)
                logger_->put_details(3, __FILE__, __func__, __LINE__, "Warning: apply_pack received index " + std::to_string(index) + " which is less than current start_idx_ " + std::to_string(start_idx_) + ". Updating start_idx_.");
            new_start_idx = index;
            start_idx_needs_update = true;
        }
        // Note: If index == start_idx_, no change to start_idx_ is needed.

        if (last_idx_in_pack > last_idx_)
        {
            std::string last_idx_val = serialize_ulong(last_idx_in_pack, logger_.get());
            batch.Put(db_->DefaultColumnFamily(), rocksdb::Slice(KEY_LAST_INDEX), rocksdb::Slice(last_idx_val));
            metadata_updated = true; // Mark last_idx_ as updated in the batch
        }

        // Add start index update to the batch if needed
        if (start_idx_needs_update)
        {
            std::string start_idx_val = serialize_ulong(new_start_idx, logger_.get());
            batch.Put(db_->DefaultColumnFamily(), rocksdb::Slice(KEY_START_INDEX), rocksdb::Slice(start_idx_val));
        }

        rocksdb::Status s = db_->Write(rocksdb::WriteOptions(), &batch);
        if (!s.ok())
        {
            // Handle RocksDB write error
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "ERROR: RocksDB apply_pack failed: " + s.ToString());
            throw std::runtime_error("Failed to apply log pack: " + s.ToString());
        }
        else
        {
            // Update cached metadata only if batch succeeded
            if (metadata_updated)
            {
                last_idx_ = last_idx_in_pack;
            }
            if (start_idx_needs_update)
            {
                start_idx_ = new_start_idx;
            }
        }
    }

    bool RaftKVLogStore::compact(nuraft::ulong last_log_index)
    {
        std::lock_guard<std::mutex> lock(log_mutex_);

        if (logger_)
            logger_->put_details(5, __FILE__, __func__, __LINE__, "Compacting log up to index " + std::to_string(last_log_index));

        // Cannot compact beyond the last committed index
        if (last_log_index > last_idx_)
        {
            if (logger_)
                logger_->put_details(3, __FILE__, __func__, __LINE__, "Attempted to compact beyond last_idx_");
            return false;
        }

        // Cannot compact below the current start index
        if (last_log_index < start_idx_)
        {
            if (logger_)
                logger_->put_details(4, __FILE__, __func__, __LINE__, "Nothing to compact: last_log_index < start_idx_");
            return true;
        }

        nuraft::ulong new_start_index = last_log_index + 1;

        // Use DeleteRange to remove logs from [start_idx_, last_log_index] inclusive
        std::string start_key = serialize_ulong(start_idx_, logger_.get());
        std::string end_key = serialize_ulong(new_start_index, logger_.get()); // DeleteRange end is exclusive

        rocksdb::WriteBatch batch;
        batch.DeleteRange(log_cf_handle_.get(), rocksdb::Slice(start_key), rocksdb::Slice(end_key));

        // Update start index metadata
        std::string start_idx_val = serialize_ulong(new_start_index, logger_.get());
        batch.Put(db_->DefaultColumnFamily(), rocksdb::Slice(KEY_START_INDEX), rocksdb::Slice(start_idx_val));

        rocksdb::Status s = db_->Write(rocksdb::WriteOptions(), &batch);
        if (!s.ok())
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "RocksDB compact failed: " + s.ToString());
            return false;
        }

        // Update cached start index on success
        start_idx_ = new_start_index;
        if (logger_)
            logger_->put_details(4, __FILE__, __func__, __LINE__, "Compacted log up to index " + std::to_string(last_log_index));
        return true;
    }

    bool RaftKVLogStore::flush()
    {
        rocksdb::Status s = db_->SyncWAL(); // Explicitly sync WAL just in case
        if (!s.ok())
        {
            if (logger_)
                logger_->put_details(2, __FILE__, __func__, __LINE__, "RocksDB SyncWAL failed: " + s.ToString());
            return false;
        }
        if (logger_)
            logger_->put_details(5, __FILE__, __func__, __LINE__, "WAL synced successfully.");
        return true;
    }

} // namespace Raft3D
