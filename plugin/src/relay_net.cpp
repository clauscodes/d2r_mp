#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include "relay_net.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "json.h"

namespace rooms::net {

namespace {

constexpr int kPingEveryMs = 20'000;
constexpr int kTrafficLogEvery = 3;  // pings: a traffic line a minute
constexpr const char* kPing = "{\"t\":\"ping\"}";

std::wstring Wide(const std::string& s) {
	if (s.empty()) return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
	return w;
}

void Log(const LogFn& log, const std::string& line) {
	if (log) log(line);
}

std::string ErrorCode(std::string_view body) {
	const auto doc = json::Parse(body);
	return doc ? (*doc)["error"].asString() : std::string();
}

// Sleeps up to `ms`, waking early when `stop` is set.
void Nap(const std::atomic<bool>& stop, int ms) {
	for (int waited = 0; waited < ms && !stop; waited += 100) Sleep(100);
}

void NoDelay(SOCKET s) {
	BOOL on = TRUE;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

bool SendAll(SOCKET s, const char* data, size_t n) {
	while (n > 0) {
		const int sent = ::send(s, data, static_cast<int>(std::min<size_t>(n, 1 << 20)), 0);
		if (sent <= 0) return false;
		data += sent;
		n -= static_cast<size_t>(sent);
	}
	return true;
}

void CloseSocket(uintptr_t& s) {
	const SOCKET sock = static_cast<SOCKET>(s);
	if (sock == INVALID_SOCKET) return;
	s = static_cast<uintptr_t>(INVALID_SOCKET);
	shutdown(sock, SD_BOTH);
	closesocket(sock);
}

struct HttpTarget {
	RelayUrl url;
	std::wstring host;
};

std::optional<HttpTarget> Target(const std::string& relayUrl) {
	auto u = ParseRelayUrl(relayUrl);
	if (!u) return std::nullopt;
	return HttpTarget {*u, Wide(u->host)};
}

// One WinHTTP request: GET, optional upgrade. Returns the handles open.
struct Request {
	HINTERNET session = nullptr, connect = nullptr, request = nullptr;
	int status = 0;
	std::string body;

	~Request() {
		if (request) WinHttpCloseHandle(request);
		if (connect) WinHttpCloseHandle(connect);
		if (session) WinHttpCloseHandle(session);
	}

	bool Send(const HttpTarget& t, const std::string& path, const std::string& headers, bool upgrade, int timeoutMs) {
		session = WinHttpOpen(L"D2R Rooms", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session) return false;
		WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
		connect = WinHttpConnect(session, t.host.c_str(), static_cast<INTERNET_PORT>(t.url.port), 0);
		if (!connect) return false;
		const std::wstring wpath = Wide(t.url.path + path);
		request = WinHttpOpenRequest(connect, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
		                             t.url.secure ? WINHTTP_FLAG_SECURE : 0);
		if (!request) return false;
		if (upgrade && !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) return false;
		const std::wstring wheaders = Wide(headers);
		if (!WinHttpSendRequest(request, wheaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wheaders.c_str(), static_cast<DWORD>(-1L),
		                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
			return false;
		if (!WinHttpReceiveResponse(request, nullptr)) return false;
		DWORD code = 0, size = sizeof(code);
		WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
		                    WINHTTP_NO_HEADER_INDEX);
		status = static_cast<int>(code);
		if (status != 101) {
			char buf[4096];
			DWORD read = 0;
			while (body.size() < 64 * 1024 && WinHttpReadData(request, buf, sizeof(buf), &read) && read > 0) body.append(buf, read);
		}
		return true;
	}
};

}  // namespace

// --- WebSocket ----------------------------------------------------------------

class WebSocket {
public:
	enum class Kind { Binary, Text, Closed };

	// nullptr with `status` (0 = no answer) and `error` when there is no upgrade.
	static std::unique_ptr<WebSocket> Connect(const std::string& url, const std::string& path, const std::string& headers, int* status,
	                                          std::string* error) {
		*status = 0;
		const auto target = Target(url);
		if (!target) {
			*error = "bad relay address";
			return nullptr;
		}
		Request r;
		if (!r.Send(*target, path, headers, true, 8000)) {
			*error = "no answer";
			return nullptr;
		}
		*status = r.status;
		if (r.status != 101) {
			*error = ErrorCode(r.body);
			if (error->empty()) *error = "HTTP " + std::to_string(r.status);
			return nullptr;
		}
		// The game may sit in a lobby for long: pings keep data flowing, so a
		// long receive timeout never fires on a live socket.
		DWORD hour = 3600 * 1000;
		WinHttpSetOption(r.request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &hour, sizeof(hour));
		HINTERNET ws = WinHttpWebSocketCompleteUpgrade(r.request, 0);
		if (!ws) {
			*error = "upgrade failed";
			return nullptr;
		}
		auto out = std::unique_ptr<WebSocket>(new WebSocket());
		out->session_ = r.session;
		out->connect_ = r.connect;
		out->ws_ = ws;
		r.session = r.connect = nullptr;  // owned by the socket now; the request handle closes with `r`
		return out;
	}

	~WebSocket() {
		Close(1000, "");
		if (connect_) WinHttpCloseHandle(connect_);
		if (session_) WinHttpCloseHandle(session_);
	}

	bool Send(const void* data, size_t n, bool text) {
		std::lock_guard lock(sendMu_);
		if (closed_) return false;
		const auto type = text ? WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE : WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
		return WinHttpWebSocketSend(ws_, type, const_cast<void*>(data), static_cast<DWORD>(n)) == NO_ERROR;
	}
	bool Send(const std::string& s, bool text = false) { return Send(s.data(), s.size(), text); }

	// One whole message. Only one thread receives.
	Kind Receive(std::string* out) {
		out->clear();
		char buf[16 * 1024];
		for (;;) {
			if (closed_) return Kind::Closed;
			DWORD read = 0;
			WINHTTP_WEB_SOCKET_BUFFER_TYPE type {};
			if (WinHttpWebSocketReceive(ws_, buf, sizeof(buf), &read, &type) != NO_ERROR) return Kind::Closed;
			if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
				USHORT code = 0;
				DWORD reasonLen = 0;
				char reason[128] {};
				if (WinHttpWebSocketQueryCloseStatus(ws_, &code, reason, sizeof(reason), &reasonLen) == NO_ERROR) {
					closeCode_ = code;
					closeReason_.assign(reason, reasonLen);
				}
				return Kind::Closed;
			}
			out->append(buf, read);
			if (out->size() > kMaxMessage + 16) return Kind::Closed;
			if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) return Kind::Binary;
			if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) return Kind::Text;
		}
	}

