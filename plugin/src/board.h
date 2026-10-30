// The board's protocol (SPEC §5) as plain data: request bodies the plugin
// sends and the answers it reads. No D2RLoader or network code here, so all of
// it is covered by the unit tests.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "relay_config.h"

namespace rooms {

inline constexpr const char* kDefaultServer = "https://d2r-rooms.d2r-rooms.workers.dev";

struct Mod {
	std::string id = "none";
	std::string version;
};

struct Player {
	std::string name;
	std::string cls;
	int level = 0;  // 0 = unknown
	int act = 0;
	std::string area;
	bool host = false;
	std::optional<Mod> mod;
};

struct Room {
	std::string id;
	std::string code;
	std::string name;
	Mod mod;
	std::string difficulty;
	std::string mode;
	int act = 0;
	int maxPlayers = 8;
	int playersCount = 1;
	bool locked = false;
	std::vector<Player> players;
	std::string relayUrl;                           // the host plays through this relay
	RelayState relayState = RelayState::Unknown;  // filled in by the plugin from the relay itself
};

struct JoinAnswer {
	std::string address;
	int port = 4000;
	bool ipv6 = false;
	bool modMismatch = false;
	std::optional<RelayRef> relay;  // join through it; the address is the fallback
};

struct Hosted {
	std::string roomId;
	std::string code;
	std::string token;
	std::string address;
	int port = 4000;
	bool ipv6 = false;
	int heartbeatEvery = 60;
};

// The mod this game was started with, from D2RLoader's "-mod <name>" argument.
Mod DetectMod(std::string_view commandLine);
// The mod's own plugin version from d2rloader.log ("[PLUGIN] D2R VR 0.189.0 by
// natpoh [mod]"), the last one listed; "" when none. D2RLoader joins only
// players with the same shared plugins, so the board shows it.
std::string ModPluginVersion(std::string_view loaderLog);

// "Authentic 0.175.0", "Unleashed", "no VR mod", or the id itself.
std::string ModLabel(const Mod& mod);

std::optional<std::vector<Room>> ParseRooms(std::string_view body);
std::optional<JoinAnswer> ParseJoin(std::string_view body);
std::optional<Hosted> ParseCreate(std::string_view body);
// The "error" field of a refusal ("bad_password", ...), or "" when absent.
std::string ParseError(std::string_view body);

std::string CreateBody(std::string_view name, const Mod& mod, std::string_view password, int act);
std::string JoinBody(std::string_view roomId, std::string_view password, const Mod& mod);
// relayJson: "" leaves the room's relay as it is, "null" clears it, or RelayRefJson().
std::string CreateBody(std::string_view name, const Mod& mod, std::string_view password, int act, std::string_view relayJson);
std::string HeartbeatBody(int act, int level, std::string_view relayJson = {});

// One line per room and one for its players, as shown in the list.
std::string RoomLine(const Room& room);
std::string PlayersLine(const Room& room);

// The same, in the D2R Rooms window's columns.
struct RoomColumns {
	std::string name;        // "test [EL9A]", "(password)" when locked
	std::string game;        // "Normal HC, Act 1"
	std::string players;     // "1/8"
	std::string version;     // "Authentic 0.189.0"
	std::string connection;  // "relay: active" or "direct"
};
RoomColumns Columns(const Room& room);

// d2r-rooms-status.json beside the plugin: what the bridge does, for D2R VR
// Settings' Multiplayer tab (it shows it; the toggles it writes go to d2r-rooms.ini).
struct BridgeStatus {
	std::string plugin;   // "0.9.5"
	std::string updated;  // UTC, "2026-10-30T17:40:00Z"
	std::string board;    // the room board's address
	RelayConfig config;
	std::vector<RelayHealth> health;  // per config.relays, Unknown when not asked yet
	std::vector<Room> rooms;
	std::string hostingCode;   // "" when not hosting
	std::string hostingRelay;  // the relay's name, "" when friends connect straight
};
std::string StatusJson(const BridgeStatus& status);

}  // namespace rooms
