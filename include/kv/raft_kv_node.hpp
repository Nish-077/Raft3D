#pragma once

#include "libnuraft/nuraft.hxx"
#include "raft3d_server.hpp"
#include "kv/raft_kv_state_mgr.hpp"
#include "kv/raft_kv_state_machine.hpp"
#include <string>
#include <vector>

namespace Raft3D
{
    class RaftKVNode final
    {
    private:
        nuraft::ptr<nuraft::raft_server> server_;
        nuraft::ptr<nuraft::raft_launcher> launcher_;
        nuraft::ptr<nuraft::logger> my_logger_ = nullptr;
        nuraft::ptr<RaftKVStateMachine> my_state_machine_;
        nuraft::ptr<RaftKVStateManager> my_state_mgr;

        int _putKey(std::string key, std::string value);
        int _getKey(std::string key, std::string &value);

    public:
        RaftKVNode(int id, int raftPort, nuraft::ptr<RaftKVStateMachine> stateMachine, nuraft::ptr<RaftKVStateManager> stateManager);
        ~RaftKVNode();

        int addServer(int id, std::string address);
        int listServers(std::vector<Raft3DServer> &servers);
    };
}