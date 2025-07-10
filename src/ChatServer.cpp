// server.cpp - Advanced Multi-Room Chat Server
#include <iostream>
#include <thread>
#include <vector>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <regex>
#include <queue>
#include <condition_variable>
#include <iomanip>
#include <algorithm>
#include <random>

// Network headers
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <cstring>
#include <sys/resource.h>
#include <signal.h>

// ANSI Color Codes for terminal styling
namespace Colors {
    const std::string RESET = "\033[0m";
    const std::string BOLD = "\033[1m";
    const std::string RED = "\033[31m";
    const std::string GREEN = "\033[32m";
    const std::string YELLOW = "\033[33m";
    const std::string BLUE = "\033[34m";
    const std::string MAGENTA = "\033[35m";
    const std::string CYAN = "\033[36m";
    const std::string WHITE = "\033[37m";
    const std::string BG_BLACK = "\033[40m";
    const std::string BG_RED = "\033[41m";
    const std::string BG_GREEN = "\033[42m";
    const std::string BG_BLUE = "\033[44m";
}

// Forward declarations
class ChatRoom;
class ClientSession;
class ChatServer;
class Logger;
class UserManager;

// Global server instance for signal handling
ChatServer* g_server = nullptr;

// Utility classes
class Logger {
private:
    std::mutex log_mutex;
    std::ofstream log_file;
    
public:
    Logger() : log_file("chat_server.log", std::ios::app) {}
    
    void log(const std::string& level, const std::string& message) {
        std::lock_guard<std::mutex> lock(log_mutex);
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        
        std::ostringstream oss;
        oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
        std::string timestamp = oss.str();
        
        std::string log_entry = "[" + timestamp + "] [" + level + "] " + message;
        
        std::cout << log_entry << std::endl;
        log_file << log_entry << std::endl;
        log_file.flush();
    }
    
    void info(const std::string& msg) { log("INFO", msg); }
    void warn(const std::string& msg) { log("WARN", msg); }
    void error(const std::string& msg) { log("ERROR", msg); }
    
    ~Logger() {
        if (log_file.is_open()) {
            log_file.close();
        }
    }
};

class Message {
public:
    std::string content;
    std::string sender;
    std::string room;
    std::chrono::system_clock::time_point timestamp;
    std::string type; // "chat", "system", "private", "command"
    
    Message(const std::string& c, const std::string& s, const std::string& r, const std::string& t = "chat")
        : content(c), sender(s), room(r), type(t), timestamp(std::chrono::system_clock::now()) {}
    
    std::string serialize() const {
        auto time_t = std::chrono::system_clock::to_time_t(timestamp);
        std::ostringstream oss;
        oss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
        std::string time_str = oss.str();
        
        if (type == "system") {
            return Colors::YELLOW + "[SYSTEM] " + content + Colors::RESET + "\n";
        } else if (type == "private") {
            return Colors::MAGENTA + "[PRIVATE] " + time_str + " " + sender + ": " + content + Colors::RESET + "\n";
        } else {
            return Colors::CYAN + "[" + time_str + "] " + Colors::GREEN + sender + Colors::RESET + ": " + content + "\n";
        }
    }
};

class ClientSession {
public:
    int socket_fd;
    std::string username;
    std::string current_room;
    std::string ip_address;
    std::chrono::system_clock::time_point connect_time;
    std::atomic<bool> authenticated{false};
    std::atomic<bool> is_admin{false};
    std::mutex send_mutex;
    
    ClientSession(int fd, const std::string& ip) 
        : socket_fd(fd), ip_address(ip), connect_time(std::chrono::system_clock::now()) {}
    
    void send_message(const std::string& message) {
        std::lock_guard<std::mutex> lock(send_mutex);
        send(socket_fd, message.c_str(), message.length(), MSG_NOSIGNAL);
    }
    
    void send_colored_message(const std::string& message, const std::string& color = "") {
        send_message(color + message + Colors::RESET);
    }
};

