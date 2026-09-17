#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "connection_pool.h"

namespace {

// A pool entry needs an open fd; a socketpair provides one without networking.
int make_open_fd() {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return -1;
    close(fds[1]);  // keep one end open
    return fds[0];
}

ConnectionPool::PooledConnection make_conn() {
    ConnectionPool::PooledConnection conn;
    const int fd = make_open_fd();
    EXPECT_GE(fd, 0);
    conn.socket = std::shared_ptr<Socket>(new Socket(fd));
    return conn;
}

}  // namespace

TEST(ConnectionPoolTest, CheckoutReturnsCheckedInConnection) {
    ConnectionPool pool;
    auto conn = make_conn();
    int fd = conn.socket->getFd();

    pool.checkin("backend", 8080, std::move(conn));
    auto out = pool.checkout("backend", 8080);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->socket->getFd(), fd);

    // pool is empty again
    EXPECT_FALSE(pool.checkout("backend", 8080).has_value());
}

TEST(ConnectionPoolTest, KeysAreSeparatedByHostPort) {
    ConnectionPool pool;
    pool.checkin("backend", 8080, make_conn());

    EXPECT_FALSE(pool.checkout("backend", 9090).has_value());
    EXPECT_FALSE(pool.checkout("other", 8080).has_value());
    EXPECT_TRUE(pool.checkout("backend", 8080).has_value());
}

TEST(ConnectionPoolTest, IdleTimeoutEvictsEntries) {
    ConnectionPool pool;
    pool.set_idle_timeout_for_testing(std::chrono::seconds(0));
    pool.checkin("backend", 8080, make_conn());

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_FALSE(pool.checkout("backend", 8080).has_value());
}

TEST(ConnectionPoolTest, MaxIdlePerUpstreamDropsExcess) {
    ConnectionPool pool;
    for (size_t i = 0; i <= ConnectionPool::max_idle_per_upstream(); ++i) {
        pool.checkin("backend", 8080, make_conn());
    }
    // exactly max entries survive; checking them all out empties the bucket
    size_t found = 0;
    while (pool.checkout("backend", 8080).has_value()) ++found;
    EXPECT_EQ(found, ConnectionPool::max_idle_per_upstream());
}

TEST(ConnectionPoolTest, InvalidateDropsAllIdleEntries) {
    ConnectionPool pool;
    pool.checkin("backend", 8080, make_conn());
    pool.checkin("backend", 8080, make_conn());
    pool.invalidate("backend", 8080);
    EXPECT_FALSE(pool.checkout("backend", 8080).has_value());
}
