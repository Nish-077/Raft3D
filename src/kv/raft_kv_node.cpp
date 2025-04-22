#include "libnuraft/asio_service.hxx"
#include "libnuraft/nuraft.hxx"
#include "raft3d_server.hpp"
#include "kv/raft_kv_node.hpp"
#include "kv/raft_kv_state_mgr.hpp"
#include "kv/raft_kv_state_machine.hpp"
#include <string>
#include <raft3d_logger.hpp>

namespace Raft3D
{
    RaftKVNode::RaftKVNode(
        int id,
        int raftPort,
        nuraft::ptr<RaftKVStateMachine> stateMachine,
        nuraft::ptr<RaftKVStateManager> stateManager,
        nuraft::ptr<Raft3DLogger> logger)
        : my_state_machine_(stateMachine),
          my_state_mgr(stateManager),
          my_logger_(logger)
    {
        launcher_ = nuraft::cs_new<nuraft::raft_launcher>();
        launcher_->init(my_state_machine_, my_state_mgr, my_logger_, raftPort, nuraft::asio_service::options(), nuraft::raft_params());
        server_ = launcher_->get_raft_server();
        if (!server_)
        {
            return;
        }
    }

    RaftKVNode::~RaftKVNode()
    {
        if (server_)
        {
            server_->shutdown();
        }
        // launcher_ and other ptrs will auto-cleanup
    }

    int RaftKVNode::put_key(const std::string &key, const std::string &value)
    {
        if (!server_->is_leader())
        {
            return -2; // Special code for not leader
        }
        nuraft::ulong key_size = key.size();
        nuraft::ulong value_size = value.size();
        nuraft::ptr<nuraft::buffer> buf = nuraft::buffer::alloc(8 + key_size + 1 + 8 + value_size);
        buf->pos(0);
        buf->put(key_size);
        buf->put_raw((const nuraft::byte *)key.data(), key_size);
        buf->put((uint8_t)1);
        buf->put(value_size);
        buf->put_raw((const nuraft::byte *)value.data(), value_size);

        auto ret = server_->append_entries({buf});
        if (!ret->get_accepted())
            return -1;
        return 0;
    }

    int RaftKVNode::get_key(std::string key, std::string &value)
    {
        auto *kvsm = dynamic_cast<RaftKVStateMachine *>(my_state_machine_.get());
        if (!kvsm)
            return -1;
        bool ok = kvsm->get(key, value);
        return ok ? 0 : -1;
    }

    int RaftKVNode::addServer(int id, std::string address)
    {
        nuraft::ptr<nuraft::srv_config> srv = nuraft::cs_new<nuraft::srv_config>(id, address);
        auto ret = server_->add_srv(*srv);
        return ret->get_accepted() ? 0 : -1;
    }

    int RaftKVNode::listServers(std::vector<Raft3DServer> &servers)
    {
        auto config = my_state_mgr->load_config();
        for (auto &srv : config->get_servers())
        {
            servers.push_back({srv->get_id(), srv->get_endpoint()});
        }
        return 0;
    }

    std::string RaftKVNode::get_leader_address()
    {
        if (!server_)
            return "";
        auto leader_id = server_->get_leader();
        auto config = my_state_mgr->load_config();
        for (auto &srv : config->get_servers())
        {
            if (srv->get_id() == leader_id)
            {
                return srv->get_endpoint();
            }
        }
        return "";
    }

} // namespace Raft3D