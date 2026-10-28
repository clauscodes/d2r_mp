#include "relay_config.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>

#include "json.h"

namespace rooms {

namespace {

std::string_view Trim(std::string_view s) {
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
	return s;
}

std::string Lower(std::string_view s) {
	std::string out(s);
	for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

bool IsToken(std::string_view s) {
	if (s.size() < 16 || s.size() > 128) return false;
	return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; });
}

template <class Fn>
void ForEachLine(std::string_view text, Fn fn) {
	size_t start = 0;
	while (start <= text.size()) {
		const size_t end = text.find('\n', start);
		fn(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
		if (end == std::string_view::npos) break;
		start = end + 1;
	}
}

}  // namespace

std::string DefaultRelayIni() {
	return std::string(
		"; D2R Rooms relays. A relay carries your game's bytes when friends cannot reach your PC\n"
		"; (no port forwarding, router without UPnP, carrier-grade NAT). Both PCs dial out to it.\n"
		"; Guide to running your own: docs/host_a_relay.md in the D2R Rooms repository.\n"
		"\n"
		"[relay]\n"
		"; auto   = only when D2RLoader reports that your port cannot be reached from outside\n"
		"; always = every room you host goes through a relay\n"
		"; never  = never (friends then need your port open)\n"
		"mode = auto\n"
		"\n"
		"[relays]\n"
		"; One relay per line:  name = wss://address [key]\n"
		"; The key is only for a private relay; its owner gives it to you.\n"
		"; When you host, the first relay that answers and takes new rooms is used.\n"
		"; When you join, the plugin uses the relay the host's room names.\n") +
		kDefaultRelayName + " = " + kDefaultRelayUrl + "\n";
}

RelayConfig ParseRelayIni(std::string_view text) {
	RelayConfig cfg;
	std::string section;
	ForEachLine(text, [&](std::string_view raw) {
		std::string_view line = Trim(raw);
		if (line.empty() || line.front() == ';' || line.front() == '#') return;
		if (line.front() == '[' && line.back() == ']') {
			section = Lower(Trim(line.substr(1, line.size() - 2)));
			return;
		}
		const size_t eq = line.find('=');
		if (eq == std::string_view::npos) return;
		const std::string_view name = Trim(line.substr(0, eq));
		std::string_view value = Trim(line.substr(eq + 1));
		if (section == "relay" && Lower(name) == "mode") {
			const std::string v = Lower(value);
			if (v == "always") cfg.mode = RelayMode::Always;
			else if (v == "never") cfg.mode = RelayMode::Never;
			else cfg.mode = RelayMode::Auto;
		} else if (section == "relays" && !name.empty()) {
			const size_t space = value.find_first_of(" \t");
			std::string url(Trim(value.substr(0, space)));
			const std::string key(space == std::string_view::npos ? std::string_view() : Trim(value.substr(space)));
			while (!url.empty() && url.back() == '/') url.pop_back();
			if (!ParseRelayUrl(url)) return;
			const bool dup = std::any_of(cfg.relays.begin(), cfg.relays.end(), [&](const RelayServer& r) { return r.url == url; });
			if (!dup) cfg.relays.push_back({std::string(name), url, key});
		}
	});
	if (cfg.relays.empty()) cfg.relays.push_back({kDefaultRelayName, kDefaultRelayUrl, ""});
	return cfg;
}

const char* RelayModeName(RelayMode mode) {
	switch (mode) {
		case RelayMode::Always: return "always";
		case RelayMode::Never: return "never";
		default: return "auto";
	}
}

int ParseGamePort(std::string_view toml) {
	int port = 4000;
	std::string section;
	ForEachLine(toml, [&](std::string_view raw) {
		std::string_view line = Trim(raw);
		if (line.empty() || line.front() == '#') return;
		if (line.front() == '[') {
			const size_t close = line.find(']');
			section = Lower(Trim(line.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1)));
			return;
		}
		const size_t eq = line.find('=');
		if (section != "d2rloader.advanced" || eq == std::string_view::npos || Trim(line.substr(0, eq)) != "game_port") return;
		std::string_view value = Trim(line.substr(eq + 1));
		const size_t hash = value.find('#');
		value = Trim(value.substr(0, hash));
		int n = 0;
		for (const char c : value) {
			if (!std::isdigit(static_cast<unsigned char>(c)) || n > 65535) return;
			n = n * 10 + (c - '0');
		}
		if (n >= 1 && n <= 65535) port = n;
	});
	return port;
}

// The strings are D2RLoader 1.3.1's (D2RCore.dll); see the plan's §1.
HostingVerdict ParseHostingVerdict(std::string_view text) {
	HostingVerdict v = HostingVerdict::Unknown;
	ForEachLine(text, [&](std::string_view line) {
		if (line.find("is reachable from outside") != std::string_view::npos ||
		    line.find("Hosting verification finished: Reachable") != std::string_view::npos ||
		    line.find("The active local address is already public") != std::string_view::npos) {
			v = HostingVerdict::Reachable;
		} else if (line.find("Hosting verification finished: Unreachable") != std::string_view::npos ||
		           line.find("unreachable from outside") != std::string_view::npos) {
			v = HostingVerdict::Unreachable;
		} else if (v == HostingVerdict::Unknown &&
		           (line.find("Automatic router mapping failed") != std::string_view::npos ||
		            line.find("no compatible router was found") != std::string_view::npos ||
		            line.find("carrier-grade NAT") != std::string_view::npos ||
		            line.find("Double NAT") != std::string_view::npos)) {
			v = HostingVerdict::Suspect;
		}
	});
	return v;
}

