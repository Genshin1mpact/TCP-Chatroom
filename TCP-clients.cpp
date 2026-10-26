#include <iostream>
#include <WinSock2.h>
#include <string>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <algorithm>
#include <array>
#include <limits>

namespace tool {
	static bool read_line(const std::string& prompt, std::string& out) {
		std::cout << prompt;
		if (!std::getline(std::cin, out)) {
			return false;
		}
		if (!out.empty() && out.back() == '\r') {
			out.pop_back();
		}
		return true;
	}

	static std::string trim(std::string str) {
		auto non_space = [](char c) { return !std::isspace(static_cast<unsigned char>(c)); };
		str.erase(str.begin(), std::find_if(str.begin(), str.end(), non_space));
		str.erase(std::find_if(str.rbegin(), str.rend(), non_space).base(), str.end());
		return str;
	}

	static bool invalid_username(const std::string& username) {
		if (username.empty() || username.size() > 32) {
			return false;
		}
		for (auto c : username) {
			if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
				return false;
			}
		}
		return true;
	}

	static bool parse_port(const std::string& s, uint16_t& port) {
		try {
			int p = std::stoi(s);
			if (p < 1 || p > 65535) {
				return false;
			}
			port = static_cast<uint16_t>(p);
			return true;
		}
		catch (...) {
			return false;
		}
	}

	static bool send_all(SOCKET socket, const std::string& message) {
		size_t sent_total = 0;
		while (sent_total < message.size()) {
			const size_t remaining = message.size() - sent_total;
			const int chunk_size = static_cast<int>(
				(std::min)(remaining, static_cast<size_t>((std::numeric_limits<int>::max)())));
			const int sent = send(socket, message.data() + sent_total, chunk_size, 0);
			if (sent == SOCKET_ERROR || sent == 0) {
				return false;
			}
			sent_total += static_cast<size_t>(sent);
		}
		return true;
	}
}

#include <mutex>
#include <thread>
#include <atomic>

std::atomic<bool> client_running{ true };
std::mutex cout_mtx;

void print_message_prompt() {
	std::lock_guard<std::mutex> lock(cout_mtx);
	std::cout << "Enter message to send (or 'exit' to quit): " << std::flush;
}

void recv_loop(SOCKET str) {
	std::string pending;
	std::array<char, 4096> buffer{};
	while (client_running) {
		const int ret = recv(str, buffer.data(), static_cast<int>(buffer.size()), 0);
		if (ret == SOCKET_ERROR || ret == 0) {
			break;
		}
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

			std::lock_guard<std::mutex> lock(cout_mtx);
			std::cout << "\n[broadcast] " << line << "\n> " << std::flush;
		}
		if (pending.size() > 64 * 1024) {
			std::lock_guard<std::mutex> lock(cout_mtx);
			std::cerr << "\nReceived message exceeds 64 KiB; closing connection." << std::endl;
			client_running = false;
			break;
		}
	}
	client_running = false;
}

int main() {
	WSADATA wsa_data{};
	const int startup_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
	if (startup_result != 0) {
		std::cerr << "WSAStartup() failed: " << startup_result << std::endl;
		return -1;
	}

	// Read and validate the username.
	std::string username;
	while (true) {
		if (!tool::read_line("Username: ", username)) {
			WSACleanup();
			return 1;
		}
		username = tool::trim(username);
		if (tool::invalid_username(username)) break;
		std::cout << "Invalid username. (1-32 Bytes, no control characters)." << std::endl;
	}

	// Read the server address and port.
	struct sockaddr_in target {};
	target.sin_family = AF_INET;
	std::string host;
	while (true) {
		if (!tool::read_line("Serve IPV4 Address: ", host)) {
			WSACleanup();
			return 1;
		}
		host = tool::trim(host);
		if (inet_pton(AF_INET, host.c_str(), &target.sin_addr) == 1) break;
		std::cout << "Invalid IP Address" << std::endl;
	}
	
	std::string port_str;
	uint16_t port = 0;
	while (true) {
		if (!tool::read_line("Serve Port (1-65535):", port_str)) {
			WSACleanup();
			return 1;
		}
		port_str = tool::trim(port_str);
		if (tool::parse_port(port_str, port)) break;
		std::cout << "Invalid port" << std::endl;
	}
	target.sin_port = htons(port);

	SOCKET client_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (client_sock == INVALID_SOCKET) {
		std::cerr << "Failed to create socket." << std::endl;
		WSACleanup();
		return -1;
	}

	std::cout << "Connecting to " << host << ":" << port << std::endl;
	if (connect(client_sock, (struct sockaddr*)&target, sizeof(target)) == SOCKET_ERROR) {
		std::cerr << "Failed to connect to server." << std::endl;
		closesocket(client_sock);
		WSACleanup();
		return -1;
	}
	std::cout << username << " connected." << std::endl;
	std::thread recv_thread(recv_loop, client_sock);

	while (client_running) {
		std::string message;
		print_message_prompt();
		if (!std::getline(std::cin, message) || message == "exit") {
			break;
		}

		std::string framed = "[" + username + "]" + message + "\n";
		if (!tool::send_all(client_sock, framed)) {
			std::cerr << "Failed to send message: " << WSAGetLastError() << std::endl;
			break;
		}
	}
	client_running = false;
	shutdown(client_sock, SD_BOTH);
	if (recv_thread.joinable()) {
		recv_thread.join();
	}
	closesocket(client_sock);
	WSACleanup();
	return 0;
}
