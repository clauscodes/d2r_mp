// The board's protocol (SPEC §5) as plain data: request bodies the plugin
// sends and the answers it reads. No D2RLoader or network code here, so all of
// it is covered by the unit tests.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
};

struct JoinAnswer {
	std::string address;
	int port = 4000;
	bool ipv6 = false;
	bool modMismatch = false;
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

// "Authentic 0.175.0", "Unleashed", "no VR mod", or the id itself.
std::string ModLabel(const Mod& mod);

std::optional<std::vector<Room>> ParseRooms(std::string_view body);
std::optional<JoinAnswer> ParseJoin(std::string_view body);
std::optional<Hosted> ParseCreate(std::string_view body);
// The "error" field of a refusal ("bad_password", ...), or "" when absent.
std::string ParseError(std::string_view body);

std::string CreateBody(std::string_view name, const Mod& mod, std::string_view password, int act);
std::string JoinBody(std::string_view roomId, std::string_view password, const Mod& mod);
std::string HeartbeatBody(int act, int level);

// One line per room and one for its players, as shown in the list.
std::string RoomLine(const Room& room);
std::string PlayersLine(const Room& room);

}  // namespace rooms
