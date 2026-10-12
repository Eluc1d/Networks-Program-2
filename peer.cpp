// Alexander Liu
// EECE 446 - Program 2: Peer-to-Peer Introduction
// Fall 2026

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
using namespace std;

// Send full request in one call
void sendMsg(int sock, const char *buf, size_t len) {
    if (send(sock, buf, len, 0) != (ssize_t)len) perror("send");
}

// JOIN: [0][4 B ID]
void join(int sock, uint32_t id) {
    char msg[5];
    msg[0] = 0;
    uint32_t netId = htonl(id);
    memcpy(msg + 1, &netId, 4);
    sendMsg(sock, msg, 5);
}

// PUBLISH: [1][4 B count][names\0]
void publish(int sock) {
    char msg[1200];
    msg[0] = 1;
    size_t offset = 5;
    uint32_t count = 0;

    error_code ec;
    for (const auto &entry : filesystem::directory_iterator("SharedFiles", ec)) {
        if (!entry.is_regular_file()) continue;
        string name = entry.path().filename().string();
        memcpy(msg + offset, name.c_str(), name.size() + 1);
        offset += name.size() + 1;
        count++;
    }
    if (ec) { cerr << "SharedFiles error" << endl; return; }

    uint32_t netCount = htonl(count);
    memcpy(msg + 1, &netCount, 4);
    sendMsg(sock, msg, offset);
}

// SEARCH: [2][name\0] -> [4 B ID][4 B IP][2 B port]
void search(int sock) {
    string name;
    cout << "Enter a file name: ";
    cin >> name;

    char msg[101];
    msg[0] = 2;
    memcpy(msg + 1, name.c_str(), name.size() + 1);
    sendMsg(sock, msg, name.size() + 2);

    char resp[10];
    for (int got = 0, n; got < 10; got += n)
        if ((n = recv(sock, resp + got, 10 - got, 0)) <= 0) { perror("recv"); return; }

    uint32_t id;
    in_addr ip;
    uint16_t port;
    memcpy(&id, resp, 4);
    memcpy(&ip, resp + 4, 4);
    memcpy(&port, resp + 8, 2);
    id = ntohl(id);
    port = ntohs(port);

    if (id == 0 && ip.s_addr == 0 && port == 0) {
        cout << "File not indexed by registry" << endl;
        return;
    }

    char ipStr[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &ip, ipStr, sizeof(ipStr))) { perror("inet_ntop"); return; }
    cout << "File found at" << endl;
    cout << "Peer " << id << endl;
    cout << ipStr << ":" << port << endl;
}

int main(int argc, char *argv[]) {
    if (argc != 4) { cerr << "Usage: ./peer <registry> <port> <peer ID>" << endl; return 1; }
    uint32_t id = stoul(argv[3]);

    addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(argv[1], argv[2], &hints, &res) != 0) { cerr << "getaddrinfo error" << endl; return 1; }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == -1) { perror("socket"); return 1; }
    if (connect(sock, res->ai_addr, res->ai_addrlen) == -1) { perror("connect"); return 1; }
    freeaddrinfo(res);

    string cmd;
    while (cout << "Enter a command: " && cin >> cmd) {
        if (cmd == "JOIN") join(sock, id);
        else if (cmd == "PUBLISH") publish(sock);
        else if (cmd == "SEARCH") search(sock);
        else if (cmd == "EXIT") break;
    }

    close(sock);
    return 0;
}
