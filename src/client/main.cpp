#include "client.h"
#include <string.h>
#include <iostream>
#include "mylogger.h"
#include <csignal>
#include <atomic>

std::atomic<bool> client_stop_flag{false};
void client_signal_handler(int sig) {
    if (sig == SIGINT) client_stop_flag.store(true);
}

int main() {
    // Initialize client logging (async, writes to logs/client.log)
    Logger::Guard g("logs/client.log");
    std::signal(SIGINT, client_signal_handler);
    auto logger = Logger::get();
    logger->info("Starting client...");

    try {
        Client client("192.168.189.138", 5005);

        client.run();
    } catch (const std::system_error& e) {
        Logger::get()->critical("");
        std::cerr << "Client error: " << e.what() << std::endl;
    }
    return 0;
}