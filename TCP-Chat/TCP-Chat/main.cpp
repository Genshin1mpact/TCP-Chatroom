#include <iostream>
#include <string>
#include <WinSock2.h>
#pragma comment(lib, "ws2_32.lib")

#include <algorithm>
#include <thread>
#include <atomic>
#include <system_error>
#include <mutex>
#include <vector>
#include <limits>

std::atomic<bool> running{ true };
std::mutex g_clients_mtx;
std::vector<SOCKET> g_clients;
std::mutex g_broadcast_mtx;

bool send_all(SOCKET s, const std::string& message) {
	size_t sent_total = 0;
	while (sent_total < message.size()) {
		const size_t remaining = message.size() - sent_total;
		const int chunk_size = static_cast<int>(
			(std::min)(remaining, static_cast<size_t>((std::numeric_limits<int>::max)())));
		const int sent = send(s, message.data() + sent_total, chunk_size, 0);
		if (sent == SOCKET_ERROR || sent == 0) {
			return false;
		}
		sent_total += static_cast<size_t>(sent);
	}
	return true;
}

void close_client(SOCKET s) {
	// Serialize closes with broadcasts so a socket cannot be closed mid-send.
	std::lock_guard<std::mutex> broadcast_lock(g_broadcast_mtx);
	{
		std::lock_guard<std::mutex> clients_lock(g_clients_mtx);
		g_clients.erase(std::remove(g_clients.begin(), g_clients.end(), s), g_clients.end());
	}
	shutdown(s, SD_BOTH);
	closesocket(s);
}

void broadcast(std::string message, SOCKET source = INVALID_SOCKET) {
	while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
		message.pop_back();
	}
	if (message.empty()) {
		return;
	}
	const std::string frame = message + "\n";

	std::lock_guard<std::mutex> broadcast_lock(g_broadcast_mtx);
	if (source != INVALID_SOCKET) {
		std::cout << "[broadcast from " << source << "] " << message << std::endl;
	}

	std::vector<SOCKET> snapshot;
	{
		std::lock_guard<std::mutex> clients_lock(g_clients_mtx);
		snapshot = g_clients;
	}
	if (snapshot.empty()) return;

	std::vector<SOCKET> dead;
	for (auto s : snapshot) {
		if (!send_all(s, frame)) {
			std::cerr << "broadcast to client " << s
				<< " failed: " << WSAGetLastError() << std::endl;
			dead.push_back(s);
		}
	}

	if (!dead.empty()) {
		std::lock_guard<std::mutex> clients_lock(g_clients_mtx);
		for (auto s : dead) {
			g_clients.erase(std::remove(g_clients.begin(), g_clients.end(), s), g_clients.end());
			shutdown(s, SD_BOTH);
		}
	}
}

void handle_client(SOCKET client_sock) {
	{
		std::lock_guard<std::mutex> lk(g_clients_mtx);
		g_clients.push_back(client_sock);
	}
	std::cerr << "[thread " << std::this_thread::get_id() << "] Client connected." << std::endl;
	broadcast("[server] client " + std::to_string(client_sock) + " joined.");

	std::string pending;
	try {
		std::string buffer;
		while (running) {
			buffer.resize(1024);
			int ret = recv(client_sock, &buffer[0], static_cast<int>(buffer.size()), 0);
			if (ret == SOCKET_ERROR || ret == 0) {
				std::cerr << "[thread " << std::this_thread::get_id() << "] Connection closed by client." << std::endl;
				break;
			}
			buffer.resize(ret);
			pending.append(buffer.data(), static_cast<size_t>(ret));

			size_t pos;
			while ((pos = pending.find('\n')) != std::string::npos) {
				std::string line = pending.substr(0, pos);
				pending.erase(0, pos + 1);
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				if (line.empty()) {
					continue;
				}
				broadcast(line, client_sock);
			}
			if (pending.size() > 64 * 1024) {
				std::cerr << "client " << client_sock << " message too long." << std::endl;
				pending.clear();
			}
		}
	}
	catch (const std::system_error& e) {
		std::cerr << "[thread " << std::this_thread::get_id() << "] Exception: " << e.what() << std::endl;
	}
	catch (...) {
		std::cerr << "[thread " << std::this_thread::get_id() << "] Unknown exception occurred." << std::endl;
	}

	close_client(client_sock);
	std::cerr << "[thread " << std::this_thread::get_id() << "] Client disconnected." << std::endl;
	broadcast("[server] client " + std::to_string(client_sock) + " left.");
}

int main() {
	// Initialize Winsock.
	WSADATA wsa_data{};
	const int startup_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
	if (startup_result != 0) {
		std::cerr << "WSAStartup() failed: " << startup_result << std::endl;
		return -1;
	}
	// Create the listening socket.
	SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listen_sock == INVALID_SOCKET) {
		std::cerr << "socket() failed: " << WSAGetLastError() << std::endl;
		WSACleanup();
		return -1;
	}
	std::cerr << "socket() success: " << listen_sock << std::endl;

	// Bind to port 8080 on every local network interface.
	struct sockaddr_in server_addr = { 0 };
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(8080);
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(listen_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
		std::cerr << "bind() failed: " << WSAGetLastError() << std::endl;
		closesocket(listen_sock);
		WSACleanup();
		return -1;
	}
	std::cerr << "bind() success" << std::endl;

	// Start accepting incoming connections.
	if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
		std::cerr << "listen() failed: " << WSAGetLastError() << std::endl;
		closesocket(listen_sock);
		WSACleanup();
		return -1;
	}

	while (running) {
		SOCKET client_sock = accept(listen_sock, nullptr, nullptr);
		if (client_sock == INVALID_SOCKET) {
			int err = WSAGetLastError();
			if (err == WSAEINTR || err == WSAENOTSOCK || !running) {
				std::cerr << "accept() interrupted by signal, exiting." << std::endl;
				break;
			}
			std::cerr << "accept() failed: " << err << std::endl;
			continue;
		}
		std::cerr << "accept() success: " << client_sock << std::endl;
		try {
			std::thread(handle_client, client_sock).detach();
		}
		catch (const std::system_error& e) {
			std::cerr << "Failed to create thread: " << e.what() << std::endl;
			closesocket(client_sock);
		}
		catch (...) {
			std::cerr << "Unknown error occurred while creating thread." << std::endl;
			closesocket(client_sock);
		}
	}
	running = false;
	closesocket(listen_sock);
	WSACleanup();
	return 0;
}
