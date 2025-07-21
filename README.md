# Multithreaded C++ Chat Server

A high-performance, feature-rich multithreaded chat server implemented in modern C++ using epoll, sockets, threads, and synchronization primitives. Supports multiple rooms, user authentication, colored output, and administrative commands.

---

## 🚀 Features

* ✅ Multithreaded server using std::thread
* ✅ Epoll-based I/O multiplexing for efficient socket management
* ✅ Room-based chat (join, create, leave rooms)
* ✅ User registration and login with basic auth
* ✅ Admin controls: kick users, private rooms
* ✅ Colored messages: SYSTEM, PRIVATE, USER
* ✅ Thread-safe logging with timestamps
* ✅ Server statistics: uptime, total connections, messages
* ✅ Graceful shutdown (Ctrl+C)

---

## 🧱 Project Structure

```
chat-server/
├── Client.cpp           # Complete chat server logic
├── Server.cpp           # Optional extended version with CLI args and monitoring
├── README.md            # Project documentation
```

---

## 🛠️ Build Instructions

### Prerequisites

* g++ (C++17+)
* Linux (epoll is Linux-specific)

### Build

```bash
g++ Client.cpp -o chat_server -pthread
```

Or if you're using the advanced version:

```bash
g++ Server.cpp -o chat_server -pthread
```

---

## 🧪 Run the Server

```bash
./chat_server [port]
```

Default port: `8888`

Example:

```bash
./chat_server 8888
```

---

## 🖥️ Connect a Client

Use `telnet` or `nc` (netcat):

```bash
telnet 127.0.0.1 8888
```

---

## 💬 Command List

* `/login <user> <pass>` – Log in
* `/register <user> <pass>` – Register
* `/join <room> [pass]` – Join room
* `/create <room> [pass]` – Create new room
* `/list` – List rooms
* `/who` – See members in current room
* `/msg <user> <msg>` – Private message
* `/kick <user>` – Kick user (admin only)
* `/stats` – Server stats
* `/quit` – Disconnect

---

## 🎨 Message Types

* **SYSTEM:** Yellow
* **PRIVATE:** Magenta
* **USER:** Cyan timestamp + green username

---

## 📂 Logs

* Stored in `chat_server.log`
* Timestamped and thread-safe

---

## 🧩 Advanced CLI Options (Server.cpp only)

```bash
./chat_server -p <port> -l <log_file> -v
```

Flags:

* `-p` / `--port`: Set port
* `-l` / `--log`: Set log file path
* `-v` / `--verbose`: Enable verbose logging

---

## 📊 Example Output

```
[12:01:01] [SYSTEM] User Darelene joined the room
[12:01:02] Darelene: Hello everyone!
[12:01:05] [PRIVATE] Admin: Please check your DM
```
