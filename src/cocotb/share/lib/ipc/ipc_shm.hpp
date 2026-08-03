// Copyright cocotb contributors
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

// Shared-memory transport for the cocotb IPC layer.
//
// The server creates one named memory-mapped region containing two SPSC ring
// buffers (requests: Python -> server, responses/callbacks/logs:
// server -> Python) and three named semaphores used as wake-up events:
//
//   /cocotb_ipc.<token>.req   posted by Python when a request is queued
//   /cocotb_ipc.<token>.resp  posted by the server when a response is queued
//   /cocotb_ipc.<token>.conn  posted by Python after it maps the region
//
// The waiting side blocks on the semaphore (event-driven, no polling).
// Frames are `[u32 LE length][payload]`, byte-compatible with the TCP
// transport, and may wrap around the ring end.
//
// The layout is shared with the Python client (src/cocotb/ipc/_shm.py):
//
//   offset 0 : magic u64 ("COCOTBIP")
//   offset 8 : version u32
//   offset 12: state u32 (0 = init, 1 = running, 2 = closed)
//   offset 16: request producer position u64 (Python writes)
//   offset 24: request consumer position u64 (server reads)
//   offset 32: response producer position u64 (server writes)
//   offset 40: response consumer position u64 (Python reads)
//   offset 48: request ring capacity u64
//   offset 56: response ring capacity u64
//   offset 256: request ring (capacity bytes)
//   offset 256 + cap: response ring (capacity bytes)
//
// Memory ordering: producers write the payload, publish the producer
// position with a release store, then post the semaphore; consumers wait on
// the semaphore and read the position with an acquire load. The semaphore
// pair (post/wait) makes the payload writes visible, which is sufficient on
// all targeted platforms; positions are std::atomic<uint64_t> so that the
// compiler keeps the code well-ordered per the C++11 memory model.
//
// No pointer is ever stored in shared memory, only integer positions.

#ifndef COCOTB_IPC_SHM_HPP_
#define COCOTB_IPC_SHM_HPP_

#include "./ipc_base.hpp"

namespace cocotb {
namespace ipc {

// Creates a new shared-memory transport (nullptr on failure).
IpcTransport *create_shm_transport();

}  // namespace ipc
}  // namespace cocotb

#endif /* COCOTB_IPC_SHM_HPP_ */