class ChatRoom {
private:
    std::string name;
    std::string password;
    std::unordered_set<std::shared_ptr<ClientSession>> members;
    std::vector<Message> message_history;
    mutable std::mutex room_mutex;
    std::atomic<int> max_members{50};
    bool is_private;
    
public:
    ChatRoom(const std::string& room_name, const std::string& room_password = "", bool private_room = false)
        : name(room_name), password(room_password), is_private(private_room) {}
    
    bool add_member(std::shared_ptr<ClientSession> client, const std::string& provided_password = "") {
        std::lock_guard<std::mutex> lock(room_mutex);
        
        if (members.size() >= max_members) {
            return false;
        }
        
        if (!password.empty() && provided_password != password) {
            return false;
        }
        
        members.insert(client);
        client->current_room = name;
        
        // Send room history to new member
        for (const auto& msg : message_history) {
            client->send_message(msg.serialize());
        }
        
        // Notify others
        Message join_msg("User " + client->username + " joined the room", "SYSTEM", name, "system");
        broadcast_message(join_msg, client->socket_fd);
        
        return true;
    }
    
    void remove_member(std::shared_ptr<ClientSession> client) {
        std::lock_guard<std::mutex> lock(room_mutex);
        members.erase(client);
        
        Message leave_msg("User " + client->username + " left the room", "SYSTEM", name, "system");
        broadcast_message(leave_msg, client->socket_fd);
    }
    
    void broadcast_message(const Message& message, int sender_fd = -1) {
        std::lock_guard<std::mutex> lock(room_mutex);
        
        // Store message in history (keep last 100 messages)
        message_history.push_back(message);
        if (message_history.size() > 100) {
            message_history.erase(message_history.begin());
        }
        
        // Broadcast to all members except sender
        for (const auto& member : members) {
            if (member->socket_fd != sender_fd) {
                member->send_message(message.serialize());
            }
        }
    }
    
    std::vector<std::string> get_member_list() const {
        std::lock_guard<std::mutex> lock(room_mutex);
        std::vector<std::string> list;
        for (const auto& member : members) {
            list.push_back(member->username);
        }
        return list;
    }
    
    size_t get_member_count() const {
        std::lock_guard<std::mutex> lock(room_mutex);
        return members.size();
    }
    
    std::string get_name() const { return name; }
    bool requires_password() const { return !password.empty(); }
    bool is_private_room() const { return is_private; }
};

class UserManager {
private:
    std::unordered_map<std::string, std::string> registered_users; // username -> password
    std::unordered_set<std::string> admin_users;
    std::mutex user_mutex;
    
public:
    UserManager() {
        // Default admin user
        registered_users["admin"] = "admin123";
        admin_users.insert("admin");
    }
    
    bool authenticate_user(const std::string& username, const std::string& password) {
        std::lock_guard<std::mutex> lock(user_mutex);
        auto it = registered_users.find(username);
        return it != registered_users.end() && it->second == password;
    }
    
    bool register_user(const std::string& username, const std::string& password) {
        std::lock_guard<std::mutex> lock(user_mutex);
        if (registered_users.find(username) != registered_users.end()) {
            return false; // User already exists
        }
        registered_users[username] = password;
        return true;
    }
    
    bool is_admin(const std::string& username) {
        std::lock_guard<std::mutex> lock(user_mutex);
        return admin_users.find(username) != admin_users.end();
    }
    
    void promote_to_admin(const std::string& username) {
        std::lock_guard<std::mutex> lock(user_mutex);
        admin_users.insert(username);
    }
};

class ChatServer {
private:
    int server_fd;
    int epoll_fd;
    std::atomic<bool> running{true};
    std::unordered_map<int, std::shared_ptr<ClientSession>> clients;
    std::unordered_map<std::string, std::shared_ptr<ChatRoom>> rooms;
    std::mutex clients_mutex;
    std::mutex rooms_mutex;
    Logger logger;
    UserManager user_manager;
    std::thread server_thread;
    std::thread stats_thread;
    
