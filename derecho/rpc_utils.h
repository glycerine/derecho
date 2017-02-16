/**
 * @file rpc_utils.h
 *
 * @date Feb 3, 2017
 * @author edward
 */

#pragma once

#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <mutils-serialization/SerializationSupport.hpp>

namespace derecho {

//Copied-and-pasted from derecho_sst.h to avoid creating another header just for this type.
using node_id_t = uint32_t;

namespace rpc {

struct Opcode {
    using t = unsigned long long;
    t id;
    Opcode(const decltype(id)& id) : id(id) {}
    Opcode() = default;
    bool operator==(const Opcode& n) const { return id == n.id; }
    bool operator<(const Opcode& n) const { return id < n.id; }
};
auto& operator<<(std::ostream& out, const Opcode& op) { return out << op.id; }
using FunctionTag = unsigned long long;

using node_list_t = std::vector<node_id_t>;

struct remote_exception_occurred : public std::exception {
    node_id_t who;
    remote_exception_occurred(node_id_t who) : who(who) {}
    virtual const char* what() const noexcept override {
        std::ostringstream o_stream;
        o_stream << "An exception occured at node with id " << who;
        std::string str = o_stream.str();
        return str.c_str();
    }
};

struct node_removed_from_group_exception : public std::exception {
    node_id_t who;
    node_removed_from_group_exception(node_id_t who) : who(who) {}
    virtual const char* what() const noexcept override {
        std::ostringstream o_stream;
        o_stream << "Node with id " << who
                 << " has been removed from the group";
        std::string str = o_stream.str();
        return str.c_str();
    }
};

struct recv_ret {
    Opcode opcode;
    std::size_t size;
    char* payload;
    std::exception_ptr possible_exception;
};

using receive_fun_t = std::function<recv_ret(
    mutils::DeserializationManager* dsm, const node_id_t&, const char* recv_buf,
    const std::function<char*(int)>& out_alloc)>;

template <typename T>
using reply_map = std::map<node_id_t, std::future<T>>;

/**
 * Data structure that holds a set of futures for a single RPC function call;
 * there is one future for each node contacted to make the call, which will
 * eventually contain that node's reply.
 */
template <typename T>
struct QueryResults {
    using map_fut = std::future<std::unique_ptr<reply_map<T>>>;
    using map = reply_map<T>;
    using type = T;

    map_fut pending_rmap;
    QueryResults(map_fut pm) : pending_rmap(std::move(pm)) {}

    struct ReplyMap {
    private:
        QueryResults& parent;

    public:
        map rmap;

        ReplyMap(QueryResults& qr) : parent(qr){};
        ReplyMap(const ReplyMap&) = delete;
        ReplyMap(ReplyMap&& rm) : parent(rm.parent), rmap(std::move(rm.rmap)) {}

        bool valid(const node_id_t& nid) {
            assert(rmap.size() == 0 || rmap.count(nid));
            return (rmap.size() > 0) && rmap.at(nid).valid();
        }

        /*
          returns true if we sent to this node,
          regardless of whether this node has replied.
        */
        bool contains(const node_id_t& nid) { return rmap.count(nid); }

        auto begin() { return std::begin(rmap); }

        auto end() { return std::end(rmap); }

        auto get(const node_id_t& nid) {
            if(rmap.size() == 0) {
                assert(parent.pending_rmap.valid());
                rmap = std::move(*parent.pending_rmap.get());
            }
            assert(rmap.size() > 0);
            assert(rmap.count(nid));
            assert(rmap.at(nid).valid());
            return rmap.at(nid).get();
        }
    };

private:
    ReplyMap replies{*this};

public:
    QueryResults(QueryResults&& o)
            : pending_rmap{std::move(o.pending_rmap)},
              replies{std::move(o.replies)} {}
    QueryResults(const QueryResults&) = delete;

    /**
     * Wait the specified duration; if a ReplyMap is available
     * after that duration, return it. Otherwise return nullptr.
     */
    template <typename Time>
    ReplyMap* wait(Time t) {
        if(replies.rmap.size() == 0) {
            if(pending_rmap.wait_for(t) == std::future_status::ready) {
                replies.rmap = std::move(*pending_rmap.get());
                return &replies;
            } else
                return nullptr;
        } else
            return &replies;
    }

