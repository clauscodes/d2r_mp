// Relays (relay/PROTOCOL.md) as plain data: the player's relay list, what
// D2RLoader's log says about the host's port, a relay's /healthz answer and the
// host socket's frames. No network or D2RLoader code, so the unit tests cover it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rooms {

// Ours; the guide (docs/host_a_relay.md) shows how to add others.
inline constexpr const char* kDefaultRelayName = "D2R Rooms";
inline constexpr const char* kDefaultRelayUrl = "wss://d2r-rooms-relay.d2r-rooms.workers.dev";
inline constexpr const char* kRelayIniName = "d2r-rooms.ini";

struct RelayServer {
	std::string name;
	std::string url;  // ws:// or wss://, no trailing slash
	std::string key;  // only for a relay closed to the public; sent by hosts only
};

enum class RelayMode {
	Auto,    // only when D2RLoader reports the port unreachable from outside
	Always,  // every hosted room uses a relay (tests, or a router D2RLoader cannot read)
	Never,
};

struct RelayConfig {
	RelayMode mode = RelayMode::Auto;
	std::vector<RelayServer> relays;  // in the player's order; never empty after parsing
};

// The file written when there is none: ours first, comments on how to add more.
std::string DefaultRelayIni();
// Unknown lines are skipped; no [relays] entry at all gives ours alone.
RelayConfig ParseRelayIni(std::string_view text);
const char* RelayModeName(RelayMode mode);

// d2rloader.toml: [d2rloader.advanced] game_port, used for hosting and joining.
int ParseGamePort(std::string_view toml);

// What d2rloader.log says about the hosted port, read from where the host
// started its game.
enum class HostingVerdict {
	Unknown,      // nothing yet (or hosting_check is off)
	Reachable,    // friends can connect straight to the host
	Suspect,      // the router mapping failed; the outside check has not answered yet
	Unreachable,  // D2RLoader checked from outside: blocked
};
HostingVerdict ParseHostingVerdict(std::string_view logText);

// PROTOCOL.md §1.1, plus the client's own "not answering".
enum class RelayState { Unknown, Active, NearLimit, Limit, NoAnswer };

struct RelayHealth {
	RelayState state = RelayState::Unknown;
	double usage = -1;  // 0..1, -1 when the relay does not say
	std::string name;
};

// `status` 0 = no answer.
RelayHealth ParseHealth(int status, std::string_view body);
// "active", "near its limit", "limit reached", "not answering", "checking".
const char* RelayStateLabel(RelayState state);

// The relay a room uses, as the board hands it out.
struct RelayRef {
	std::string url;
	std::string room;
	std::string key;  // the join key
};
std::string RelayRefJson(const RelayRef& relay);

struct RelayUrl {
	bool secure = true;
	std::string host;
	int port = 443;
	std::string path;  // "" or "/prefix", no trailing slash
};
std::optional<RelayUrl> ParseRelayUrl(std::string_view url);

// Host socket frames.
inline constexpr uint8_t kFrameOpen = 1;
inline constexpr uint8_t kFrameData = 2;
inline constexpr uint8_t kFrameClose = 3;
inline constexpr size_t kMaxMessage = 65536;
std::string Frame(uint8_t type, uint8_t pipe, std::string_view payload = {});

// 22 base64url characters from 16 random bytes: room ids, host secrets, join keys.
std::string Base64Url(const uint8_t* bytes, size_t n);
std::string Sha256Hex(std::string_view text);

}  // namespace rooms