    // Statistics
    std::atomic<int> total_connections{0};
    std::atomic<int> active_connections{0};
    std::atomic<int> total_messages{0};
    std::chrono::system_clock::time_point server_start_time;
    
public:
    ChatServer(int port = 8888) : server_start_time(std::chrono::system_clock::now()) {
        // Create default rooms
        create_room("general", "", false);
        create_room("admin", "admin123", true);
        create_room("random", "", false);
        
        // Setup server socket
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd == -1) {
            throw std::runtime_error("Socket creation failed");
        }
        
        // Set socket options
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        // Setup address
        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        server_addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            throw std::runtime_error("Bind failed");
        }
        
        if (listen(server_fd, 20) < 0) {
            throw std::runtime_error("Listen failed");
        }
        
        // Setup epoll
        epoll_fd = epoll_create1(0);
        if (epoll_fd == -1) {
            throw std::runtime_error("Epoll creation failed");
        }
        
        epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = server_fd;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);
        
        logger.info("🚀 Advanced Chat Server started on port " + std::to_string(port));
        print_banner();
    }
    
    void print_banner() {
        std::cout << Colors::CYAN << Colors::BOLD << R"(
╔══════════════════════════════════════════════════════════════════════════════╗
║                          🎭 ADVANCED CHAT SERVER 🎭                          ║
║                                                                              ║
║  Features: Multi-room chat, User authentication, Admin panel,               ║
║           Message history, Private messaging, Room management               ║
║                                                                              ║
║  Commands: /help, /join <room>, /create <room>, /list, /who,                ║
║           /msg <user> <message>, /kick <user>, /stats                       ║
╚══════════════════════════════════════════════════════════════════════════════╝
        )" << Colors::RESET << std::endl;
    }
    
    void run() {
        server_thread = std::thread([this]() {
            const int MAX_EVENTS = 64;
            epoll_event events[MAX_EVENTS];
            
            while (running) {
                int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
                
                for (int i = 0; i < nfds; i++) {
                    if (events[i].data.fd == server_fd) {
                        accept_connection();
                    } else {
                        handle_client_message(events[i].data.fd);
                    }
                }
            }
        });
        
        // Start statistics thread
        stats_thread = std::thread([this]() {
            while (running) {
                std::this_thread::sleep_for(std::chrono::seconds(30));
                log_statistics();
            }
        });
        
        logger.info("Server threads started. Press Ctrl+C to stop.");
        
        server_thread.join();
        if (stats_thread.joinable()) {
            stats_thread.join();
        }
    }
    
    void accept_connection() {
        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &addr_len);
        
        if (client_fd < 0) {
            logger.error("Accept failed");
            return;
        }
        
        // Set non-blocking
        fcntl(client_fd, F_SETFL, O_NONBLOCK);
        
        // Add to epoll
        epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = client_fd;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
        
        std::string client_ip = inet_ntoa(client_addr.sin_addr);
        auto client = std::make_shared<ClientSession>(client_fd, client_ip);
        
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients[client_fd] = client;
        }
        
        total_connections++;
        active_connections++;
        
        logger.info("🔗 New connection from " + client_ip + " (FD: " + std::to_string(client_fd) + ")");
        
        // Send welcome message
        std::string welcome = Colors::GREEN + Colors::BOLD + 
                             "🎉 Welcome to Advanced Chat Server!\n" +
                             "Please authenticate: /login <username> <password>\n" +
                             "New user? Register with: /register <username> <password>\n" +
                             "Type /help for available commands\n" + Colors::RESET;
        client->send_message(welcome);
    }
    
    void handle_client_message(int client_fd) {
        char buffer[4096];
        ssize_t bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_received <= 0) {
            disconnect_client(client_fd);
            return;
        }
        
        buffer[bytes_received] = '\0';
        std::string message(buffer);
        
        // Remove trailing newline
        if (!message.empty() && message.back() == '\n') {
            message.pop_back();
        }
        
        std::shared_ptr<ClientSession> client;
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            auto it = clients.find(client_fd);
            if (it == clients.end()) return;
            client = it->second;
        }
        
        if (message.empty()) return;
        
        total_messages++;
        
        if (message[0] == '/') {
            handle_command(client, message);
        } else {
            handle_chat_message(client, message);
        }
    }
    
    void handle_command(std::shared_ptr<ClientSession> client, const std::string& cmd) {
        std::istringstream iss(cmd);
        std::string command;
        iss >> command;
        
        if (command == "/help") {
            send_help(client);
        } else if (command == "/login") {
            std::string username, password;
            iss >> username >> password;
            authenticate_user(client, username, password);
        } else if (command == "/register") {
            std::string username, password;
            iss >> username >> password;
            register_user(client, username, password);
        } else if (command == "/join") {
            std::string room_name, password;
            iss >> room_name >> password;
            join_room(client, room_name, password);
        } else if (command == "/create") {
            std::string room_name, password;
            iss >> room_name >> password;
            create_room(room_name, password, false);
            client->send_colored_message("Room '" + room_name + "' created successfully!", Colors::GREEN);
        } else if (command == "/list") {
            list_rooms(client);
        } else if (command == "/who") {
            list_room_members(client);
        } else if (command == "/msg") {
            std::string target_user;
            iss >> target_user;
            std::string private_msg;
            std::getline(iss, private_msg);
            if (!private_msg.empty()) private_msg = private_msg.substr(1); // Remove leading space
            send_private_message(client, target_user, private_msg);
        } else if (command == "/kick" && client->is_admin) {
            std::string target_user;
            iss >> target_user;
            kick_user(client, target_user);
        } else if (command == "/stats") {
            show_statistics(client);
        } else if (command == "/quit") {
            disconnect_client(client->socket_fd);
        } else {
            client->send_colored_message("Unknown command. Type /help for available commands.", Colors::RED);
        }
    }
    
    void handle_chat_message(std::shared_ptr<ClientSession> client, const std::string& message) {
        if (!client->authenticated) {
            client->send_colored_message("Please authenticate first with /login or /register", Colors::RED);
            return;
        }
        
        if (client->current_room.empty()) {
            client->send_colored_message("Please join a room first with /join <room_name>", Colors::YELLOW);
            return;
        }
        
        std::lock_guard<std::mutex> lock(rooms_mutex);
        auto room_it = rooms.find(client->current_room);
        if (room_it != rooms.end()) {
            Message msg(message, client->username, client->current_room);
            room_it->second->broadcast_message(msg, client->socket_fd);
        }
    }
    
    void authenticate_user(std::shared_ptr<ClientSession> client, const std::string& username, const std::string& password) {
        if (user_manager.authenticate_user(username, password)) {
            client->username = username;
            client->authenticated = true;
            client->is_admin = user_manager.is_admin(username);
            
            std::string welcome = "✅ Authentication successful! Welcome, " + username + "!";
            if (client->is_admin) {
                welcome += " [ADMIN]";
            }
            client->send_colored_message(welcome, Colors::GREEN);
            
            // Auto-join general room
            join_room(client, "general");
            
            logger.info("User " + username + " authenticated from " + client->ip_address);
        } else {
            client->send_colored_message("❌ Authentication failed. Invalid username or password.", Colors::RED);
        }
    }
    
    void register_user(std::shared_ptr<ClientSession> client, const std::string& username, const std::string& password) {
        if (username.empty() || password.empty()) {
            client->send_colored_message("❌ Username and password cannot be empty.", Colors::RED);
            return;
        }
        
        if (user_manager.register_user(username, password)) {
            client->send_colored_message("✅ Registration successful! You can now login with /login " + username + " <password>", Colors::GREEN);
            logger.info("New user registered: " + username);
        } else {
            client->send_colored_message("❌ Registration failed. Username already exists.", Colors::RED);
        }
    }
    
    void join_room(std::shared_ptr<ClientSession> client, const std::string& room_name, const std::string& password = "") {
        if (!client->authenticated) {
            client->send_colored_message("Please authenticate first.", Colors::RED);
            return;
        }
        
        std::lock_guard<std::mutex> lock(rooms_mutex);
        auto room_it = rooms.find(room_name);
        if (room_it == rooms.end()) {
            client->send_colored_message("Room '" + room_name + "' does not exist.", Colors::RED);
            return;
        }
        
        // Leave current room
        if (!client->current_room.empty()) {
            auto current_room_it = rooms.find(client->current_room);
            if (current_room_it != rooms.end()) {
                current_room_it->second->remove_member(client);
            }
        }
        
        // Join new room
        if (room_it->second->add_member(client, password)) {
            client->send_colored_message("✅ Joined room '" + room_name + "'", Colors::GREEN);
            logger.info("User " + client->username + " joined room " + room_name);
        } else {
            client->send_colored_message("❌ Failed to join room. Room might be full or password incorrect.", Colors::RED);
        }
    }
    
    void create_room(const std::string& room_name, const std::string& password = "", bool is_private = false) {
        std::lock_guard<std::mutex> lock(rooms_mutex);
        if (rooms.find(room_name) == rooms.end()) {
            rooms[room_name] = std::make_shared<ChatRoom>(room_name, password, is_private);
            logger.info("Room '" + room_name + "' created");
        }
    }
    
    void list_rooms(std::shared_ptr<ClientSession> client) {
        std::lock_guard<std::mutex> lock(rooms_mutex);
        std::string room_list = Colors::CYAN + "📋 Available Rooms:\n" + Colors::RESET;
        
        for (const auto& [name, room] : rooms) {
            if (!room->is_private_room() || client->is_admin) {
                room_list += "  🏠 " + name + " (" + std::to_string(room->get_member_count()) + " users)";
                if (room->requires_password()) {
                    room_list += " 🔒";
                }
                room_list += "\n";
            }
        }
        
        client->send_message(room_list);
    }
    
    void list_room_members(std::shared_ptr<ClientSession> client) {
        if (client->current_room.empty()) {
            client->send_colored_message("You are not in any room.", Colors::YELLOW);
            return;
        }
        
        std::lock_guard<std::mutex> lock(rooms_mutex);
        auto room_it = rooms.find(client->current_room);
        if (room_it != rooms.end()) {
            auto members = room_it->second->get_member_list();
            std::string member_list = Colors::CYAN + "👥 Members in '" + client->current_room + "':\n" + Colors::RESET;
            
            for (const auto& member : members) {
                member_list += "  👤 " + member + "\n";
            }
            
            client->send_message(member_list);
        }
    }
    
    void send_private_message(std::shared_ptr<ClientSession> sender, const std::string& target_user, const std::string& message) {
        if (!sender->authenticated) {
            sender->send_colored_message("Please authenticate first.", Colors::RED);
            return;
        }
        
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (const auto& [fd, client] : clients) {
            if (client->username == target_user && client->authenticated) {
                Message private_msg(message, sender->username, "", "private");
                client->send_message(private_msg.serialize());
                sender->send_colored_message("Private message sent to " + target_user, Colors::GREEN);
                return;
            }
        }
        
        sender->send_colored_message("User '" + target_user + "' not found or not online.", Colors::RED);
    }
    
    void kick_user(std::shared_ptr<ClientSession> admin, const std::string& target_user) {
        if (!admin->is_admin) {
            admin->send_colored_message("❌ You don't have permission to kick users.", Colors::RED);
            return;
        }
        
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (const auto& [fd, client] : clients) {
            if (client->username == target_user && client->authenticated) {
                client->send_colored_message("⚠️ You have been kicked by an administrator.", Colors::RED);
                disconnect_client(fd);
                admin->send_colored_message("✅ User '" + target_user + "' has been kicked.", Colors::GREEN);
                logger.info("User " + target_user + " kicked by admin " + admin->username);
                return;
            }
        }
        
        admin->send_colored_message("User '" + target_user + "' not found.", Colors::RED);
    }
    
    void show_statistics(std::shared_ptr<ClientSession> client) {
        auto now = std::chrono::system_clock::now();
        auto server_uptime = now - server_start_time;
        auto hours = std::chrono::duration_cast<std::chrono::hours>(server_uptime).count();
        auto minutes = std::chrono::duration_cast<std::chrono::minutes>(server_uptime % std::chrono::hours(1)).count();
        
        std::string stats = Colors::CYAN + Colors::BOLD + "📊 Server Statistics:\n" + Colors::RESET;
        stats += "  🔗 Total connections: " + std::to_string(total_connections.load()) + "\n";
        stats += "  👥 Active connections: " + std::to_string(active_connections.load()) + "\n";
        stats += "  💬 Total messages: " + std::to_string(total_messages.load()) + "\n";
        stats += "  🏠 Total rooms: " + std::to_string(rooms.size()) + "\n";
        stats += "  ⏰ Server uptime: " + std::to_string(hours) + "h " + std::to_string(minutes) + "m\n";
        
        client->send_message(stats);
    }
    
    void send_help(std::shared_ptr<ClientSession> client) {
        std::string help = Colors::YELLOW + Colors::BOLD + "🆘 Available Commands:\n" + Colors::RESET;
        help += "  /login <username> <password> - Authenticate\n";
        help += "  /register <username> <password> - Register new account\n";
        help += "  /join <room> [password] - Join a chat room\n";
        help += "  /create <room> [password] - Create a new room\n";
        help += "  /list - List available rooms\n";
        help += "  /who - List members in current room\n";
        help += "  /msg <user> <message> - Send private message\n";
        help += "  /stats - Show server statistics\n";
        help += "  /quit - Disconnect from server\n";
        
        if (client->is_admin) {
            help += Colors::RED + "  Admin Commands:\n" + Colors::RESET;
            help += "  /kick <user> - Kick a user\n";
        }
        
        client->send_message(help);
    }
    
		
		void disconnect_client(int client_fd) {
        std::shared_ptr<ClientSession> client;
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            auto it = clients.find(client_fd);
            if (it != clients.end()) {
                client = it->second;
                clients.erase(it);
            }
        }
        
        if (client) {
            // Remove from current room
            if (!client->current_room.empty()) {
                std::lock_guard<std::mutex> lock(rooms_mutex);
                auto room_it = rooms.find(client->current_room);
                if (room_it != rooms.end()) {
                    room_it->second->remove_member(client);
                }
            }
            
            logger.info("🔌 Client disconnected: " + client->username + " (" + client->ip_address + ")");
            active_connections--;
        }
        
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
        close(client_fd);
    }
    
    void log_statistics() {
        std::lock_guard<std::mutex> lock(rooms_mutex);
        int total_users = 0;
        for (const auto& [name, room] : rooms) {
            total_users += room->get_member_count();
        }
        
        logger.info("📊 Stats - Active: " + std::to_string(active_connections.load()) + 
                   ", Total Messages: " + std::to_string(total_messages.load()) + 
                   ", Total Users in Rooms: " + std::to_string(total_users));
    }
    
    void shutdown() {
        logger.info("🛑 Server shutdown initiated...");
        running = false;
        
        // Close all client connections
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            for (const auto& [fd, client] : clients) {
                client->send_colored_message("⚠️ Server is shutting down. Goodbye!", Colors::YELLOW);
                close(fd);
            }
            clients.clear();
        }
        
        // Close server socket
        if (server_fd >= 0) {
            close(server_fd);
        }
        
        // Close epoll
        if (epoll_fd >= 0) {
            close(epoll_fd);
        }
        
        logger.info("✅ Server shutdown complete");
    }
    
    ~ChatServer() {
        shutdown();
    }
    
    // Public method to stop the server
    void stop() {
        shutdown();
    }
    
    // Get server statistics
    struct ServerStats {
        int total_connections;
        int active_connections;
        int total_messages;
        int total_rooms;
        std::chrono::system_clock::time_point start_time;
    };
    
    ServerStats get_stats() const {
        return {
            total_connections.load(),
            active_connections.load(),
            total_messages.load(),
            static_cast<int>(rooms.size()),
            server_start_time
        };
    }
};