    /**
     * Block until the ReplyMap is fulfilled, then return the map.
     */
    ReplyMap& get() {
        using namespace std::chrono;
        while(true) {
            if(auto rmap = wait(5min)) {
                return *rmap;
            }
        }
    }
};

template <>
struct QueryResults<void> {
    using type = void;
    /* This currently has no functionality; Ken suggested a "flush," which
       we might want to have in both this and the non-void variant.
    */
};

class PendingBase {
public:
    virtual void fulfill_map(const node_list_t&) {
        assert(false);
    }
    virtual void set_exception_for_removed_node(const node_id_t&) {
        assert(false);
    }
    virtual ~PendingBase(){}
};

/**
 * Data structure that holds a set of promises for a single RPC function call;
 * the promises transmit one response (either a value or an exception) for
 * each node that was called. The future ends of these promises are stored in
 * a corresponding QueryResults object.
 */
template <typename T>
struct PendingResults : public PendingBase {
    std::promise<std::unique_ptr<reply_map<T>>> pending_map;
    std::map<node_id_t, std::promise<T>> populated_promises;

    bool map_fulfilled = false;
    std::set<node_id_t> dest_nodes, responded_nodes;

    /**
     * Fill the result map with an entry for each node that will be contacted
     * in this RPC call
     * @param who A list of nodes that will be contacted
     */
    void fulfill_map(const node_list_t& who) {
        map_fulfilled = true;
        std::unique_ptr<reply_map<T>> to_add = std::make_unique<reply_map<T>>();
        for(const auto& e : who) {
            to_add->emplace(e, populated_promises[e].get_future());
        }
        dest_nodes.insert(who.begin(), who.end());
        pending_map.set_value(std::move(to_add));
    }

    void set_exception_for_removed_node(const node_id_t& removed_nid) {
        assert(map_fulfilled);
        if(dest_nodes.find(removed_nid) != dest_nodes.end() &&
           responded_nodes.find(removed_nid) == responded_nodes.end()) {
            set_exception(removed_nid,
                          std::make_exception_ptr(
                              node_removed_from_group_exception{removed_nid}));
        }
    }

    void set_value(const node_id_t& nid, const T& v) {
        responded_nodes.insert(nid);
        populated_promises[nid].set_value(v);
    }

    void set_exception(const node_id_t& nid, const std::exception_ptr e) {
        responded_nodes.insert(nid);
        populated_promises[nid].set_exception(e);
    }

    QueryResults<T> get_future() {
        return QueryResults<T>{pending_map.get_future()};
    }
};

template <>
struct PendingResults<void> : public PendingBase {
    /* This currently has no functionality; Ken suggested a "flush," which
       we might want to have in both this and the non-void variant.
    */

    void fulfill_map(const node_list_t&) {}
    QueryResults<void> get_future() { return QueryResults<void>{}; }
};

/**
 * Utility functions for manipulating the headers of RPC messages
 */
namespace remote_invocation_utilities {

inline std::size_t header_space() {
    return sizeof(std::size_t) + sizeof(Opcode) + sizeof(node_id_t);
    //          size           operation           from
}

inline char* extra_alloc(int i) {
    const auto hs = header_space();
    return (char*)calloc(i + hs, sizeof(char)) + hs;
}

inline void populate_header(char* reply_buf,
                            const std::size_t& payload_size,
                            const Opcode& op, const node_id_t& from) {
    ((std::size_t*)reply_buf)[0] = payload_size;           // size
    ((Opcode*)(sizeof(std::size_t) + reply_buf))[0] = op;  // what
    ((node_id_t*)(sizeof(std::size_t) + sizeof(Opcode) + reply_buf))[0] =
        from;  // from
}

inline void retrieve_header(mutils::DeserializationManager* dsm,
                            char const* const reply_buf,
                            std::size_t& payload_size, Opcode& op,
                            node_id_t& from) {
    payload_size = ((std::size_t const* const)reply_buf)[0];
    op = ((Opcode const* const)(sizeof(std::size_t) + reply_buf))[0];
    from = ((node_id_t const* const)(sizeof(std::size_t) + sizeof(Opcode) +
                                   reply_buf))[0];
}
} // namespace remote_invocation_utilities

} // namespace rpc
} // namespace derecho



