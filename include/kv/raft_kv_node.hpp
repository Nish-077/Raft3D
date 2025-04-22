#pragma once

#include "libnuraft/nuraft.hxx"
#include "raft3d_server.hpp"
#include "kv/raft_kv_state_mgr.hpp"
#include "kv/raft_kv_state_machine.hpp"
#include "raft3d_logger.hpp"
#include <string>
#include <vector>

namespace Raft3D
{
    class RaftKVNode final
    {
    private:
        nuraft::ptr<nuraft::raft_server> server_;
        nuraft::ptr<nuraft::raft_launcher> launcher_;
        nuraft::ptr<nuraft::logger> my_logger_;
        nuraft::ptr<nuraft::state_machine> my_state_machine_;
        nuraft::ptr<nuraft::state_mgr> my_state_mgr;

    public:
        RaftKVNode(int id, int raftPort, nuraft::ptr<RaftKVStateMachine> stateMachine, nuraft::ptr<RaftKVStateManager> stateManager, nuraft::ptr<Raft3DLogger> logger);
        ~RaftKVNode();

        int put_key(const std::string &key, const std::string &value);
        int get_key(std::string key, std::string &value);
        int addServer(int id, std::string address);
        int listServers(std::vector<Raft3DServer> &servers);
        std::string get_leader_address();
        nuraft::ptr<nuraft::raft_server> get_server() const { return server_; }
    };
}