// Signal handler for graceful shutdown
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\n" << Colors::YELLOW << "🛑 Received shutdown signal..." << Colors::RESET << std::endl;
        if (g_server) {
            g_server->stop();
        }
        exit(0);
    }
}

// Enhanced command line argument parsing
struct ServerConfig {
    int port = 8888;
    std::string log_file = "chat_server.log";
    int max_connections = 100;
    bool verbose = false;
    
    void parse_args(int argc, char* argv[]) {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            
            if (arg == "-p" || arg == "--port") {
                if (i + 1 < argc) {
                    port = std::stoi(argv[++i]);
                }
            } else if (arg == "-l" || arg == "--log") {
                if (i + 1 < argc) {
                    log_file = argv[++i];
                }
            } else if (arg == "-m" || arg == "--max-connections") {
                if (i + 1 < argc) {
                    max_connections = std::stoi(argv[++i]);
                }
            } else if (arg == "-v" || arg == "--verbose") {
                verbose = true;
            } else if (arg == "-h" || arg == "--help") {
                print_usage();
                exit(0);
            }
        }
    }
    
    void print_usage() {
        std::cout << "Advanced Chat Server Usage:\n";
        std::cout << "  -p, --port <port>           Set server port (default: 8888)\n";
        std::cout << "  -l, --log <file>            Set log file path (default: chat_server.log)\n";
        std::cout << "  -m, --max-connections <n>   Set max connections (default: 100)\n";
        std::cout << "  -v, --verbose               Enable verbose logging\n";
        std::cout << "  -h, --help                  Show this help message\n";
    }
};

