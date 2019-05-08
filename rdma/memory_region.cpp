#include "memory_region.hpp"
#include "exception/rdma_exceptions.hpp"

#include <rdma/fi_domain.h>
#include <arpa/inet.h>
#include <stdio.h>

#include <memory>
#include <tuple>

namespace rdma {

MemoryRegion::MemoryRegion(node::node_id_t remote_id, char* send_buf, char* recv_buf, size_t size)
        : remote_id(remote_id),
          rdma_connection(RDMAConnectionManager::get(remote_id)),
          send_buf(send_buf),
          recv_buf(recv_buf) {
    std::shared_ptr<RDMAConnection> shared_rdma_connection = rdma_connection.lock();
    if(!shared_rdma_connection) {
        throw RDMAConnectionRemoved("RDMA Connection to " + std::to_string(remote_id) + " has been removed");
    }
    if(shared_rdma_connection->is_broken) {
        throw RDMAConnectionBroken("RDMA Connection to " + std::to_string(remote_id) + " is broken");
    }

    // register the write buffer
    FAIL_IF_NONZERO_RETRY_EAGAIN(
            fi_mr_reg(
                    RDMAConnectionManager::g_ctxt.domain, send_buf, size, FI_SEND | FI_RECV | FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE,
                    0, 0, 0, &this->write_mr, NULL),
            // 0, LF_WMR_KEY(r_id), 0, &this->write_mr, NULL),
            "register memory buffer for write",
            CRASH_ON_FAILURE);
    // register the read buffer
    FAIL_IF_NONZERO_RETRY_EAGAIN(
            fi_mr_reg(
                    RDMAConnectionManager::g_ctxt.domain, recv_buf, size, FI_SEND | FI_RECV | FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE,
                    0, 0, 0, &this->read_mr, NULL),
            //0, LF_RMR_KEY(r_id), 0, &this->read_mr, NULL),
            "register memory buffer for read",
            CRASH_ON_FAILURE);

    this->mr_lrkey = fi_mr_key(this->read_mr);
    if(this->mr_lrkey == FI_KEY_NOTAVAIL) {
        CRASH_WITH_MESSAGE("fail to get read memory key.");
    }
    this->mr_lwkey = fi_mr_key(this->write_mr);
    if(this->mr_lwkey == FI_KEY_NOTAVAIL) {
        CRASH_WITH_MESSAGE("fail to get write memory key.");
    }

    // exchange memory addresses
    MRConnectionData local_data;
    MRConnectionData remote_data;

    local_data.mr_key = (uint64_t)htonll(this->mr_lrkey);
    local_data.vaddr = (uint64_t)htonll((uint64_t)this->recv_buf); // for pull mode

    FAIL_IF_ZERO(tcp_exchange(remote_id, local_data, remote_data),"exchange connection management info.",CRASH_ON_FAILURE);

    this->mr_rwkey = (uint64_t)ntohll(remote_data.mr_key);
    this->remote_recv_buf = (char*)ntohll(remote_data.vaddr);
    // initialize remote_send_buf and remote_recv_buf
}

bool MemoryRegion::write_remote(size_t offset, size_t size, bool with_completion) {
    std::shared_ptr<RDMAConnection> shared_rdma_connection = rdma_connection.lock();
    if(!shared_rdma_connection) {
      throw RDMAConnectionRemoved("RDMA Connection to " + std::to_string(remote_id) + " has been removed");
    }
    assert(offset + size <= this->size);
    return shared_rdma_connection->write_remote(send_buf + offset, remote_recv_buf + offset,
                                                size, with_completion, mr_rwkey, mr_lrkey);
}

bool MemoryRegion::sync() const {
    std::shared_ptr<RDMAConnection> shared_rdma_connection = rdma_connection.lock();
    if(!shared_rdma_connection) {
        throw RDMAConnectionRemoved("RDMA Connection to " + std::to_string(remote_id) + " has been removed");
    }
    assert(shared_rdma_connection);
    return shared_rdma_connection->sync();
}
}  // namespace rdma