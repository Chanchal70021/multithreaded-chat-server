#include <iostream>
#include <thread>
#include <vector>
#include <mutex>
#include <string>
#include <unistd.h>       // close()
#include <netinet/in.h>   // sockaddr_in
#include <sys/socket.h>   // socket functions
#include <arpa/inet.h>    // inet_ntoa
#include <algorithm>
#include <cstring>     // remove

std::vector<int> clients;
std::mutex clients_mutex;

void broadcast(const std::string& message, int sender_fd) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (int client_fd : clients) {
        if (client_fd != sender_fd) {
            send(client_fd, message.c_str(), message.length(), 0);
        }
    }
}

void handle_client(int client_socket, sockaddr_in client_addr) {
    char buffer[1024];
    std::string welcome = "Welcome to the chat!\n";
    send(client_socket, welcome.c_str(), welcome.length(), 0);

    while (true) {
        memset(buffer, 0, sizeof(buffer));
        int bytes_received = recv(client_socket, buffer, sizeof(buffer), 0);
        if (bytes_received <= 0) {
            std::cout << "[DISCONNECT] " << inet_ntoa(client_addr.sin_addr) << "\n";
            break;
        }
        std::string message = "[Client " + std::string(inet_ntoa(client_addr.sin_addr)) + "]: " + std::string(buffer);
        std::cout << message;
        broadcast(message, client_socket);
    }

    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        clients.erase(std::remove(clients.begin(), clients.end(), client_socket), clients.end());
    }
    close(client_socket);
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Socket creation failed");
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8888);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("Listen failed");
        return 1;
    }

    std::cout << "[SERVER STARTED] Listening on port 8888...\n";

    while (true) {
        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &addr_len);

        if (client_fd < 0) {
            perror("Accept failed");
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients.push_back(client_fd);
        }

        std::thread(handle_client, client_fd, client_addr).detach();
    }

    close(server_fd);
    return 0;
}