// Enhanced logging with log levels
class EnhancedLogger {
private:
    std::mutex log_mutex;
    std::ofstream log_file;
    bool verbose_mode;
    
public:
    EnhancedLogger(const std::string& filename, bool verbose = false) 
        : log_file(filename, std::ios::app), verbose_mode(verbose) {}
    
    void log(const std::string& level, const std::string& message, bool force_display = false) {
        std::lock_guard<std::mutex> lock(log_mutex);
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        
        std::ostringstream oss;
        oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
        std::string timestamp = oss.str();
        
        std::string log_entry = "[" + timestamp + "] [" + level + "] " + message;
        
        if (verbose_mode || force_display || level == "ERROR") {
            if (level == "ERROR") {
                std::cout << Colors::RED << log_entry << Colors::RESET << std::endl;
            } else if (level == "WARN") {
                std::cout << Colors::YELLOW << log_entry << Colors::RESET << std::endl;
            } else if (level == "INFO") {
                std::cout << Colors::GREEN << log_entry << Colors::RESET << std::endl;
            } else {
                std::cout << log_entry << std::endl;
            }
        }
        
        log_file << log_entry << std::endl;
        log_file.flush();
    }
    
    void info(const std::string& msg) { log("INFO", msg); }
    void warn(const std::string& msg) { log("WARN", msg); }
    void error(const std::string& msg) { log("ERROR", msg, true); }
    void debug(const std::string& msg) { if (verbose_mode) log("DEBUG", msg); }
    
