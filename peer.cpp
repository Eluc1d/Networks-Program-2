// Alexander Liu
// EECE 446 - Program 2: Peer-to-Peer Introduction
// Fall 2026
//
// P2P peer that talks to the registry using the JOIN, PUBLISH, and SEARCH
// actions. Usage: ./peer <registry host/IP> <registry port> <peer ID>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint8_t ACTION_JOIN = 0;
constexpr uint8_t ACTION_PUBLISH = 1;
constexpr uint8_t ACTION_SEARCH = 2;

constexpr size_t JOIN_SIZE = 5;              // action + 4 B ID
constexpr size_t MAX_PUBLISH_SIZE = 1200;    // per protocol
constexpr size_t MAX_FILENAME_SIZE = 100;    // includes NULL terminator
constexpr size_t SEARCH_RESPONSE_SIZE = 10;  // 4 B ID + 4 B IPv4 + 2 B port

const char *SHARED_DIR = "SharedFiles";

// Peer information returned by a SEARCH.
struct PeerInfo {
    uint32_t id;    // host byte order
    in_addr addr;   // network byte order (ready for inet_ntop)
    uint16_t port;  // host byte order
};

// Sends the whole buffer. The full request is handed to a single send()
// call; the loop only runs again if the kernel accepts a partial send.
bool send_all(int sock, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(sock, buf + total, len - total, 0);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("send");
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
}

// Receives exactly len bytes. Returns false on error or closed connection.
bool recv_all(int sock, char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(sock, buf + total, len - total, 0);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv");
            return false;
        }
        if (n == 0) {
            std::cerr << "Registry closed the connection" << std::endl;
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
}

// Resolves the registry and returns a connected socket, or -1 on failure.
int connect_to_registry(const char *host, const char *port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *results = nullptr;
    int rc = getaddrinfo(host, port, &hints, &results);
    if (rc != 0) {
        std::cerr << "getaddrinfo: " << gai_strerror(rc) << std::endl;
        return -1;
    }

    int sock = -1;
    for (addrinfo *p = results; p != nullptr; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == -1) {
            perror("socket");
            continue;
        }
        if (connect(sock, p->ai_addr, p->ai_addrlen) == -1) {
            perror("connect");
            if (close(sock) == -1) {
                perror("close");
            }
            sock = -1;
            continue;
        }
        break;  // connected
    }

    freeaddrinfo(results);
    return sock;
}

// JOIN: [0][4 B peer ID]
bool do_join(int sock, uint32_t peer_id) {
    char msg[JOIN_SIZE];
    msg[0] = static_cast<char>(ACTION_JOIN);
    uint32_t net_id = htonl(peer_id);
    std::memcpy(msg + 1, &net_id, sizeof(net_id));
    return send_all(sock, msg, sizeof(msg));
}

// Collects the names of regular files directly inside SharedFiles.
bool list_shared_files(std::vector<std::string> &names) {
    std::error_code ec;
    fs::directory_iterator it(SHARED_DIR, ec);
    if (ec) {
        std::cerr << "Cannot open " << SHARED_DIR << ": " << ec.message()
                  << std::endl;
        return false;
    }

    const fs::directory_iterator end;
    while (it != end) {
        std::error_code type_ec;
        if (it->is_regular_file(type_ec) && !type_ec) {
            names.push_back(it->path().filename().string());
        }
        it.increment(ec);
        if (ec) {
            std::cerr << "Error reading " << SHARED_DIR << ": "
                      << ec.message() << std::endl;
            return false;
        }
    }
    return true;
}

// PUBLISH: [1][4 B count][name\0 name\0 ...]
bool do_publish(int sock) {
    std::vector<std::string> names;
    if (!list_shared_files(names)) {
        return false;
    }

    char msg[MAX_PUBLISH_SIZE];
    msg[0] = static_cast<char>(ACTION_PUBLISH);
    size_t offset = 5;  // leave room for action + count
    uint32_t count = 0;

    for (const std::string &name : names) {
        size_t name_len = name.size() + 1;  // include NULL
        if (name_len > MAX_FILENAME_SIZE) {
            std::cerr << "Skipping file with name too long: " << name
                      << std::endl;
            continue;
        }
        if (offset + name_len > MAX_PUBLISH_SIZE) {
            std::cerr << "PUBLISH request too large; remaining files skipped"
                      << std::endl;
            break;
        }
        std::memcpy(msg + offset, name.c_str(), name_len);
        offset += name_len;
        ++count;
    }

    uint32_t net_count = htonl(count);
    std::memcpy(msg + 1, &net_count, sizeof(net_count));

    return send_all(sock, msg, offset);
}

