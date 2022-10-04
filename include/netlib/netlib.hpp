#pragma once 

#include <memory>
#include <thread>
#include <mutex>
#include <deque>
#include <optional>
#include <vector>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <cstdint>

/* Make compiler warnings go away on Windows
 */
#ifdef _WIN32
	#define _WINSOCK_DEPRECATED_NO_WARNINGS
	#ifndef _WIN32_WINNT
		#define _WIN32_WINNT 0x0A00
	#endif
#endif

/* We are not using asio as part of boost
 */
#define ASIO_STANDALONE
#include <asio.hpp>
#include <asio/ts/buffer.hpp>
#include <asio/ts/internet.hpp>

namespace NetLib {

/* A Message consists of a header of a fixed size and a body
   The template type is intended to be an enum class that can be accessed by both the server
   and client applications, though it could be any integral type
 */
template <typename T> struct MessageHeader {
	T type { };
	uint32_t body_size = 0;
};

/* The first several bytes of an incoming message will be a header. The
   header can be read to determine the size of the message body. After that,
	 the next several bytes contain the body of the message.
	 The message body is stored as a vector of raw bytes
 */
template <typename T> struct Message {
public:
	MessageHeader<T> header { };
	std::vector<uint8_t> body;

public:
/* Obligatory std::cout helper
 */
	friend std::ostream& operator << (std::ostream& os, const Message<T>& msg) {
		os << "Message [Type: " << int(msg.header.type) << " Body Size: " << msg.header.body_size << "]";
		return os;
	}

/* Serialise POD into message body vector
 */
	template<typename DataType> friend Message<T>& operator << (Message<T>& msg, const DataType& data) {
		// Check that the data is trivially copyable
		static_assert(std::is_standard_layout<DataType>::value, "Data must be POD-like to be serialised");

		// Cache current size of vector, as this will be the point we insert the data
		size_t i = msg.body.size();

		// Resize the body vector to make space for the new data
		msg.body.resize(msg.body.size() + sizeof(DataType));

		// Copy the data into the body vector
		std::memcpy(msg.body.data() + i, &data, sizeof(DataType));

		// Recalculate the message size
		msg.header.body_size = (uint32_t)msg.body.size();

		// Return the message so that << operators can be chained together
		return msg;
	}

/* Deserialise POD from message body vector
 */
	template<typename DataType> friend Message<T>& operator >> (Message<T>& msg, DataType& data) {
		// Check that the data is trivially copyable
		static_assert(std::is_standard_layout<DataType>::value, "Data must be POD-like to be deserialised");

		// Cache the location towards the end of the vector where the pulled data starts
		size_t i = msg.body.size() - sizeof(DataType);

		// Physically copy the data from the vector into the user variable
		std::memcpy(&data, msg.body.data() + i, sizeof(DataType));

		// Shrink the vector to remove read bytes, and reset end position
		msg.body.resize(i);

		// Recalculate the message size
		msg.header.body_size = (uint32_t)msg.body.size();

		// Return the message so that >> operators can be chained together
		return msg;
	}
};

template <typename T> class Connection;

/* An OwnedMessage is a Message that is associated with the connection that
	 the message was received from
 */
template <typename T> struct OwnedMessage {
	std::shared_ptr<Connection<T>> remote = nullptr;
	Message<T> msg;

/* Obligatory ostream operator
 */
	friend std::ostream& operator<<(std::ostream& os, const OwnedMessage<T>& msg) {
		os << msg.msg;
		return os;
	}
};

/* Wrapper around a std::deque that uses a std::mutex to guard against concurrent access
 */
template<typename T> class ThreadSafeQueue {
public:
	ThreadSafeQueue() = default;
	ThreadSafeQueue(const ThreadSafeQueue<T>&) = delete;
	virtual ~ThreadSafeQueue() { clear(); }

public:
/* Returns item at front of queue without removing it from the queue
 */
	const T& front() {
		std::scoped_lock lock(queue_mutex_);
		return queue_.front();
	}

/* Returns item at back of queue without removing it from the queue
 */
	const T& back() {
		std::scoped_lock lock(queue_mutex_);
		return queue_.back();
	}

/* Removes item from front of queueand returns it
 */
	T pop_front() {
		std::scoped_lock lock(queue_mutex_);
		auto t = std::move(queue_.front());
		queue_.pop_front();
		return t;
	}

/* Removes item from back of queueand returns it
 */
	T pop_back() {
		std::scoped_lock lock(queue_mutex_);
		auto t = std::move(queue_.back());
		queue_.pop_back();
		return t;
	}

/* Adds an item to back of queue
 */
	void push_back(const T& item) {
		std::scoped_lock lock(queue_mutex_);
		queue_.emplace_back(std::move(item));

		std::unique_lock<std::mutex> ul(blocking_mutex_);
		blocking_condition_.notify_one();
	}

/* Adds an item to front of queue
 */
	void push_front(const T& item) {
		std::scoped_lock lock(queue_mutex_);
		queue_.emplace_front(std::move(item));

		std::unique_lock<std::mutex> ul(blocking_mutex_);
		blocking_condition_.notify_one();
	}

/* Returns true if queue has no items
 */
	bool empty() {
		std::scoped_lock lock(queue_mutex_);
		return queue_.empty();
	}

/* Returns number of items in queue
 */
	size_t count() {
		std::scoped_lock lock(queue_mutex_);
		return queue_.size();
	}

/* Clears all items from queue
 */
	void clear() {
		std::scoped_lock lock(queue_mutex_);
		queue_.clear();
	}

/* Wait for an item to be added to the queue using *magic semphores*
 */
	void wait() {
		while (empty()) {
			std::unique_lock<std::mutex> ul(blocking_mutex_);
			blocking_condition_.wait(ul);
		}
	}

protected:
	std::mutex queue_mutex_;
	std::deque<T> queue_;
	std::condition_variable blocking_condition_;
	std::mutex blocking_mutex_;
};

template<typename T> class Server;

template<typename T> class Connection : public std::enable_shared_from_this<Connection<T>> {
public:
/* A connection can be owned by either a client or a server
 */
	enum class Owner {
		kServer,
		kClient,
	};

public:
/* When a connection is created, we perform the initial handshake
 */
	Connection(Owner parent, asio::io_context& context, asio::ip::tcp::socket socket, ThreadSafeQueue<OwnedMessage<T>>& in_queue)
      : context_(context), socket_(std::move(socket)), in_queue_(in_queue) {
		owner_ = parent;

		// Construct validation check data
		if (owner_ == Owner::kServer) {
			// Connection is Server -> Client, construct random data for the client
			// to transform and send back for validation
			handshake_out_ = uint64_t(std::chrono::system_clock::now().time_since_epoch().count());

			// Precalculate the result so that it can be compared with the client response
			handshake_check_ = Scramble(handshake_out_);
		}
		else {
			// Connection is client to server, so there is nothing to do
			handshake_in_ = 0;
			handshake_out_ = 0;
		}
	}

	virtual ~Connection() { }

/* System-wide ID used to identify clients across a network without sharing IPs or similar
 */
	uint32_t GetId() const {
		return id_;
	}

public:
	void ConnectToClient(Server<T>* server, uint32_t uid = 0) {
		if (owner_ == Owner::kServer) {
			if (socket_.is_open()) {
				id_ = uid;

				// A client has attempted to connect to the server, but we wish
				// the client to first validate itself, so first write out the
				// handshake data to be validated
				WriteValidation();

				// Next, issue a task to sit and wait asynchronously for precisely
				// the validation data sent back from the client
				ReadValidation(server);
			}
		}
	}

	void ConnectToServer(const asio::ip::tcp::resolver::results_type& endpoints) {
		// Only clients can connect to servers
		if (owner_ == Owner::kClient) {
			// Request asio attempts to connect to an endpoint
			asio::async_connect(socket_, endpoints, [this](std::error_code ec, asio::ip::tcp::endpoint endpoint) {
				if (!ec) {
					// First thing server will do is send packet to be validated, so wait for that and respond
					ReadValidation();
				}
			});
		}
	}

	void Disconnect() {
		if (IsConnected()) {
			asio::post(context_, [this]() {
				socket_.close();
			});
		}
	}

	bool IsConnected() const {
		return socket_.is_open();
	}

public:
	// ASYNC - Send a message, connections are one-to-one so no need to specifiy
	// the target, for a client, the target is the server and vice versa
	void Send(const Message<T>& msg) {
		asio::post(context_, [this, msg]() {
			// If the queue has a message in it, then we must 
			// assume that it is in the process of asynchronously being written.
			// Either way add the message to the queue to be output. If no messages
			// were available to be written, then start the process of writing the
			// message at the front of the queue.
			bool is_writing_message = !out_queue_.empty();
			out_queue_.push_back(msg);
			if (!is_writing_message) {
				WriteHeader();
			}
		});
	}

private:
	void WriteHeader() {
   /* Send at least one message by allocating a buffer and issuing the work to asio
    */
		asio::async_write(socket_, asio::buffer(&out_queue_.front().header, sizeof(MessageHeader<T>)), [this](std::error_code ec, std::size_t length) {
			// asio has now sent the bytes - if there was a problem an error would have been generated
			if (!ec) {
				// ... no error, so check if the message header just sent also has a message body...
				if (out_queue_.front().body.size() > 0) {
					// ...it does, so issue the task to write the body bytes
					WriteBody();
				}
				else {
					// ...it didnt, so we are done with this message. Remove it from 
					// the outgoing message queue
					out_queue_.pop_front();

					// If the queue is not empty, there are more messages to send, so
					// make this happen by issuing the task to send the next header.
					if (!out_queue_.empty()) {
						WriteHeader();
					}
				}
			}
			else
			{
				// ...asio failed to write the message, we could analyse why but 
				// for now simply assume the connection has died by closing the
				// socket. When a future attempt to write to this client fails due
				// to the closed socket, it will be tidied up.
				std::cout << "[" << id_ << "] Write Header Fail.\n";
				socket_.close();
			}
		});
	}

	// ASYNC - Prime context to write a message body
	void WriteBody() {
		// If this function is called, a header has just been sent, and that header
		// indicated a body existed for this message. Fill a transmission buffer
		// with the body data, and send it!
		asio::async_write(socket_, asio::buffer(out_queue_.front().body.data(), out_queue_.front().body.size()), [this](std::error_code ec, std::size_t length) {
			if (!ec) {
				// Sending was successful, so we are done with the message. Remove it from the queue
				out_queue_.pop_front();

				// If the queue still has messages in it, then issue the task to 
				// send the next messages' header.
				if (!out_queue_.empty()) {
					WriteHeader();
				}
			}
			else {
				// Sending failed, see WriteHeader() equivalent for description :P
				std::cout << "[" << id_ << "] Write Body Fail.\n";
				socket_.close();
			}
		});
	}

	// ASYNC - Prime context ready to read a message header
	void ReadHeader() {
		// If this function is called, we are expecting asio to wait until it receives
		// enough bytes to form a header of a message. We know the headers are a fixed
		// size, so allocate a transmission buffer large enough to store it. In fact, 
		// we will construct the message in a "temporary" message object as it's 
		// convenient to work with.
		asio::async_read(socket_, asio::buffer(&msg_temp_in_.header, sizeof(MessageHeader<T>)), [this](std::error_code ec, std::size_t length) {
			if (!ec) {
				// A complete message header has been read, check if this message
				// has a body to follow...
				if (msg_temp_in_.header.body_size > 0) {
					// ...it does, so allocate enough space in the messages' body
					// vector, and issue asio with the task to read the body.
					msg_temp_in_.body.resize(msg_temp_in_.header.body_size);
					ReadBody();
				}
				else {
					// it doesn't, so add this bodyless message to the connections
					// incoming message queue
					AddToIncomingMessageQueue();
				}
			}
			else {
				// Reading form the client went wrong, most likely a disconnect
				// has occurred. Close the socket and let the system tidy it up later.
				std::cout << "[" << id_ << "] Read Header Fail.\n";
				socket_.close();
			}
		});
	}

	// ASYNC - Prime context ready to read a message body
	void ReadBody() {
		// If this function is called, a header has already been read, and that header
		// request we read a body, The space for that body has already been allocated
		// in the temporary message object, so just wait for the bytes to arrive...
		asio::async_read(socket_, asio::buffer(msg_temp_in_.body.data(), msg_temp_in_.body.size()), [this](std::error_code ec, std::size_t length) {
			if (!ec) {
				// ...and they have! The message is now complete, so add
				// the whole message to incoming queue
				AddToIncomingMessageQueue();
			}
			else {
				std::cout << "[" << id_ << "] Read Body Fail.\n";
				socket_.close();
			}
		});
	}

/* Fake encryption
   TODO: This should be virtual
 */
	uint64_t Scramble(uint64_t nInput) {
		uint64_t out = nInput ^ 0xDEADBEEFC0DECAFE;
		out = (out & 0xF0F0F0F0F0F0F0) >> 4 | (out & 0x0F0F0F0F0F0F0F) << 4;
		return out ^ 0xC0DEFACE12345678;
	}

	// ASYNC - Used by both client and server to write validation packet
	void WriteValidation() {
		asio::async_write(socket_, asio::buffer(&handshake_out_, sizeof(uint64_t)), [this](std::error_code ec, std::size_t length) {
			if (!ec) {
				// Validation data sent, clients should sit and wait
				// for a response (or a closure)
				if (owner_ == Owner::kClient) {
					ReadHeader();
				}
			}
			else {
				socket_.close();
			}
		});
	}

	void ReadValidation(Server<T>* server = nullptr) {
		asio::async_read(socket_, asio::buffer(&handshake_in_, sizeof(uint64_t)), [this, server](std::error_code ec, std::size_t length) {
			if (!ec) {
				if (owner_ == Owner::kServer) {
					// Connection is a server, so check response from client

					// Compare sent data to actual solution
					if (handshake_in_ == handshake_check_) {
						// Client has provided valid solution, so allow it to connect properly
						std::cout << "Client Validated" << std::endl;
						server->OnClientValidated(this->shared_from_this());

						// Sit waiting to receive data now
						ReadHeader();
					}
					else {
						// Client gave incorrect data, so disconnect
						std::cout << "Client Disconnected (Fail Validation)" << std::endl;
						socket_.close();
					}
				}
				else {
					// Connection is a client, so solve puzzle
					handshake_out_ = Scramble(handshake_in_);

					// Write the result
					WriteValidation();
				}
			}
			else {
				// Some bigger failure occured
				std::cout << "Client Disconnected (ReadValidation)" << std::endl;
				socket_.close();
			}
		});
	}

/* Once a full message is received, add it to the incoming queue
 */
	void AddToIncomingMessageQueue() {
		// Make the message "owned" by adding a pointer to the current object 
		if (owner_ == Owner::kServer) {
			in_queue_.push_back({ this->shared_from_this(), msg_temp_in_ });
		}
		else {
			in_queue_.push_back({ nullptr, msg_temp_in_ });
		}

    // Prime the asio context to receive the next message
		ReadHeader();
	}

protected:
	asio::ip::tcp::socket socket_;

	// This context is shared with the whole asio instance
	asio::io_context& context_;

	ThreadSafeQueue<Message<T>> out_queue_;

	// This queue will be shared if the connection is owned by a server
	ThreadSafeQueue<OwnedMessage<T>>& in_queue_;

	// Incoming messages are constructed asynchronously, so we will store the partly 
  // assembled message here until it is ready to be sent
	Message<T> msg_temp_in_;

	Owner owner_ = Owner::kServer;

	uint64_t handshake_out_ = 0;
	uint64_t handshake_in_ = 0;
	uint64_t handshake_check_ = 0;

	bool is_handshake_valid_ = false;
	bool is_connection_established_ = false;

	uint32_t id_ = 0;

};

template <typename T> class Client {
public:
	Client() = default;

/* *Try* to disconnect cleanly
 */
	virtual ~Client() {
		Disconnect();
	}

public:
/* Connect to server with hostname / ip address and port
 */
	bool Connect(const std::string& host, const uint16_t port) {
		try {
			// Resolve hostname/ip-address into tangiable physical address
			asio::ip::tcp::resolver resolver(context_);
			asio::ip::tcp::resolver::results_type endpoints = resolver.resolve(host, std::to_string(port));

			// Create connection
			connection_ = std::make_unique<Connection<T>>(Connection<T>::Owner::kClient, context_, asio::ip::tcp::socket(context_), in_queue_);

			// Tell the connection object to connect to server
			connection_->ConnectToServer(endpoints);

			// Start Context Thread
			context_thread_ = std::thread([this]() { context_.run(); });
		}
		catch (std::exception& e) {
			std::cerr << "Client Exception: " << e.what() << "\n";
			return false;
		}
		return true;
	}

	// Disconnect from server
	void Disconnect() {
		// If connection exists, and it's connected then...
		if (IsConnected()) {
			// ...disconnect from server gracefully
			connection_->Disconnect();
		}

		// Either way, we're also done with the asio context...				
		context_.stop();
		// ...and its thread
		if (context_thread_.joinable()) {
			context_thread_.join();
		}

		// Destroy the connection object
		connection_.release();
	}

	// Check if client is actually connected to a server
	bool IsConnected() {
		if (connection_) {
			return connection_->IsConnected();
		}
		else {
			return false;
		}
	}

public:
	// Send message to server
	void Send(const Message<T>& msg) {
		if (IsConnected()) {
		  connection_->Send(msg);
		}
	}

	// Retrieve queue of messages from server
	ThreadSafeQueue<OwnedMessage<T>>& Incoming() {
		return in_queue_;
	}

protected:
	// asio context handles the data transfer...
	asio::io_context context_;
	// ...but needs a thread of its own to execute its work commands
	std::thread context_thread_;
	// The client has a single instance of a "connection" object, which handles data transfer
	std::unique_ptr<Connection<T>> connection_;

private:
	// This is the thread safe queue of incoming messages from server
	ThreadSafeQueue<OwnedMessage<T>> in_queue_;
};

template<typename T> class Server {
public:
	Server(uint16_t port) : acceptor_(context_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) { }

/* Attempt a clean disconnect on server destruction
 */
	virtual ~Server() {
		Stop();
	}

	bool Start() {
		try {
			// Prime the context with work so that it doesn't immediately exit
			WaitForClientConnection();

			// Launch the asio context in its own thread
			context_thread_ = std::thread([this]() { context_.run(); });
		}
		catch (std::exception& e) {
			// Something prohibited the server from listening
			std::cerr << "[SERVER] Exception: " << e.what() << "\n";
			return false;
		}

		std::cout << "[SERVER] Started!\n";
		return true;
	}

/* Close the asio context and stop the thread
 */
	void Stop() {
		context_.stop();
		if (context_thread_.joinable()) {
			context_thread_.join();
    }

		std::cout << "[SERVER] Stopped!\n";
	}

	// ASYNC - Instruct asio to wait for connection
	void WaitForClientConnection() {
		// Prime context with an instruction to wait until a socket connects. This
		// is the purpose of an "acceptor" object. It will provide a unique socket
		// for each incoming connection attempt
		acceptor_.async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
			// Triggered by incoming connection request
			if (!ec) {
				// Display some useful(?) information
				std::cout << "[SERVER] New Connection: " << socket.remote_endpoint() << "\n";

				// Create a new connection to handle this client
				std::shared_ptr<Connection<T>> newconn = std::make_shared<Connection<T>>(Connection<T>::Owner::kServer, context_, std::move(socket), in_queue_);

				// Give the user server a chance to deny connection
				if (OnClientConnect(newconn)) {
					// Connection allowed, so add to container of new connections
					connections_.push_back(std::move(newconn));

					// And very important! Issue a task to the connection's asio context to sit and wait for bytes to arrive!
					connections_.back()->ConnectToClient(this, id_counter_++);

					std::cout << "[" << connections_.back()->GetId() << "] Connection Approved\n";
				}
				else {
					std::cout << "[-----] Connection Denied\n";

					// Connection will go out of scope with no pending tasks, so will
					// get destroyed automagically due to the wonder of smart pointers
				}
			}
			else {
				// Error has occurred during acceptance
				std::cout << "[SERVER] New Connection Error: " << ec.message() << "\n";
			}

			// Prime the asio context with more work - again simply wait for
			// another connection...
			WaitForClientConnection();
		});
	}

	// Send a message to a specific client
	void MessageClient(std::shared_ptr<Connection<T>> client, const Message<T>& msg) {
		// Check client is legitimate...
		if (client && client->IsConnected()) {
			// ...and post the message via the connection
			client->Send(msg);
		}
		else {
			// If we cant communicate with client then we may as 
			// well remove the client - let the server know, it may
			// be tracking it somehow
			OnClientDisconnect(client);

			// Off you go now, bye bye!
			client.reset();

			// Then physically remove it from the container
			connections_.erase(std::remove(connections_.begin(), connections_.end(), client), connections_.end());
		}
	}

	// Send message to all clients
	void MessageAllClients(const Message<T>& msg, std::shared_ptr<Connection<T>> client_to_ignore = nullptr) {
		bool bInvalidClientExists = false;

		// Iterate through all clients in container
		for (auto& client : connections_) {
			// Check client is connected...
			if (client && client->IsConnected()) {
				// ..it is!
				if (client != client_to_ignore) {
					client->Send(msg);
				}
			}
			else {
				// The client couldnt be contacted, so assume it has
				// disconnected.
				OnClientDisconnect(client);
				client.reset();

				// Set this flag to then remove dead clients from container
				bInvalidClientExists = true;
			}
		}

		// Remove dead clients, all in one go - this way, we dont invalidate the
		// container as we iterated through it.
		if (bInvalidClientExists) {
			connections_.erase(std::remove(connections_.begin(), connections_.end(), nullptr), connections_.end());
		}
	}