	// Sends a close frame and unblocks a waiting Receive.
	void Close(uint16_t code, const std::string& reason) {
		HINTERNET ws = nullptr;
		{
			std::lock_guard lock(sendMu_);
			if (closed_) return;
			closed_ = true;
			ws = ws_;
		}
		WinHttpWebSocketShutdown(ws, code, const_cast<char*>(reason.data()), static_cast<DWORD>(reason.size()));
		WinHttpCloseHandle(ws);
	}

	uint16_t CloseCode() const { return closeCode_; }
	const std::string& CloseReason() const { return closeReason_; }

private:
	WebSocket() = default;
	HINTERNET session_ = nullptr, connect_ = nullptr, ws_ = nullptr;
	std::mutex sendMu_;
	std::atomic<bool> closed_ {false};
	uint16_t closeCode_ = 0;
	std::string closeReason_;
};

// --- helpers --------------------------------------------------------------------

bool StartNetworking() {
	static const bool ok = [] {
		WSADATA data {};
		return WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	return ok;
}

std::string RandomToken() {
	uint8_t bytes[16] {};
	BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	return Base64Url(bytes, sizeof(bytes));
}

RelayHealth CheckRelay(const std::string& url, int timeoutMs) {
	const auto target = Target(url);
	if (!target) return ParseHealth(0, {});
	Request r;
	if (!r.Send(*target, "/healthz", {}, false, timeoutMs)) return ParseHealth(0, {});
	return ParseHealth(r.status, r.body);
}

RelayServer PickRelay(const std::vector<RelayServer>& relays, std::vector<RelayHealth>* states) {
	RelayServer picked;
	for (const RelayServer& relay : relays) {
		const RelayHealth h = CheckRelay(relay.url);
		if (states) states->push_back(h);
		if (picked.url.empty() && h.state == RelayState::Active) {
			picked = relay;
			if (!states) break;
		}
	}
	return picked;
}

// --- traffic ----------------------------------------------------------------------

namespace {

std::string Rate(uint64_t bytes, uint64_t msgs, double seconds) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.0f B/s %.1f msg/s", bytes / seconds, msgs / seconds);
	return buf;
}

std::string Amount(uint64_t bytes) {
	char buf[32];
	if (bytes < 10 * 1024) std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
	else if (bytes < 10 * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
	else std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
	return buf;
}

}  // namespace

Traffic::Traffic() : start_(Clock::now()), last_(start_) {}

void Traffic::Up(size_t bytes) {
	upBytes_ += bytes;
	++upMsgs_;
}

void Traffic::Down(size_t bytes) {
	downBytes_ += bytes;
	++downMsgs_;
}

std::string Traffic::SinceLast() {
	const auto now = Clock::now();
	const double seconds = std::max(0.001, std::chrono::duration<double>(now - last_).count());
	const uint64_t ub = upBytes_, um = upMsgs_, db = downBytes_, dm = downMsgs_;
	std::string line;
	if (um != lastUpMsgs_ || dm != lastDownMsgs_)
		line = "up " + Rate(ub - lastUpBytes_, um - lastUpMsgs_, seconds) + ", down " + Rate(db - lastDownBytes_, dm - lastDownMsgs_, seconds);
	last_ = now;
	lastUpBytes_ = ub, lastUpMsgs_ = um, lastDownBytes_ = db, lastDownMsgs_ = dm;
	return line;
}

std::string Traffic::Total() const {
	const double seconds = std::max(0.001, std::chrono::duration<double>(Clock::now() - start_).count());
	const uint64_t ub = upBytes_, um = upMsgs_, db = downBytes_, dm = downMsgs_;
	return std::to_string(static_cast<long long>(seconds)) + " s: up " + Amount(ub) + " (" + Rate(ub, um, seconds) + "), down " +
	       Amount(db) + " (" + Rate(db, dm, seconds) + ")";
}

// --- host -------------------------------------------------------------------------

struct HostRelay::Pipe {
	uint8_t n = 0;
	uintptr_t socket = static_cast<uintptr_t>(INVALID_SOCKET);
	std::mutex mu;
	std::string pending;  // bytes from the joiner before the game connection is up
	bool connected = false;
	bool closed = false;
};

HostRelay::HostRelay(Options options) : opt_(std::move(options)) {}

HostRelay::~HostRelay() {
	Stop();
}

bool HostRelay::Start(std::string* error) {
	StartNetworking();
	room_ = RandomToken();
	secret_ = RandomToken();
	joinKey_ = RandomToken();
	std::string headers = "X-Host-Secret: " + secret_ + "\r\nX-Join-Key-Hash: " + Sha256Hex(joinKey_) + "\r\n";
	if (!opt_.relay.key.empty()) headers += "X-Relay-Key: " + opt_.relay.key + "\r\n";
	int status = 0;
	ws_ = WebSocket::Connect(opt_.relay.url, "/v1/rooms/" + room_ + "/host", headers, &status, error);
	if (!ws_) {
		Log(opt_.log, "relay host: " + opt_.relay.url + " refused: " + *error);
		return false;
	}
	Log(opt_.log, "relay host: room open on " + opt_.relay.url);
	stop_ = false;
	reader_ = std::thread([this] { ReadLoop(); });
	pinger_ = std::thread([this] {
		for (int pings = 1; !stop_; ++pings) {
			Nap(stop_, kPingEveryMs);
			if (!stop_ && ws_) ws_->Send(kPing, true);
			if (pings % kTrafficLogEvery != 0) continue;
			const std::string line = traffic_.SinceLast();
			if (!stop_ && !line.empty()) Log(opt_.log, "relay host: traffic " + line);
		}
	});
	return true;
}

void HostRelay::LogTotal() {
	if (!totalLogged_.exchange(true)) Log(opt_.log, "relay host: traffic in all, " + traffic_.Total());
}

void HostRelay::Stop() {
	if (stop_.exchange(true) && !reader_.joinable()) return;
	if (ws_) ws_->Close(1000, "host left");
	if (reader_.joinable()) reader_.join();
	if (pinger_.joinable()) pinger_.join();
	std::vector<std::shared_ptr<Pipe>> pipes;
	{
		std::lock_guard lock(mu_);
		pipes.swap(pipes_);
	}
	for (auto& p : pipes) {
		std::lock_guard lock(p->mu);
		p->closed = true;
		CloseSocket(p->socket);
	}
	for (auto& t : threads_)
		if (t.joinable()) t.join();
	threads_.clear();
	if (ws_) LogTotal();
	ws_.reset();
}

int HostRelay::Pipes() const {
	std::lock_guard lock(mu_);
	return static_cast<int>(pipes_.size());
}

void HostRelay::Send(const std::string& frame) {
	if (ws_) ws_->Send(frame);
}

void HostRelay::ClosePipe(uint8_t n, bool tellRelay) {
	std::shared_ptr<Pipe> pipe;
	{
		std::lock_guard lock(mu_);
		for (auto it = pipes_.begin(); it != pipes_.end(); ++it) {
			if ((*it)->n == n) {
				pipe = *it;
				pipes_.erase(it);
				break;
			}
		}
	}
	if (!pipe) return;
	{
		std::lock_guard lock(pipe->mu);
		pipe->closed = true;
		CloseSocket(pipe->socket);
	}
	if (tellRelay) Send(Frame(kFrameClose, n));
	Log(opt_.log, "relay host: pipe " + std::to_string(n) + " closed");
}

void HostRelay::PipeThread(std::shared_ptr<Pipe> pipe) {
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in addr {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<u_short>(opt_.gamePort));
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (s == INVALID_SOCKET || connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
		if (s != INVALID_SOCKET) closesocket(s);
		Log(opt_.log, "relay host: the game does not listen on 127.0.0.1:" + std::to_string(opt_.gamePort));
		std::lock_guard lock(mu_);
		const bool current = std::find(pipes_.begin(), pipes_.end(), pipe) != pipes_.end();
		if (!current) return;
		pipes_.erase(std::find(pipes_.begin(), pipes_.end(), pipe));
		Send(Frame(kFrameClose, pipe->n, "the host's game is not listening"));
		return;
	}
	NoDelay(s);
	{
		std::lock_guard lock(pipe->mu);
		if (pipe->closed) {
			closesocket(s);
			return;
		}
		pipe->socket = static_cast<uintptr_t>(s);
		pipe->connected = true;
		if (!pipe->pending.empty()) SendAll(s, pipe->pending.data(), pipe->pending.size());
		pipe->pending.clear();
	}
	char buf[16 * 1024];
	for (;;) {
		const int n = recv(s, buf, sizeof(buf), 0);
		if (n <= 0) break;
		Send(Frame(kFrameData, pipe->n, std::string_view(buf, static_cast<size_t>(n))));
		traffic_.Up(static_cast<size_t>(n));
	}
	// The game ended this connection (or the relay closed the pipe first:
	// then it is no longer listed and nothing is sent).
	std::shared_ptr<Pipe> listed;
	{
		std::lock_guard lock(mu_);
		auto it = std::find(pipes_.begin(), pipes_.end(), pipe);
		if (it != pipes_.end()) listed = pipe;
	}
	if (listed) ClosePipe(pipe->n, true);
}

void HostRelay::ReadLoop() {
	std::string msg;
	for (;;) {
		const auto kind = ws_->Receive(&msg);
		if (kind == WebSocket::Kind::Closed) break;
		if (kind == WebSocket::Kind::Text || msg.size() < 2) continue;  // hello, pong
		const uint8_t type = static_cast<uint8_t>(msg[0]);
		const uint8_t n = static_cast<uint8_t>(msg[1]);
		if (type == kFrameOpen) {
			auto pipe = std::make_shared<Pipe>();
			pipe->n = n;
			{
				std::lock_guard lock(mu_);
				pipes_.push_back(pipe);
			}
			Log(opt_.log, "relay host: joiner on pipe " + std::to_string(n));
			threads_.emplace_back([this, pipe] { PipeThread(pipe); });
		} else if (type == kFrameData) {
			std::shared_ptr<Pipe> pipe;
			{
				std::lock_guard lock(mu_);
				for (auto& p : pipes_)
					if (p->n == n) pipe = p;
			}
			if (!pipe) continue;
			traffic_.Down(msg.size() - 2);
			std::lock_guard lock(pipe->mu);
			if (pipe->closed) continue;
			if (pipe->connected) SendAll(static_cast<SOCKET>(pipe->socket), msg.data() + 2, msg.size() - 2);
			else pipe->pending.append(msg, 2);
		} else if (type == kFrameClose) {
			ClosePipe(n, false);
		}
	}
	if (stop_) return;
	Log(opt_.log, "relay host: the relay socket closed (" + std::to_string(ws_->CloseCode()) + " " + ws_->CloseReason() + ")");
	std::vector<uint8_t> open;
	{
		std::lock_guard lock(mu_);
		for (auto& p : pipes_) open.push_back(p->n);
	}
	for (const uint8_t n : open) ClosePipe(n, false);
	LogTotal();
	if (opt_.lost) opt_.lost();
}

// --- join -------------------------------------------------------------------------

JoinRelay::JoinRelay(Options options) : opt_(std::move(options)) {}

JoinRelay::~JoinRelay() {
	Stop();
}

bool JoinRelay::Start(std::string* address, std::string* error) {
	StartNetworking();
	int status = 0;
	ws_ = WebSocket::Connect(opt_.relay.url, "/v1/rooms/" + opt_.relay.room + "/join", "X-Join-Key: " + opt_.relay.key + "\r\n", &status,
	                         error);
	if (!ws_) {
		Log(opt_.log, "relay join: " + opt_.relay.url + " refused: " + *error);
		return false;
	}
	// The game joins this listener. D2RLoader may hold the same port on all
	// addresses for its hosting check; an exclusive bind on one loopback
	// address still works, and another loopback address is the fallback.
	for (const char* ip : {"127.0.0.1", "127.0.0.2", "127.0.0.3"}) {
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID_SOCKET) break;
		BOOL on = TRUE;
		setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
		sockaddr_in addr {};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_cast<u_short>(opt_.gamePort));
		inet_pton(AF_INET, ip, &addr.sin_addr);
		if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && listen(s, 1) == 0) {
			listener_ = static_cast<uintptr_t>(s);
			*address = ip;
			break;
		}
		closesocket(s);
	}
	if (listener_ == static_cast<uintptr_t>(INVALID_SOCKET)) {
		*error = "port " + std::to_string(opt_.gamePort) + " is taken on loopback";
		ws_->Close(1000, "no listener");
		ws_.reset();
		return false;
	}
	Log(opt_.log, "relay join: pipe up; the game joins " + *address + ":" + std::to_string(opt_.gamePort));
	stop_ = false;
	runner_ = std::thread([this] { Run(); });
	pinger_ = std::thread([this] {
		for (int pings = 1; !stop_; ++pings) {
			Nap(stop_, kPingEveryMs);
			if (!stop_ && ws_) ws_->Send(kPing, true);
			if (pings % kTrafficLogEvery != 0) continue;
			const std::string line = traffic_.SinceLast();
			if (!stop_ && !line.empty()) Log(opt_.log, "relay join: traffic " + line);
		}
	});
	return true;
}

