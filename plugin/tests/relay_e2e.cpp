// The plugin's relay client against a running relay, with a fake game:
//
//   relay_e2e.exe ws://127.0.0.1:8080          (relay/python/relay_server.py)
//   relay_e2e.exe ws://127.0.0.1:8787          (wrangler dev in relay/cloudflare)
//
// The "host game" is an echo server on loopback; two "joiner games" connect to
// their JoinRelay listeners, send bytes and must get the same bytes back
// through relay -> HostRelay -> echo -> HostRelay -> relay. No D2R needed.
//
//   relay_e2e.exe <url> --bench [seconds] [packets/s] [bytes]   (30 25 120)
//
// measures instead: two joiners on 127.0.0.1 each send game-sized packets at
// a steady rate for `seconds`, the host's game echoes them, and it prints the
// round trips and the bytes/s and messages/s the plugin's counters saw; then
// one 768 KB burst for the most one pipe carries.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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

void NoDelay(SOCKET s) {
	BOOL on = TRUE;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
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

using Clock = std::chrono::steady_clock;

// One joiner's game for the bench: `pps` packets of `size` bytes a second,
// each stamped with its send time; the echo's arrival gives the round trip.
struct BenchGame {
	std::vector<double> rttMs;
	int sent = 0;

	void Run(SOCKET s, int seconds, int pps, int size) {
		const int total = seconds * pps;
		std::thread reader([&] {
			std::string packet(static_cast<size_t>(size), '\0');
			for (int got = 0; got < total; ++got) {
				size_t have = 0;
				while (have < packet.size()) {
					const int n = recv(s, packet.data() + have, static_cast<int>(packet.size() - have), 0);
					if (n <= 0) return;
					have += static_cast<size_t>(n);
				}
				Clock::rep stamp = 0;
				std::memcpy(&stamp, packet.data(), sizeof(stamp));
				const Clock::duration rtt = Clock::now().time_since_epoch() - Clock::duration(stamp);
				rttMs.push_back(std::chrono::duration<double, std::milli>(rtt).count());
			}
		});
		const auto start = Clock::now();
		std::string packet(static_cast<size_t>(size), 'g');
		for (int i = 0; i < total; ++i) {
			std::this_thread::sleep_until(start + std::chrono::microseconds(1'000'000LL * i / pps));
			const Clock::rep now = Clock::now().time_since_epoch().count();
			std::memcpy(packet.data(), &now, sizeof(now));
			if (send(s, packet.data(), size, 0) != size) break;
			++sent;
		}
		reader.join();
	}

	std::string Summary() {
		if (rttMs.empty()) return "no echoes";
		std::sort(rttMs.begin(), rttMs.end());
		auto at = [&](double q) { return rttMs[std::min(rttMs.size() - 1, static_cast<size_t>(q * rttMs.size()))]; };
		char buf[160];
		std::snprintf(buf, sizeof(buf), "%zu/%d echoed, round trip ms: median %.1f, 95%% %.1f, max %.1f", rttMs.size(), sent, at(0.5),
		              at(0.95), rttMs.back());
		return buf;
	}
};

int Bench(const std::string& url, int seconds, int pps, int size) {
	auto log = [](const std::string& line) { std::printf("     %s\n", line.c_str()); };
	std::printf("bench %s: 2 joiners x %d packets/s x %d B for %d s\n", url.c_str(), pps, size, seconds);
	const int hostPort = FreePort();
	Echo echo(hostPort);
	net::HostRelay host({.relay = {"bench", url, ""}, .gamePort = hostPort, .log = log, .lost = nullptr});
	std::string error;
	if (!host.Start(&error)) {
		std::printf("FAIL the host claims a room: %s\n", error.c_str());
		return 1;
	}
	std::vector<std::unique_ptr<net::JoinRelay>> joins;
	std::vector<SOCKET> games;
	for (int i = 0; i < 2; ++i) {
		const int port = FreePort();
		auto j = std::make_unique<net::JoinRelay>(
		    net::JoinRelay::Options {.relay = host.Ref(), .gamePort = port, .acceptTimeoutMs = 10000, .log = log, .ended = nullptr});
		std::string address;
		if (!j->Start(&address, &error)) {
			std::printf("FAIL joiner %d opens its pipe: %s\n", i + 1, error.c_str());
			return 1;
		}
		const SOCKET g = Dial(address, port);
		if (g == INVALID_SOCKET) {
			std::printf("FAIL game %d connects to %s:%d\n", i + 1, address.c_str(), port);
			return 1;
		}
		NoDelay(g);
		games.push_back(g);
		joins.push_back(std::move(j));
	}

	BenchGame bench[2];
	std::thread t0([&] { bench[0].Run(games[0], seconds, pps, size); });
	std::thread t1([&] { bench[1].Run(games[1], seconds, pps, size); });
	t0.join();
	t1.join();
	for (int i = 0; i < 2; ++i) {
		std::printf("joiner %d: %s\n", i + 1, bench[i].Summary().c_str());
		std::printf("joiner %d counters, %s\n", i + 1, joins[i]->Traffic().Total().c_str());
	}
	std::printf("host counters, %s\n", host.Traffic().Total().c_str());

	// The most one pipe carries: a burst under the relay's 1 MiB allowance.
	std::string burst(768 * 1024, '\0');
	for (size_t i = 0; i < burst.size(); ++i) burst[i] = static_cast<char>((i * 131 + 3) & 0xff);
	const auto start = Clock::now();
	const bool intact = RoundTrip(games[0], burst);
	const double secs = std::chrono::duration<double>(Clock::now() - start).count();
	std::printf("burst: 768 KB there and back %s in %.0f ms = %.1f MB/s each way\n", intact ? "intact" : "BROKEN", secs * 1000,
	            0.75 / secs);

	for (SOCKET g : games) closesocket(g);
	for (auto& j : joins) j->Stop();
	host.Stop();
	bool all = intact && bench[0].sent == seconds * pps && bench[1].sent == seconds * pps;
	for (auto& b : bench) all = all && b.rttMs.size() == static_cast<size_t>(b.sent);
	std::printf("%s\n", all ? "bench done" : "BENCH LOST BYTES");
	return all ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::printf("usage: relay_e2e <ws://relay>\n");
		return 2;
	}
	const std::string url = argv[1];
	net::StartNetworking();
	if (argc >= 3 && std::string(argv[2]) == "--bench")
		return Bench(url, argc > 3 ? std::atoi(argv[3]) : 30, argc > 4 ? std::atoi(argv[4]) : 25,
		             argc > 5 ? std::max(8, std::atoi(argv[5])) : 120);
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