/* Respond to incoming messages
 */
	void Update(size_t max_to_process = -1, bool wait_for_message = false) {
		if (wait_for_message) {
      in_queue_.wait();
    }

		// Process as many messages as you can up to the value specified
		size_t message_count = 0;
		while (message_count < max_to_process && !in_queue_.empty()) {
			// Grab the front message
			auto msg = in_queue_.pop_front();

			// Pass to message handler
			OnMessage(msg.remote, msg.msg);

			++message_count;
		}
	}

protected:
/* These functions should be overriden by the implementation class
 */

	// Called when a client connects, you can veto the connection by returning false
	virtual bool OnClientConnect(std::shared_ptr<Connection<T>> client) { return true; }

	// Called when a client appears to have disconnected
	virtual void OnClientDisconnect(std::shared_ptr<Connection<T>> client) { }

	// Called when a message arrives
	virtual void OnMessage(std::shared_ptr<Connection<T>> client, Message<T>& msg) { }

public:
	// Called when a client is validated
	virtual void OnClientValidated(std::shared_ptr<Connection<T>> client) { }

protected:
	// Thread Safe Queue for incoming message packets
	ThreadSafeQueue<OwnedMessage<T>> in_queue_;

	// Container of active validated connections
	std::deque<std::shared_ptr<Connection<T>>> connections_;

	// Order of declaration is important - it is also the order of initialisation
	asio::io_context context_;
	std::thread context_thread_;

	// These things need an asio context
	asio::ip::tcp::acceptor acceptor_; // Handles new incoming connection attempts...

	// Clients will be identified in the "wider system" via an ID
	uint32_t id_counter_ = 10000;
};

}
