#pragma once

#include <string>

namespace Raft3D
{
    struct Raft3DServer
    {
        int id;
        std::string endpoint;
        bool isLeader;
    };

} // namespace Raft3D
