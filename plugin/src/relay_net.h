// The relay client (relay/PROTOCOL.md): WinHTTP WebSockets out to the relay and
// plain TCP on loopback to the game. Windows only, but no D2RLoader calls, so
// tests/relay_e2e.cpp drives it against a real relay with a fake game.
//
// Every blocking call runs on threads of its own, never on the game's UI or
// game thread. TCP_NODELAY everywhere: each recv() goes out as one message at
// once, so the relay adds no delay of its own.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "relay_config.h"

namespace rooms::net {

using LogFn = std::function<void(const std::string&)>;

// WSAStartup once; safe to call again.
bool StartNetworking();

// 16 random bytes as base64url (22 characters).
std::string RandomToken();

// GET <relay>/healthz; `timeoutMs` for each step. Blocking.
RelayHealth CheckRelay(const std::string& url, int timeoutMs = 5000);

// The first relay of the list that answers and takes new rooms (status active).
// Blocking; empty name when none does. `states` gets every relay's state.
RelayServer PickRelay(const std::vector<RelayServer>& relays, std::vector<RelayHealth>* states = nullptr);

class WebSocket;

// The host's side: one socket to the relay; each OPEN from it becomes a TCP
// connection to the game at 127.0.0.1:gamePort.
class HostRelay {
public:
	struct Options {
		RelayServer relay;
		int gamePort = 4000;
		LogFn log;
		std::function<void()> lost;  // the relay socket dropped (called on a relay thread)
	};

	explicit HostRelay(Options options);
	~HostRelay();
	HostRelay(const HostRelay&) = delete;
	HostRelay& operator=(const HostRelay&) = delete;

	// Claims a fresh room. Blocking; false with `error` set ("near_limit",
	// "relay_key", "no answer", ...) when the relay refuses.
	bool Start(std::string* error);
	void Stop();

	// What joiners need, through the board.
	RelayRef Ref() const { return {opt_.relay.url, room_, joinKey_}; }
	int Pipes() const;

private:
	struct Pipe;
	void ReadLoop();
	void PipeThread(std::shared_ptr<Pipe> pipe);
	void Send(const std::string& frame);
	void ClosePipe(uint8_t n, bool tellRelay);

	Options opt_;
	std::string room_, secret_, joinKey_;
	std::unique_ptr<WebSocket> ws_;
	std::thread reader_, pinger_;
	std::atomic<bool> stop_ {false};
	mutable std::mutex mu_;
	std::vector<std::shared_ptr<Pipe>> pipes_;
	std::vector<std::thread> threads_;
};

// A joiner's side: one socket to the relay, a listener on loopback that the
// game joins, and the bytes piped both ways until either side ends.
class JoinRelay {
public:
	struct Options {
		RelayRef relay;
		int gamePort = 4000;     // the joiner's own D2RLoader game_port
		int acceptTimeoutMs = 120'000;
		LogFn log;
		std::function<void(const std::string& why)> ended;  // on a relay thread
	};

	explicit JoinRelay(Options options);
	~JoinRelay();
	JoinRelay(const JoinRelay&) = delete;
	JoinRelay& operator=(const JoinRelay&) = delete;

	// Opens the relay socket, then the listener. Blocking. On success
	// `address` is what to type into D2R's Join Game box ("127.0.0.1", or
	// another loopback address when that port is taken). `error` is the relay's
	// refusal ("over_budget", "no_room", ...) or "no answer".
	bool Start(std::string* address, std::string* error);
	void Stop();
	bool Connected() const { return connected_; }

private:
	void Run();
	Options opt_;
	std::unique_ptr<WebSocket> ws_;
	uintptr_t listener_ = ~uintptr_t(0);
	uintptr_t game_ = ~uintptr_t(0);
	std::thread runner_, pinger_;
	std::atomic<bool> stop_ {false};
	std::atomic<bool> connected_ {false};
	std::mutex sendMu_;
};

}  // namespace rooms::net
