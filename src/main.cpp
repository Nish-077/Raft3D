#include "crow.h"
#include "kv/raft_kv_node.hpp"
#include "kv/raft_kv_state_machine.hpp"
#include "kv/raft_kv_state_mgr.hpp"
#include "raft3d_logger.hpp"
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <memory>
#include <vector>
#include <string>
#include <iostream>
#include <crow/json.h>
#include <sstream>
#include <atomic>
#include <chrono>

using namespace Raft3D;

int main(int argc, char *argv[])
{
    if (argc < 4)
    {
        std::cerr << "Usage: " << argv[0] << " <node_id> <raft_port> <db_path>\n";
        return 1;
    }

    int node_id = std::stoi(argv[1]);
    int raft_port = std::stoi(argv[2]);
    std::string db_path = argv[3];

    // Only add the current node as a peer at startup
    std::vector<Raft3DServer> peers = {
        {node_id, "localhost:" + std::to_string(raft_port)}};

    // RocksDB setup
    rocksdb::Options options;
    options.create_if_missing = true;
    options.create_missing_column_families = true;
    std::vector<std::string> cfs = {"default", "log_cf", "state_cf"};
    std::vector<rocksdb::ColumnFamilyDescriptor> cf_descs;
    for (auto &name : cfs)
    {
        cf_descs.emplace_back(name, rocksdb::ColumnFamilyOptions());
    }

    std::vector<rocksdb::ColumnFamilyHandle *> handles;
    rocksdb::DB *db_ptr = nullptr;
    rocksdb::Status s = rocksdb::DB::Open(options, db_path, cf_descs, &handles, &db_ptr);
    if (!s.ok())
    {
        std::cerr << "Failed to open RocksDB: " << s.ToString() << std::endl;
        return 1;
    }

    std::shared_ptr<rocksdb::DB> db(db_ptr);
    std::shared_ptr<rocksdb::ColumnFamilyHandle> log_cf(handles[1], [db](rocksdb::ColumnFamilyHandle *h)
                                                        { if (db && h) db->DestroyColumnFamilyHandle(h); });
    std::shared_ptr<rocksdb::ColumnFamilyHandle> state_cf(handles[2], [db](rocksdb::ColumnFamilyHandle *h)
                                                          { if (db && h) db->DestroyColumnFamilyHandle(h); });

    auto state_machine = nuraft::cs_new<RaftKVStateMachine>(db, state_cf);
    auto state_mgr = nuraft::cs_new<RaftKVStateManager>(node_id, db, log_cf, peers);
    auto logger = nuraft::cs_new<Raft3DLogger>();
    RaftKVNode node(node_id, raft_port, state_machine, state_mgr, logger);

    crow::SimpleApp app;

    // Helper: Generate unique IDs (for demo, use timestamp+counter)
    std::atomic<uint64_t> id_counter{0};
    auto generate_id = [&id_counter]()
    {
        return std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "_" + std::to_string(id_counter++);
    };

    // Add server endpoint (for dynamic cluster expansion)
    CROW_ROUTE(app, "/add_server/<int>/<string>")
    ([&node](int id, const std::string &endpoint)
     {
        if (node.addServer(id, endpoint) == 0)
            return crow::response(200, "Server added\n");
        else
            return crow::response(500, "Failed to add server\n"); });

    CROW_ROUTE(app, "/list_servers").methods("GET"_method)([&node]()
                                                           {
        std::vector<Raft3DServer> servers;
        if (node.listServers(servers) != 0) {
            return crow::response(500, "Failed to list servers\n");
        }
        crow::json::wvalue arr = crow::json::wvalue::list();
        size_t idx = 0;
        for (const auto& srv : servers) {
            crow::json::wvalue obj;
            obj["id"] = srv.id;
            obj["endpoint"] = srv.endpoint;
            arr[idx++] = std::move(obj);
        }
        return crow::response(200, arr.dump() + "\n"); });

    // POST /api/v1/printers
    CROW_ROUTE(app, "/api/v1/printers").methods("POST"_method)([&node, &generate_id](const crow::request &req)
                                                               {
        auto body = crow::json::load(req.body);
        if (!body) return crow::response(400, "Invalid JSON\n");
        crow::json::wvalue wbody = body;

        // Validate required fields
        if (!body.has("company") || !body.has("model"))
            return crow::response(400, "Missing company or model\n");

        std::string id = generate_id();
        wbody["id"] = id;

        // Serialize and store
        std::string key = "printer:" + id;
        int res = node.put_key(key, wbody.dump());
        if (res == -2) {
            std::string leader_addr = node.get_leader_address();
            std::string msg = "Not leader. Leader is at: " + (leader_addr.empty() ? "unknown" : leader_addr) + "\n";
            return crow::response(403, msg);
        }
        if (res != 0)
            return crow::response(500, "Failed to store printer\n");

        // Update printers list
        std::string ids_json;
        crow::json::wvalue id_arr = crow::json::wvalue::list();
        if (node.get_key("printers", ids_json) == 0) {
            auto arr = crow::json::load(ids_json);
            size_t idx = 0;
            for (auto& v : arr)
                id_arr[idx++] = v.s();
        }
        id_arr[id_arr.size()] = id; // add the new printer id
        int list_res = node.put_key("printers", id_arr.dump());
        if (list_res == -2) {
            std::string leader_addr = node.get_leader_address();
            std::string msg = "Not leader. Leader is at: " + (leader_addr.empty() ? "unknown" : leader_addr) + "\n";
            return crow::response(403, msg);
        }
        if (list_res != 0)
            return crow::response(500, "Failed to update printers list\n");

        return crow::response(201, wbody.dump() + "\n"); });

    // GET /api/v1/printers
    CROW_ROUTE(app, "/api/v1/printers").methods("GET"_method)([&node]()
                                                              {
        std::string ids_json;
        if (node.get_key("printers", ids_json) != 0)
            return crow::response(200, "[]\n");
        auto ids = crow::json::load(ids_json);
        crow::json::wvalue arr = crow::json::wvalue::list();
        size_t idx = 0;
        for (auto& id : ids) {
            std::string val;
            if (node.get_key("printer:" + std::string(id.s()), val) == 0)
                arr[idx++] = crow::json::load(val);
        }
        return crow::response(200, arr.dump() + "\n"); });

    // POST /api/v1/filaments
    CROW_ROUTE(app, "/api/v1/filaments").methods("POST"_method)([&node, &generate_id](const crow::request &req)
                                                                {
        auto body = crow::json::load(req.body);
        if (!body) return crow::response(400, "Invalid JSON\n");
        crow::json::wvalue wbody = body;

        // Validate required fields
        if (!body.has("type") || !body.has("color") || !body.has("total_weight_in_grams") || !body.has("remaining_weight_in_grams"))
            return crow::response(400, "Missing required filament fields\n");

        std::string id = generate_id();
        wbody["id"] = id;

        std::string key = "filament:" + id;
        int res = node.put_key(key, wbody.dump());
        if (res == -2) {
            std::string leader_addr = node.get_leader_address();
            std::string msg = "Not leader. Leader is at: " + (leader_addr.empty() ? "unknown" : leader_addr) + "\n";
            return crow::response(403, msg);
        }
        if (res != 0)
            return crow::response(500, "Failed to store filament\n");

        // Update filaments list (store only IDs)
        std::string ids_json;
        crow::json::wvalue id_arr = crow::json::wvalue::list();
        if (node.get_key("filaments", ids_json) == 0) {
            auto arr = crow::json::load(ids_json);
            size_t idx = 0;
            for (auto& v : arr)
                id_arr[idx++] = v.s();
        }
        id_arr[id_arr.size()] = id; // add the new filament id
        int list_res = node.put_key("filaments", id_arr.dump());
        if (list_res == -2) {
            std::string leader_addr = node.get_leader_address();
            std::string msg = "Not leader. Leader is at: " + (leader_addr.empty() ? "unknown" : leader_addr) + "\n";
            return crow::response(403, msg);
        }
        if (list_res != 0)
            return crow::response(500, "Failed to update filaments list\n");

        return crow::response(201, wbody.dump() + "\n"); });

    // GET /api/v1/filaments
    CROW_ROUTE(app, "/api/v1/filaments").methods("GET"_method)([&node]()
                                                               {
        std::string ids_json;
        if (node.get_key("filaments", ids_json) != 0)
            return crow::response(200, "[]\n");
        auto ids = crow::json::load(ids_json);
        crow::json::wvalue arr = crow::json::wvalue::list();
        size_t idx = 0;
        for (auto& id : ids) {
            std::string val;
            if (node.get_key("filament:" + std::string(id.s()), val) == 0)
                arr[idx++] = crow::json::load(val);
        }
        return crow::response(200, arr.dump() + "\n"); });

    // POST /api/v1/print_jobs
    CROW_ROUTE(app, "/api/v1/print_jobs").methods("POST"_method)([&node, &generate_id](const crow::request &req)
                                                                 {
        auto body = crow::json::load(req.body);
        if (!body) return crow::response(400, "Invalid JSON\n");
        crow::json::wvalue wbody = body;

        // Validate required fields
        if (!body.has("printer_id") || !body.has("filament_id") || !body.has("filepath") || !body.has("print_weight_in_grams"))
            return crow::response(400, "Missing required print job fields\n");

        // Validate printer_id exists
        std::string printer_val;
        if (node.get_key("printer:" + std::string(body["printer_id"].s()), printer_val) != 0)
            return crow::response(400, "Invalid printer_id\n");

        // Validate filament_id exists
        std::string filament_val;
        if (node.get_key("filament:" + std::string(body["filament_id"].s()), filament_val) != 0)
            return crow::response(400, "Invalid filament_id\n");

        // Validate print_weight_in_grams does not exceed remaining_weight_in_grams
        auto filament = crow::json::load(filament_val);
        int remaining = std::stoi(filament["remaining_weight_in_grams"].s());
        int requested = body["print_weight_in_grams"].i();

        // TODO: Subtract queued/running jobs' weights for this filament
        if (requested > remaining)
            return crow::response(400, "Not enough filament remaining\n");

        std::string id = generate_id();
        wbody["id"] = id;
        wbody["status"] = "queued";

        std::string key = "print_job:" + id;
        int res = node.put_key(key, wbody.dump());
        if (res == -2) {
            std::string leader_addr = node.get_leader_address();
            std::string msg = "Not leader. Leader is at: " + (leader_addr.empty() ? "unknown" : leader_addr) + "\n";
            return crow::response(403, msg);
        }
        if (res != 0)
            return crow::response(500, "Failed to store print job\n");

        // Update print_jobs list (store only IDs)
        std::string ids_json;
        crow::json::wvalue id_arr = crow::json::wvalue::list();
        if (node.get_key("print_jobs", ids_json) == 0) {
            auto arr = crow::json::load(ids_json);
            size_t idx = 0;
            for (auto& v : arr)
                id_arr[idx++] = v.s();
        }
        id_arr[id_arr.size()] = id; // add the new print job id
        int list_res = node.put_key("print_jobs", id_arr.dump());
        if (list_res == -2) {
            std::string leader_addr = node.get_leader_address();
            std::string msg = "Not leader. Leader is at: " + (leader_addr.empty() ? "unknown" : leader_addr) + "\n";
            return crow::response(403, msg);
        }
        if (list_res != 0)
            return crow::response(500, "Failed to update print jobs list\n");

        return crow::response(201, wbody.dump() + "\n"); });

    // GET /api/v1/print_jobs
    CROW_ROUTE(app, "/api/v1/print_jobs").methods("GET"_method)([&node]()
                                                                {
        std::string ids_json;
        if (node.get_key("print_jobs", ids_json) != 0)
            return crow::response(200, "[]\n");
        auto ids = crow::json::load(ids_json);
        crow::json::wvalue arr = crow::json::wvalue::list();
        size_t idx = 0;
        for (auto& id : ids) {
            std::string val;
            if (node.get_key("print_job:" + std::string(id.s()), val) == 0)
                arr[idx++] = crow::json::load(val);
        }
        return crow::response(200, arr.dump() + "\n"); });

    // POST /api/v1/print_jobs/<string>/status
    CROW_ROUTE(app, "/api/v1/print_jobs/<string>/status").methods("POST"_method)([&node](const crow::request &req, const std::string &job_id)
                                                                                 {
        auto query = crow::query_string(req.url_params);
        std::string status = req.url_params.get("status") ? req.url_params.get("status") : "";
        if (status.empty()) return crow::response(400, "Missing status\n");
        std::string job_json;
        if (node.get_key("print_job:" + job_id, job_json) != 0)
            return crow::response(404, "Print job not found\n");
        auto job = crow::json::load(job_json);
        std::string current_status = job["status"].s();
        if (status == "running" && current_status != "queued")
            return crow::response(400, "Can only go to running from queued\n");
        if (status == "done" && current_status != "running")
            return crow::response(400, "Can only go to done from running\n");
        if (status == "canceled" && !(current_status == "queued" || current_status == "running"))
            return crow::response(400, "Can only cancel from queued or running\n");
        if (status == "done") {
            // Reduce filament's remaining_weight_in_grams
            std::string filament_id = job["filament_id"].s();
            std::string filament_json;
            std::string filament_key = "filament:" + filament_id;
            if (node.get_key(filament_key, filament_json) == 0) {
                auto filament = crow::json::load(filament_json);
                int remaining = std::stoi(filament["remaining_weight_in_grams"].s());
                int used = job["print_weight_in_grams"].i();
                int updated = remaining - used;
                if (updated < 0) updated = 0;
                crow::json::wvalue wfilament = filament;
                wfilament["remaining_weight_in_grams"] = updated;
                int res = node.put_key(filament_key, wfilament.dump());
                if (res == -2) {
                    std::string leader_addr = node.get_leader_address();
                    std::string msg = "Not leader. Leader is at: " + (leader_addr.empty() ? "unknown" : leader_addr) + "\n";
                    return crow::response(403, msg);
                }
                if (res != 0)
                    return crow::response(500, "Failed to update print job status\n");
            }
            else {
                return crow::response(400, "Filament not found\n");
            }
        }
        
        crow::json::wvalue wjob = job;
        wjob["status"] = status;
        if (node.put_key("print_job:" + job_id, wjob.dump()) != 0)
            return crow::response(500, "Failed to update job\n");
        return crow::response(200, wjob.dump() + "\n"); });

    CROW_ROUTE(app, "/is_leader").methods("GET"_method)([&node]()
                                                        {
        bool is_leader = false;
        auto srv = node.get_server();
        if (srv) {
            is_leader = srv->is_leader();
        }
        crow::json::wvalue resp;
        resp["is_leader"] = is_leader;
        if (!is_leader) {
            resp["leader_address"] = node.get_leader_address();
        }
        return crow::response(200, resp.dump() + "\n"); });

    int crow_port = 18080 + node_id;
    std::cout << "RaftKVNode HTTP API running on port " << crow_port << "\n";
    app.port(crow_port).multithreaded().run();
}
