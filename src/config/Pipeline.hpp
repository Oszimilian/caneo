#pragma once

#include "Config.hpp"

#include <string>

// Input pipeline = the graph behind `Config::interfaces`:
//   device (hw / virtual SocketCAN) -> baudrate (gRPC SocketCAN: no baudrate, the server owns the bus) -> [decode] -> live / log
// `interfaces` stays the flat description used by sockets, decoders and the TUI; it is rebuilt from the graph.

// Blocks that create channels: hw_socketcan, virtual_socketcan, grpc_socketcan (remote channels of a caneo gRPC server).
bool pipeline_is_device(const PipelineNodeSpec& n);

// Number of input / output ports of a block.
int pipeline_in_ports(const PipelineNodeSpec& n);
int pipeline_out_ports(const PipelineNodeSpec& n);

// Builds the graph for a config that only has `interfaces:` (old files).
PipelineSpec pipeline_from_interfaces(const Config& cfg);

// Rebuilds cfg.interfaces (+ logging.format) from cfg.pipeline. Actions of interfaces with the same name are kept.
void derive_interfaces(Config& cfg);

// Called after loading: creates the graph from the interfaces if there is none, otherwise derives the interfaces.
void sync_pipeline(Config& cfg);