    ~EnhancedLogger() {
        if (log_file.is_open()) {
            log_file.close();
        }
    }
};

// Performance monitoring class
class PerformanceMonitor {
private:
    std::chrono::high_resolution_clock::time_point start_time;
    std::atomic<long long> total_bytes_sent{0};
    std::atomic<long long> total_bytes_received{0};
    std::atomic<int> peak_connections{0};
    
public:
    PerformanceMonitor() : start_time(std::chrono::high_resolution_clock::now()) {}
    
    void record_bytes_sent(size_t bytes) {
        total_bytes_sent += bytes;
    }
    
    void record_bytes_received(size_t bytes) {
        total_bytes_received += bytes;
    }
    
    void update_peak_connections(int current_connections) {
        int current_peak = peak_connections.load();
        while (current_connections > current_peak && 
               !peak_connections.compare_exchange_weak(current_peak, current_connections)) {
            // CAS loop
        }
    }
    
    std::string get_performance_report() {
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time);
        
        std::ostringstream oss;
        oss << "📈 Performance Report:\n";
        oss << "  Runtime: " << duration.count() << " seconds\n";
        oss << "  Total bytes sent: " << total_bytes_sent.load() << "\n";
        oss << "  Total bytes received: " << total_bytes_received.load() << "\n";
        oss << "  Peak connections: " << peak_connections.load() << "\n";
        
