#include "client.h"
#include <string.h>
#include <algorithm>
#include <iostream>
#include "mylogger.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <csignal>
#include <atomic>
#include <stdexcept>

std::atomic<bool> client_stop_flag{false};
void client_signal_handler(int sig) {
    if (sig == SIGINT) client_stop_flag.store(true);
}

namespace {
constexpr uint16_t kDefaultPort = 5005;

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  --host <name>  server hostname or IPv4 literal (default: localhost)\n"
              << "  --port <n>     server port (default: " << kDefaultPort << ")\n"
              << "  --path <path>  request path (default: /)\n"
              << "  --ca <file>    trust anchor used to verify the server "
              << "(default: " << ClientTlsConfig{}.ca_path << ")\n"
              << "  --insecure     skip certificate verification (do not use in production)\n"
              << "  --no-tls       speak plaintext HTTP instead of HTTPS\n"
              << "  -h, --help     show this help and exit\n";
}

std::string next_value(int argc, char** argv, int& i, const std::string& flag) {
    if (i + 1 >= argc) throw std::runtime_error("missing value for " + flag);
    return argv[++i];
}
}  // namespace

int main(int argc, char** argv) {
    // Initialize client logging (async, writes to logs/client.log)
    Logger::Guard g("logs/client.log");
    std::signal(SIGINT, client_signal_handler);
    // SSL_write/SSL_shutdown take no MSG_NOSIGNAL flag, so writing to a socket
    // the peer already closed would kill the process with SIGPIPE.
    std::signal(SIGPIPE, SIG_IGN);
    auto logger = Logger::get();
    // The shared logger also mirrors warnings to stdout; the response itself is
    // the client's stdout product (piping is a documented use), so drop that
    // mirror in this process. Everything still goes to logs/client.log.
    auto& sinks = logger->sinks();
    sinks.erase(std::remove_if(sinks.begin(), sinks.end(),
                   [](const spdlog::sink_ptr& s) {
                       return dynamic_cast<spdlog::sinks::stdout_color_sink_mt*>(s.get()) != nullptr;
                   }),
                sinks.end());

    ClientTlsConfig tls;
    std::string host = "localhost";
    std::string path = "/";
    uint16_t port = kDefaultPort;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--host") {
                host = next_value(argc, argv, i, arg);
            } else if (arg == "--port") {
                const std::string value = next_value(argc, argv, i, arg);
                const int parsed = std::stoi(value);
                if (parsed <= 0 || parsed > 65535) {
                    throw std::runtime_error("port out of range: " + value);
                }
                port = static_cast<uint16_t>(parsed);
            } else if (arg == "--path") {
                path = next_value(argc, argv, i, arg);
            } else if (arg == "--ca") {
                tls.ca_path = next_value(argc, argv, i, arg);
            } else if (arg == "--insecure") {
                tls.insecure = true;
            } else if (arg == "--no-tls") {
                tls.enable_tls = false;
            } else if (arg == "-h" || arg == "--help") {
                print_usage(argv[0]);
                return 0;
            } else {
                throw std::runtime_error("unknown option: " + arg);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Client error: " << e.what() << "\n\n";
        print_usage(argv[0]);
        return 2;
    }

    // Also used for SNI. Verification itself is governed by --insecure, so the
    // expected name is inert when verification is switched off.
    tls.verify_host = host;

    logger->info("Starting client...");
    if (!tls.enable_tls) {
        logger->warn("TLS disabled (--no-tls): sending plaintext HTTP");
    } else if (tls.insecure) {
        logger->warn("--insecure set: the server certificate is not verified");
    }

    try {
        Client client(host, port, tls, path);
        const int rc = client.run();
        const std::string response = client.response();
        std::cout << response;
        if (!response.empty() && response.back() != '\n') std::cout << '\n';
        return rc;
    } catch (const std::exception& e) {
        logger->critical("Client error: {}", e.what());
        std::cerr << "Client error: " << e.what() << std::endl;
        return 1;
    }
}