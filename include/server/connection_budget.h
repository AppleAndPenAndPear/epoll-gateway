#pragma once
#include <atomic>
#include <cstddef>
#include <optional>

// Process-wide ceiling on concurrent client connections, shared by every worker
// (each worker has its own accept loop, so a per-worker counter would let the
// process hold num_workers * limit connections). Reaching the limit is treated
// as overload: the caller sheds the connection instead of queueing it, which
// bounds file descriptors and memory under a flood.
//
// The slot is held by an RAII Ticket stored inside the connection table entry,
// so releasing happens exactly once when the connection is torn down, no matter
// which path removed it (idle timeout, peer close, handshake failure, forced
// drain). There is no separate release call to forget.
class ConnectionBudget {
public:
    explicit ConnectionBudget(size_t max_connections) : max_(max_connections) {}

    class Ticket {
    public:
        Ticket() = default;
        Ticket(const Ticket&) = delete;
        Ticket& operator=(const Ticket&) = delete;
        Ticket(Ticket&& other) noexcept : owner_(other.owner_) { other.owner_ = nullptr; }
        Ticket& operator=(Ticket&& other) noexcept {
            if (this != &other) {
                reset();
                owner_ = other.owner_;
                other.owner_ = nullptr;
            }
            return *this;
        }
        ~Ticket() { reset(); }

    private:
        friend class ConnectionBudget;
        explicit Ticket(ConnectionBudget* owner) : owner_(owner) {}
        void reset() {
            if (owner_ != nullptr) {
                owner_->release();
                owner_ = nullptr;
            }
        }
        ConnectionBudget* owner_ = nullptr;
    };

    // Reserves one slot, or returns nullopt when the limit is already reached.
    // The reserve-then-compare CAS loop is safe under concurrent workers.
    std::optional<Ticket> try_acquire() {
        size_t current = in_use_.load(std::memory_order_relaxed);
        for (;;) {
            if (current >= max_.load(std::memory_order_relaxed)) {
                return std::nullopt;
            }
            if (in_use_.compare_exchange_weak(current, current + 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
                return Ticket(this);
            }
            // compare_exchange_weak refreshed `current` on failure; retry.
        }
    }

    size_t in_use() const { return in_use_.load(std::memory_order_relaxed); }
    size_t limit() const { return max_.load(std::memory_order_relaxed); }

    // Applies a reloaded limit. Lowering below the current usage does not evict
    // live connections; it only refuses new ones until usage drops back below
    // the new ceiling.
    void set_limit(size_t max_connections) { max_.store(max_connections, std::memory_order_relaxed); }

private:
    void release() { in_use_.fetch_sub(1, std::memory_order_acq_rel); }

    std::atomic<size_t> in_use_{0};
    std::atomic<size_t> max_;
};