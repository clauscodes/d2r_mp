// The plugin's relay client against a running relay, with a fake game:
//
//   relay_e2e.exe ws://127.0.0.1:8080          (relay/python/relay_server.py)
//   relay_e2e.exe ws://127.0.0.1:8787          (wrangler dev in relay/cloudflare)
//
// The "host game" is an echo server on loopback; two "joiner games" connect to
// their JoinRelay listeners, send bytes and must get the same bytes back
// through relay -> HostRelay -> echo -> HostRelay -> relay. No D2R needed.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "relay_net.h"

using namespace rooms;

namespace {

int g_failed = 0;

void Check(bool ok, const char* what) {
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++g_failed;
}

int FreePort() {
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in a {};
	a.sin_family = AF_INET;
	inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
	bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
	int len = sizeof(a);
	getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
	closesocket(s);
	return ntohs(a.sin_port);
}

// The host's "game": echoes every connection.
struct Echo {
	SOCKET listener = INVALID_SOCKET;
	std::vector<std::thread> threads;
	std::atomic<int> connections {0};

	explicit Echo(int port) {
		listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		sockaddr_in a {};
		a.sin_family = AF_INET;
		a.sin_port = htons(static_cast<u_short>(port));
		inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
		bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof(a));
		listen(listener, 8);
		threads.emplace_back([this] {
			for (;;) {
				SOCKET c = accept(listener, nullptr, nullptr);
				if (c == INVALID_SOCKET) return;
				++connections;
				threads.emplace_back([c] {
					char buf[8192];
					for (;;) {
						const int n = recv(c, buf, sizeof(buf), 0);
						if (n <= 0 || send(c, buf, n, 0) != n) break;
					}
					closesocket(c);
				});
			}
		});
	}
	~Echo() {
		closesocket(listener);
		for (auto& t : threads) t.join();
	}
};

SOCKET Dial(const std::string& ip, int port) {
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in a {};
	a.sin_family = AF_INET;
	a.sin_port = htons(static_cast<u_short>(port));
	inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
	if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
		closesocket(s);
		return INVALID_SOCKET;
	}
	DWORD timeout = 10000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
	return s;
}

// Sends `data` and reads the same number of bytes back.
bool RoundTrip(SOCKET s, const std::string& data) {
	std::thread writer([&] { send(s, data.data(), static_cast<int>(data.size()), 0); });
	std::string got;
	char buf[16384];
	while (got.size() < data.size()) {
		const int n = recv(s, buf, sizeof(buf), 0);
		if (n <= 0) break;
		got.append(buf, static_cast<size_t>(n));
	}
	writer.join();
	return got == data;
}

template <class Fn>
bool WaitFor(Fn fn, int ms) {
	for (int t = 0; t < ms; t += 50) {
		if (fn()) return true;
		Sleep(50);
	}
	return fn();
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::printf("usage: relay_e2e <ws://relay>\n");
		return 2;
	}
	const std::string url = argv[1];
	net::StartNetworking();
	auto log = [](const std::string& line) { std::printf("     %s\n", line.c_str()); };

	const RelayHealth health = net::CheckRelay(url);
	Check(health.state == RelayState::Active, "the relay answers /healthz and takes rooms");
	std::vector<RelayHealth> states;
	const RelayServer picked = net::PickRelay({{"dead", "ws://127.0.0.1:1", ""}, {"test", url, ""}}, &states);
	Check(picked.url == url && states.size() == 2 && states[0].state == RelayState::NoAnswer, "PickRelay skips a dead relay");

	const int hostPort = FreePort();
	Echo echo(hostPort);
	net::HostRelay host({.relay = {"test", url, ""}, .gamePort = hostPort, .log = log, .lost = nullptr});
	std::string error;
	Check(host.Start(&error), "the host claims a room");

	// Two joiners, as two PCs would be; each has its own game_port here
	// because both run on this one PC.
	const int joinPort1 = FreePort(), joinPort2 = FreePort();
	std::atomic<int> ended {0};
	net::JoinRelay j1({.relay = host.Ref(), .gamePort = joinPort1, .acceptTimeoutMs = 10000, .log = log,
	                   .ended = [&](const std::string&) { ++ended; }});
	std::string address;
	Check(j1.Start(&address, &error), "joiner 1 opens its pipe");
	Check(address == "127.0.0.1", "joiner 1 listens on 127.0.0.1");
	SOCKET g1 = Dial(address, joinPort1);
	Check(g1 != INVALID_SOCKET, "game 1 connects to its listener");
	Check(RoundTrip(g1, "hello through the relay"), "small bytes go there and back");
	std::string big(200 * 1024, '\0');
	for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>((i * 31 + 7) & 0xff);
	Check(RoundTrip(g1, big), "200 KB go there and back in order");

	net::JoinRelay j2({.relay = host.Ref(), .gamePort = joinPort2, .acceptTimeoutMs = 10000, .log = log,
	                   .ended = [&](const std::string&) { ++ended; }});
	std::string address2;
	Check(j2.Start(&address2, &error), "joiner 2 opens its pipe");
	SOCKET g2 = Dial(address2, joinPort2);
	Check(g2 != INVALID_SOCKET && RoundTrip(g2, "second player"), "a second joiner plays at the same time");
	Check(RoundTrip(g1, "first still here"), "the first joiner is unaffected");
	Check(WaitFor([&] { return host.Pipes() == 2 && echo.connections == 2; }, 3000), "two pipes, two game connections on the host");

	net::JoinRelay wrong({.relay = {url, host.Ref().room, net::RandomToken()}, .gamePort = FreePort(), .acceptTimeoutMs = 1000, .log = log,
	                      .ended = nullptr});
	std::string wrongAddress;
	Check(!wrong.Start(&wrongAddress, &error) && error == "bad_join_key", "a wrong join key is refused");

	closesocket(g1);
	Check(WaitFor([&] { return host.Pipes() == 1; }, 5000), "game 1 leaving closes its pipe on the host");
	Check(WaitFor([&] { return ended >= 1; }, 5000), "joiner 1 reports the end");

	host.Stop();
	Check(WaitFor([&] { return ended >= 2; }, 8000), "the host leaving ends joiner 2");
	closesocket(g2);
	j1.Stop();
	j2.Stop();

	std::printf("%s\n", g_failed ? "SOME CHECKS FAILED" : "all checks passed");
	return g_failed ? 1 : 0;
}