        if (duration.count() > 0) {
            oss << "  Avg bytes/sec sent: " << total_bytes_sent.load() / duration.count() << "\n";
            oss << "  Avg bytes/sec received: " << total_bytes_received.load() / duration.count() << "\n";
        }
        
        return oss.str();
    }
};

// Main function with enhanced error handling and configuration
int main(int argc, char* argv[]) {
    try {
        // Parse command line arguments
        ServerConfig config;
        config.parse_args(argc, argv);
        
        // Setup signal handlers
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
        signal(SIGPIPE, SIG_IGN); // Ignore broken pipe signals
        
        // Create and start server
        std::cout << Colors::CYAN << "🚀 Starting Advanced Chat Server..." << Colors::RESET << std::endl;
        std::cout << Colors::BLUE << "Configuration:" << Colors::RESET << std::endl;
        std::cout << "  Port: " << config.port << std::endl;
        std::cout << "  Log file: " << config.log_file << std::endl;
        std::cout << "  Max connections: " << config.max_connections << std::endl;
        std::cout << "  Verbose: " << (config.verbose ? "Yes" : "No") << std::endl;
        
        ChatServer server(config.port);
        g_server = &server;
        
        // Performance monitoring
        PerformanceMonitor monitor;
        
        // Run server
        server.run();
        
    } catch (const std::exception& e) {
        std::cerr << Colors::RED << "❌ Server error: " << e.what() << Colors::RESET << std::endl;
        return 1;
    } catch (...) {
        std::cerr << Colors::RED << "❌ Unknown server error occurred" << Colors::RESET << std::endl;
        return 1;
    }
    
    return 0;
}

