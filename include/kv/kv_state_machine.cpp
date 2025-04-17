#include "kv_state_machine.hpp"
#include <fstream>
#include <iostream>
#include <sstream>

kv_state_machine::kv_state_machine() {
    // Initialize RocksDB with options
    rocksdb::Options options;
    options.create_if_missing = true;
    auto status = rocksdb::DB::Open(options, db_path_, &db_);
    if (!status.ok()) {
        std::cerr << "Failed to open RocksDB: " << status.ToString() << std::endl;
        exit(1);  // Consider using proper error handling instead of exit
    }
}

kv_state_machine::~kv_state_machine() {
    // Cleanup RocksDB instance
    delete db_;
}

nuraft::ptr<nuraft::buffer> kv_state_machine::commit(uint64_t log_idx, nuraft::buffer& data) {
    // Convert buffer to string for processing commands
    std::string raw((char*)data.data_begin(), data.size());
    auto delim1 = raw.find(':');
    auto delim2 = raw.find(':', delim1 + 1);

    std::string cmd = raw.substr(0, delim1);  // Command type (PUT/DELETE)
    std::string key = raw.substr(delim1 + 1, delim2 - delim1 - 1);  // Key
    std::string value = raw.substr(delim2 + 1);  // Value

    // Lock the RocksDB for thread safety
    std::lock_guard<std::mutex> lock(db_mutex_);

    // Execute the command on RocksDB based on the type (PUT/DELETE)
    if (cmd == "PUT") {
        db_->Put(rocksdb::WriteOptions(), key, value);
    }
    // } else if (cmd == "DELETE") {
    //     db_->Delete(rocksdb::WriteOptions(), key);
    // }

    // Update the index of the last applied log entry
    last_applied_idx_ = log_idx;
    return nullptr;  // Raft doesn't need a response for this operation
}

bool kv_state_machine::get(const std::string& key, std::string& value_out) { //value_out contains value of key
    std::lock_guard<std::mutex> lock(db_mutex_);
    auto status = db_->Get(rocksdb::ReadOptions(), key, &value_out);
    return status.ok();  // Return true if key is found
}


void kv_state_machine::create_snapshot(nuraft::snapshot& s, nuraft::async_result<bool>::handler_type& when_done) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    
    // Open snapshot file for writing the current state of the RocksDB
    std::ofstream snap_out(snapshot_file_);
    rocksdb::Iterator* it = db_->NewIterator(rocksdb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        // Save key-value pairs to snapshot file
        snap_out << it->key().ToString() << '=' << it->value().ToString() << '\n';
    }
    delete it;
    snap_out.close();

    // Store the current term for snapshot (consider it part of the Raft term)
    last_snapshot_term_ = s.get_last_log_term();

    // Signal that the snapshot has been created
    bool result = true;
    nuraft::ptr<std::exception> err = nullptr;
    when_done(result, err);
}

bool kv_state_machine::apply_snapshot(nuraft::snapshot& s) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    // Destroy the current RocksDB and recreate it from scratch
    delete db_;
    rocksdb::DestroyDB(db_path_, rocksdb::Options());
    rocksdb::Options options;
    options.create_if_missing = true;
    rocksdb::DB::Open(options, db_path_, &db_);

    // Restore key-value pairs from the snapshot file
    std::ifstream snap_in(snapshot_file_);
    std::string line;
    while (std::getline(snap_in, line)) {
        auto eq = line.find('=');
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        db_->Put(rocksdb::WriteOptions(), key, value);  // Insert into the new DB
    }

    // Update the last applied index and snapshot term
    last_applied_idx_ = s.get_last_log_idx();
    last_snapshot_term_ = s.get_last_log_term();

    return true;
}

nuraft::ptr<nuraft::snapshot> kv_state_machine::last_snapshot() {
    // Return the latest snapshot metadata
    return nuraft::cs_new<nuraft::snapshot>(last_applied_idx_, last_snapshot_term_);
}
