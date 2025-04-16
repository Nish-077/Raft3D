# Raft3D: A Distributed 3D Printer Control System using Raft Consensus Algorithm

## Project Overview

Raft3D is a distributed 3D printer control system built to leverage the Raft consensus algorithm for ensuring fault tolerance, consistency, and high availability in a network of 3D printers. The system enables seamless communication between multiple 3D printers by utilizing Raft for leader election and log replication, while incorporating features such as dynamic printer metadata storage, filament data management, and print job status monitoring.

The goal of this project is to demonstrate the application of the Raft consensus algorithm in a real-world distributed system that controls and monitors 3D printers, ensuring data consistency even during node failures and leader changes.

## Features

- **Raft Consensus Algorithm**: Implementation of the Raft consensus protocol to maintain consistency and fault tolerance across distributed nodes.
- **Fault Tolerance**: The system handles node failures, leader election, and log replication without compromising data consistency.
- **Distributed Control**: Multiple 3D printers are controlled and monitored by a central leader node, allowing efficient task allocation and synchronization.
- **Printer Metadata Storage**: Metadata about each printer, including printer specifications and configuration details, is stored and replicated.
- **Filament and Print Job Tracking**: Filament inventory and print job status are tracked in real-time, ensuring up-to-date information across all nodes.
- **Dynamic Print Jobs**: Users can submit new print jobs, track progress, and update job status while maintaining fault-tolerant processing.
- **Snapshotting**: The system supports state snapshots to efficiently recover after node failures and log replays.

## Architecture

The architecture is based on the Raft consensus algorithm, where a set of 3D printers and a leader node communicate to ensure data consistency and fault tolerance. The leader is responsible for accepting and replicating log entries to the follower nodes. In the case of leader failure, a new leader is elected, and the system continues to operate without disruption.

- **Leader Node**: Coordinates the print job assignments, handles client requests, and maintains consistency across the system.
- **Follower Nodes**: These nodes replicate the state of the leader and handle print jobs when required.
- **Client**: Users can interact with the system to submit print jobs, check status, and manage printer metadata.

## Prerequisites

Before running the project, make sure you have the following installed:

- **CMake** (version 3.10 or higher)
- **C++ Compiler** (g++ or clang++)
- **Boost** (for Crow integration)
- **Docker** (for simulating multiple nodes)

### Additional Libraries

- **NuRaft**: A C++ library for implementing the Raft consensus protocol.
- **Crow**: A header-only C++ web framework used to build a REST API for interacting with the 3D printers.

## Setup and Installation

### 1. Clone the Repository

Clone this repository to your local machine using Git:

```bash
git clone https://github.com/Nish-077/Raft3D.git
```

### 2. Initialize Submodules

This project uses Crow as a submodule. Initialize the submodule by running:

```bash
git submodule update --init --recursive
```

### 3. Install Dependencies

Install the required dependencies on your system:

```bash
sudo apt update
sudo apt install build-essential cmake libboost-all-dev libssl-dev libcurl4-openssl-dev uuid-dev libuv1-dev libasio-dev g++ zlib1g-dev libsnappy-dev libbz2-dev libgflags-dev liblz4-dev libzstd-dev
```

### 4. Build the Project

Navigate to the project directory and create a build folder:

```bash
cd raft3d
mkdir build
cd build
```

Now, run CMake to configure the project and generate the necessary files:

```bash
cmake ..
```

Finally, build the project:

```bash
make -j$(nproc)
```

### 5. Running the System

After the project is successfully built, you can run the system. For example, to run the leader node:

```bash
./raft3d --mode leader
```

To run a follower node:

```bash
./raft3d --mode follower
```

### Docker Setup

If you prefer to use Docker to simulate multiple nodes:

- Build the Docker image:

```bash
docker build -t raft3d .
```

- Run the Docker container:

```bash
docker run -d --name raft3d-node --network host raft3d
```

Repeat for multiple containers to simulate a multi-node environment.

### Usage

#### Submit a Print Job

To submit a print job, send a POST request to the leader node’s API endpoint:

```bash
curl -X POST http://leader-node-ip:port/submit-job -d '{"job": "3D-Print-Job-Details"}'
```

#### Monitor Print Jobs

To check the status of a print job:

```bash
curl http://leader-node-ip:port/job-status/3D-Print-Job-ID
```