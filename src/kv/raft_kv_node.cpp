#include "libnuraft/nuraft.hxx"
#include "raft3d_server.hpp"
#include "kv/raft_kv_node.hpp"
#include "kv/raft_kv_state_mgr.hpp"
#include "kv/raft_kv_state_machine.hpp"
#include <string>

namespace Raft3D
{

    RaftKVNode::RaftKVNode(
        int id,
        int raftPort,
        nuraft::ptr<RaftKVStateMachine> stateMachine,
        nuraft::ptr<RaftKVStateManager> stateManager)
        : my_state_machine_(stateMachine),
          my_state_mgr(stateManager)
    {
        auto log_store = my_state_mgr->load_log_store();

        nuraft::ptr<nuraft::context> ctx = nuraft::cs_new<nuraft::context>();
        ctx->state_mgr_ = my_state_mgr;
        ctx->state_machine_ = my_state_machine_;
        // ... set up other context fields as needed ...

        nuraft::raft_server::init_options opts;
        opts.raft_callback_ = nullptr; // Set callback if needed
        opts.start_server_in_constructor_ = true;
        server_ = nuraft::cs_new<nuraft::raft_server>(ctx.get(), opts);

        // Create and start launcher (for networking)
        launcher_ = nuraft::cs_new<nuraft::raft_launcher>();
        launcher_->init(my_state_machine_, my_state_mgr, my_logger_, raftPort, nuraft::asio_service::options(), nuraft::raft_params());
    }

    RaftKVNode::~RaftKVNode()
    {
        if (server_)
        {
            server_->shutdown();
        }
        // launcher_ and other ptrs will auto-cleanup
    }

    // --- Generic Put ---
    int RaftKVNode::_putKey(std::string key, std::string value)
    {
        uint32_t key_size = key.size();
        uint32_t value_size = value.size();
        nuraft::ptr<nuraft::buffer> buf = nuraft::buffer::alloc(4 + key_size + 1 + 4 + value_size);
        buf->pos(0);
        buf->put(key_size);
        buf->put_raw((const nuraft::byte *)key.data(), key_size);
        buf->put((uint8_t)1); // value_type: 1=string
        buf->put(value_size);
        buf->put_raw((const nuraft::byte *)value.data(), value_size);

        nuraft::ptr<nuraft::log_entry> entry = nuraft::cs_new<nuraft::log_entry>(0, buf);
        auto ret = server_->append_entries({entry});
        if (!ret->get_accepted())
            return -1;
        return 0;
    }

    // --- Generic Get ---
    int RaftKVNode::_getKey(std::string key, std::string &value)
    {
        bool ok = my_state_machine_->get(key, value);
        return ok ? 0 : -1;
    }

    // --- Add Server to Cluster ---
    int RaftKVNode::addServer(int id, std::string address)
    {
        // Use NuRaft API to add server
        // Example: server_->add_srv(cs_new<srv_config>(id, address));
        nuraft::ptr<nuraft::srv_config> srv = nuraft::cs_new<nuraft::srv_config>(id, address);
        auto ret = server_->add_srv(*srv);
        return ret->get_accepted() ? 0 : -1;
    }

    // --- List Servers ---
    int RaftKVNode::listServers(std::vector<Raft3DServer> &servers)
    {
        auto config = my_state_mgr->load_config();
        for (auto &srv : config->get_servers())
        {
            servers.push_back({srv->get_id(), srv->get_endpoint()});
        }
        return 0;
    }

} // namespace Raft3D