// SEARCH: sends [2][name\0] and fills result from the 10 B response.
// Returns false on a communication error. A file that is not indexed is
// reported as an all-zero PeerInfo.
bool search_registry(int sock, const std::string &filename, PeerInfo &result) {
    size_t name_len = filename.size() + 1;  // include NULL
    if (name_len > MAX_FILENAME_SIZE) {
        std::cerr << "File name too long (max " << MAX_FILENAME_SIZE - 1
                  << " characters)" << std::endl;
        return false;
    }

    char msg[1 + MAX_FILENAME_SIZE];
    msg[0] = static_cast<char>(ACTION_SEARCH);
    std::memcpy(msg + 1, filename.c_str(), name_len);
    if (!send_all(sock, msg, 1 + name_len)) {
        return false;
    }

    char resp[SEARCH_RESPONSE_SIZE];
    if (!recv_all(sock, resp, sizeof(resp))) {
        return false;
    }

    uint32_t net_id;
    uint16_t net_port;
    std::memcpy(&net_id, resp, 4);
    std::memcpy(&result.addr.s_addr, resp + 4, 4);
    std::memcpy(&net_port, resp + 8, 2);

    result.id = ntohl(net_id);
    result.port = ntohs(net_port);
    return true;
}

void print_search_result(const PeerInfo &peer) {
    if (peer.id == 0 && peer.addr.s_addr == 0 && peer.port == 0) {
        std::cout << "File not indexed by registry" << std::endl;
        return;
    }

    char ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &peer.addr, ip, sizeof(ip)) == nullptr) {
        perror("inet_ntop");
        return;
    }

    std::cout << "File found at" << std::endl;
    std::cout << "Peer " << peer.id << std::endl;
    std::cout << ip << ":" << peer.port << std::endl;
}

// Parses the peer ID; must be a positive number less than 65,535.
bool parse_peer_id(const char *arg, uint32_t &id) {
    errno = 0;
    char *end = nullptr;
    unsigned long value = std::strtoul(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0' || arg[0] == '-' ||
        value == 0 || value >= 65535) {
        return false;
    }
    id = static_cast<uint32_t>(value);
    return true;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <registry host> <registry port> <peer ID>" << std::endl;
        return EXIT_FAILURE;
    }

    uint32_t peer_id;
    if (!parse_peer_id(argv[3], peer_id)) {
        std::cerr << "Peer ID must be a positive number less than 65535"
                  << std::endl;
        return EXIT_FAILURE;
    }

    int sock = connect_to_registry(argv[1], argv[2]);
    if (sock == -1) {
        std::cerr << "Unable to connect to registry" << std::endl;
        return EXIT_FAILURE;
    }

    int status = EXIT_SUCCESS;
    std::string command;

    while (true) {
        std::cout << "Enter a command: " << std::flush;
        if (!(std::cin >> command)) {
            break;  // EOF on stdin: exit cleanly
        }

        if (command == "JOIN") {
            if (!do_join(sock, peer_id)) {
                status = EXIT_FAILURE;
                break;
            }
        } else if (command == "PUBLISH") {
            if (!do_publish(sock)) {
                std::cerr << "PUBLISH failed" << std::endl;
            }
        } else if (command == "SEARCH") {
            std::cout << "Enter a file name: " << std::flush;
            std::string filename;
            if (!std::getline(std::cin >> std::ws, filename)) {
                break;
            }
            PeerInfo peer{};
            if (search_registry(sock, filename, peer)) {
                print_search_result(peer);
            } else {
                std::cerr << "SEARCH failed" << std::endl;
            }
        } else if (command == "EXIT") {
            break;
        } else {
            std::cout << "Unknown command. Use JOIN, PUBLISH, SEARCH, or EXIT."
                      << std::endl;
        }
    }

    if (close(sock) == -1) {
        perror("close");
        status = EXIT_FAILURE;
    }
    return status;
}
