#include <chrono>
#include <thread>

#include <netlib/netlib.hpp>

enum class ChatMessageType : uint32_t {
	kMessage,
};

class ChatClient : public NetLib::Client<ChatMessageType> {
};

int main() {
	ChatClient client;
	std::cout << "Enter Server IP: \n";
	std::string ip;
	std::getline(std::cin, ip);
	client.Connect(ip, 12345);

	bool is_connected = false;
	for (int i = 0; i < 5000 / 20; ++i) {
		if (client.IsConnected()) {
			std::cout << "Connected!\n";
			is_connected = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	std::mutex stdout_mutex;

	if (!is_connected) {
		std::cout << "Failed to connect :(\n";
		std::cin.get();
		return -1;
	}
	else {
		std::thread in_msg_thread([&](){
			while (true) {
				while (client.Incoming().empty()) {}
				std::scoped_lock lock(stdout_mutex);
				auto msg = client.Incoming().pop_front().msg;
				std::string s;
				while (msg.header.body_size > 0) {
					char c;
				  msg >> c;
					s += c;
				}
				std::cout << "INCOMING MESSAGE!: " << s << "\n";
			}
		});

		while (true) {
			std::cout << "Enter a message to send to all other clients:\n";
			std::string next_message;
			std::getline(std::cin, next_message);

			std::scoped_lock lock(stdout_mutex);
			NetLib::Message<ChatMessageType> msg;
			for (std::string::reverse_iterator itr = next_message.rbegin(); itr != next_message.rend(); ++itr) {
				char c = *itr;
		    msg << c;
			}
			client.Send(msg);
		}
	}

}
