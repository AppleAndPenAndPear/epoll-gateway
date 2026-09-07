#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "http_client.h"

namespace {

int create_listener(uint16_t& port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }

    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
        close(fd);
        return -1;
    }
    port = ntohs(address.sin_port);
    return fd;
}

}

TEST(HttpClientTest, ReturnsGatewayTimeoutWhenBackendDoesNotRespond) {
    uint16_t port = 0;
    int listener = create_listener(port);
    ASSERT_GE(listener, 0);

    std::thread backend([listener]() {
        int connection = accept(listener, nullptr, nullptr);
        if (connection >= 0) {
            char buffer[1024];
            recv(connection, buffer, sizeof(buffer), 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            close(connection);
        }
        close(listener);
    });

    auto start = std::chrono::steady_clock::now();
    BackendResponse response = forward_request(
        "127.0.0.1", port, "GET", "/slow", {}, "", 50);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    EXPECT_EQ(response.status_code, 504);
    EXPECT_EQ(response.error, BackendError::ReadTimeout);
    EXPECT_LT(elapsed.count(), 150);
    backend.join();
}

TEST(HttpClientTest, DistinguishesConnectionFailure) {
    BackendResponse response = forward_request(
        "127.0.0.1", 1, "GET", "/unavailable", {}, "", 50);

    EXPECT_EQ(response.status_code, 502);
    EXPECT_EQ(response.error, BackendError::ConnectFailed);
}