// Additional utility functions for server administration
namespace ServerUtils {
    // Function to check if port is available
    bool is_port_available(int port) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return false;
        
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        
        int result = bind(sock, (sockaddr*)&addr, sizeof(addr));
        close(sock);
        
        return result == 0;
    }
    
    // Function to get system resource limits
    void print_system_limits() {
        std::cout << Colors::CYAN << "📊 System Resource Limits:" << Colors::RESET << std::endl;
        
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
            std::cout << "  Max open files: " << rl.rlim_cur << " (soft), " << rl.rlim_max << " (hard)" << std::endl;
        }
        
        if (getrlimit(RLIMIT_NPROC, &rl) == 0) {
            std::cout << "  Max processes: " << rl.rlim_cur << " (soft), " << rl.rlim_max << " (hard)" << std::endl;
        }
    }
    
    // Function to daemonize the server (run in background)
    void daemonize() {
        pid_t pid = fork();
        
        if (pid < 0) {
            throw std::runtime_error("Fork failed");
        }
        
        if (pid > 0) {
            exit(0); // Parent process exits
        }
        
        // Child process continues
        setsid(); // Create new session
        
        // Change working directory to root
        chdir("/");
        
        // Close standard file descriptors
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        
        // Redirect standard file descriptors to /dev/null
        open("/dev/null", O_RDONLY); // stdin
        open("/dev/null", O_WRONLY); // stdout
        open("/dev/null", O_WRONLY); // stderr
    }
}