RelayHealth ParseHealth(int status, std::string_view body) {
	RelayHealth h;
	if (status == 0) {
		h.state = RelayState::NoAnswer;
		return h;
	}
	const auto doc = json::Parse(body);
	if (status != 200 || !doc || !(*doc)["ok"].asBool() || (*doc)["relay"].asInt() != 1) {
		h.state = RelayState::NoAnswer;
		return h;
	}
	const std::string s = (*doc)["status"].asString();
	if (s == "active") h.state = RelayState::Active;
	else if (s == "near_limit") h.state = RelayState::NearLimit;
	else if (s == "limit") h.state = RelayState::Limit;
	else h.state = (*doc)["open"].asBool(true) ? RelayState::Active : RelayState::Limit;  // a relay before status
	h.usage = (*doc)["usage"].asNumber(-1);
	h.name = (*doc)["name"].asString();
	return h;
}

const char* RelayStateLabel(RelayState state) {
	switch (state) {
		case RelayState::Active: return "active";
		case RelayState::NearLimit: return "near its limit";
		case RelayState::Limit: return "limit reached";
		case RelayState::NoAnswer: return "not answering";
		default: return "checking";
	}
}

std::string RelayRefJson(const RelayRef& relay) {
	return "{\"url\":" + json::Quote(relay.url) + ",\"room\":" + json::Quote(relay.room) + ",\"key\":" + json::Quote(relay.key) + "}";
}

std::optional<RelayUrl> ParseRelayUrl(std::string_view url) {
	RelayUrl u;
	if (url.substr(0, 6) == "wss://") {
		url.remove_prefix(6);
	} else if (url.substr(0, 5) == "ws://") {
		u.secure = false;
		u.port = 80;
		url.remove_prefix(5);
	} else {
		return std::nullopt;
	}
	const size_t slash = url.find('/');
	std::string_view authority = url.substr(0, slash);
	if (slash != std::string_view::npos) {
		std::string path(url.substr(slash));
		while (!path.empty() && path.back() == '/') path.pop_back();
		u.path = path;
	}
	const size_t colon = authority.rfind(':');
	if (colon != std::string_view::npos) {
		int port = 0;
		const std::string_view digits = authority.substr(colon + 1);
		if (digits.empty() || digits.size() > 5) return std::nullopt;
		for (const char c : digits) {
			if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
			port = port * 10 + (c - '0');
		}
		if (port < 1 || port > 65535) return std::nullopt;
		u.port = port;
		authority = authority.substr(0, colon);
	}
	if (authority.empty() || authority.size() > 253) return std::nullopt;
	for (const char c : authority)
		if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') return std::nullopt;
	u.host = std::string(authority);
	return u;
}

std::string Frame(uint8_t type, uint8_t pipe, std::string_view payload) {
	std::string f;
	f.reserve(payload.size() + 2);
	f.push_back(static_cast<char>(type));
	f.push_back(static_cast<char>(pipe));
	f.append(payload);
	return f;
}

std::string Base64Url(const uint8_t* bytes, size_t n) {
	static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	std::string out;
	uint32_t acc = 0;
	int bits = 0;
	for (size_t i = 0; i < n; ++i) {
		acc = (acc << 8) | bytes[i];
		bits += 8;
		while (bits >= 6) {
			bits -= 6;
			out.push_back(kAlphabet[(acc >> bits) & 63]);
		}
	}
	if (bits > 0) out.push_back(kAlphabet[(acc << (6 - bits)) & 63]);
	return out;
}

// FIPS 180-4, small and slow enough for a few keys.
std::string Sha256Hex(std::string_view text) {
	static constexpr std::array<uint32_t, 64> k {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
		0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
		0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
		0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
		0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
		0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
	std::array<uint32_t, 8> h {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	std::string msg(text);
	const uint64_t bitLen = static_cast<uint64_t>(msg.size()) * 8;
	msg.push_back(static_cast<char>(0x80));
	while (msg.size() % 64 != 56) msg.push_back('\0');
	for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bitLen >> (i * 8)) & 0xff));
	auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
	for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
		std::array<uint32_t, 64> w {};
		for (int i = 0; i < 16; ++i) {
			const auto* p = reinterpret_cast<const unsigned char*>(msg.data() + chunk + i * 4);
			w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
		}
		for (int i = 16; i < 64; ++i) {
			const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		auto [a, b, c, d, e, f, g, hh] = h;
		for (int i = 0; i < 64; ++i) {
			const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
			const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
	}
	char out[65];
	for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
	return std::string(out, 64);
}

}  // namespace rooms
