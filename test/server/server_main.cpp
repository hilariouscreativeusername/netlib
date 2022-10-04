#include <netlib/netlib.hpp>

enum class ChatMessageType : uint32_t {
  kMessageAll,
};

class ChatServer : public NetLib::Server<ChatMessageType> {
public:
  ChatServer() : NetLib::Server<ChatMessageType>(12345) { }

	virtual bool OnClientConnect(std::shared_ptr<NetLib::Connection<ChatMessageType>> client) {
		return true;
	}

	virtual void OnClientDisconnect(std::shared_ptr<NetLib::Connection<ChatMessageType>> client) {
	}

	virtual void OnMessage(std::shared_ptr<NetLib::Connection<ChatMessageType>> client, NetLib::Message<ChatMessageType>& msg) {
		switch (msg.header.type) {
			case ChatMessageType::kMessageAll: {
				std::cout << "MessageAll\n";
				MessageAllClients(msg, client);
				break;
      }
		}
	}
};

int main() {
  ChatServer server;
	server.Start();

  while (true) {
    server.Update(-1, true);
  }
}