void JoinRelay::Run() {
	std::string why = "ended";
	// 1. Wait for the game to connect.
	const SOCKET listener = static_cast<SOCKET>(listener_);
	fd_set set;
	FD_ZERO(&set);
	FD_SET(listener, &set);
	timeval tv {opt_.acceptTimeoutMs / 1000, (opt_.acceptTimeoutMs % 1000) * 1000};
	SOCKET game = INVALID_SOCKET;
	if (select(0, &set, nullptr, nullptr, &tv) == 1) game = accept(listener, nullptr, nullptr);
	CloseSocket(listener_);
	if (game == INVALID_SOCKET) {
		why = stop_ ? "stopped" : "the game did not connect";
	} else {
		NoDelay(game);
		game_ = static_cast<uintptr_t>(game);
		connected_ = true;
		Log(opt_.log, "relay join: the game connected");
		// 2. Game -> relay on its own thread; relay -> game here.
		std::thread up([this, game] {
			char buf[16 * 1024];
			for (;;) {
				const int n = recv(game, buf, sizeof(buf), 0);
				if (n <= 0 || !ws_->Send(buf, static_cast<size_t>(n), false)) break;
				traffic_.Up(static_cast<size_t>(n));
			}
			ws_->Close(1000, "game closed");
		});
		std::string msg;
		for (;;) {
			const auto kind = ws_->Receive(&msg);
			if (kind == WebSocket::Kind::Closed) break;
			if (kind != WebSocket::Kind::Binary) continue;
			traffic_.Down(msg.size());
			if (!SendAll(game, msg.data(), msg.size())) break;
		}
		why = ws_->CloseReason().empty() ? "relay closed" : ws_->CloseReason();
		CloseSocket(game_);
		ws_->Close(1000, "");
		up.join();
	}
	connected_ = false;
	Log(opt_.log, "relay join: " + why);
	if (traffic_.UpBytes() || traffic_.DownBytes()) Log(opt_.log, "relay join: traffic in all, " + traffic_.Total());
	if (!stop_ && opt_.ended) opt_.ended(why);
}

void JoinRelay::Stop() {
	stop_ = true;
	if (ws_) ws_->Close(1000, "stopped");
	CloseSocket(listener_);
	CloseSocket(game_);
	if (runner_.joinable()) runner_.join();
	if (pinger_.joinable()) pinger_.join();
	ws_.reset();
}

}  // namespace rooms::net
