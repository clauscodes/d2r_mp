#include "board.h"

#include <cctype>
#include <cstdio>

#include "json.h"

namespace rooms {

namespace {

std::string Lower(std::string_view s) {
	std::string out(s);
	for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

std::string Capital(std::string s) {
	if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
	return s;
}

Mod ReadMod(const json::Value& v) {
	Mod m;
	m.id = v["id"].asString("none");
	m.version = v["version"].asString();
	return m;
}

// A mod name as a board id: "D2R-VR" -> "d2r-vr", "My Mod_2" -> "mymod2"; "" when nothing is left.
std::string ModSlug(std::string_view name) {
	std::string id;
	for (const char c : Lower(name))
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') id += c;
	return id.substr(0, 32);
}

std::string_view Trim(std::string_view s) {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
	return s;
}

// The mod D2RLoader runs, from its "Active mod:" table ("| name | D2R-PCVR |"):
// it is there also when the player set a default mod and gave no -mod.
std::string ActiveModName(std::string_view loaderLog) {
	const size_t table = loaderLog.find("Active mod:");
	if (table == std::string_view::npos) return {};
	size_t at = table;
	for (int lines = 0; lines < 8; ++lines) {
		const size_t next = loaderLog.find('\n', at);
		if (next == std::string_view::npos) break;
		at = next + 1;
		const std::string_view line = loaderLog.substr(at, std::min(loaderLog.find('\n', at), loaderLog.size()) - at);
		const size_t bar = line.find('|');
		if (bar == std::string_view::npos) continue;
		std::string_view cells = line.substr(bar + 1);
		const size_t mid = cells.find('|');
		if (mid == std::string_view::npos || Trim(cells.substr(0, mid)) != "name") continue;
		cells = cells.substr(mid + 1);
		const std::string_view value = Trim(cells.substr(0, cells.find('|')));
		if (value.empty() || value == "null" || value.front() == '<') return {};
		return std::string(value);
	}
	return {};
}

}  // namespace

Mod DetectMod(std::string_view commandLine) {
	// Tokens split on spaces; a quoted mod name keeps its spaces.
	std::vector<std::string> tokens;
	std::string cur;
	bool quoted = false;
	for (const char c : commandLine) {
		if (c == '"') {
			quoted = !quoted;
		} else if (c == ' ' && !quoted) {
			if (!cur.empty()) tokens.push_back(std::move(cur)), cur.clear();
		} else {
			cur += c;
		}
	}
	if (!cur.empty()) tokens.push_back(std::move(cur));

	Mod mod;
	for (size_t i = 0; i + 1 < tokens.size(); ++i) {
		if (Lower(tokens[i]) != "-mod") continue;
		const std::string id = ModSlug(tokens[i + 1]);
		mod.id = id.empty() ? "none" : id;
	}
	return mod;
}

std::vector<ModPlugin> ModPlugins(std::string_view loaderLog) {
	std::vector<ModPlugin> out;
	size_t at = 0;
	while ((at = loaderLog.find("[PLUGIN]", at)) != std::string_view::npos) {
		const size_t end = std::min(loaderLog.find('\n', at), loaderLog.size());
		std::string_view line = Trim(loaderLog.substr(at + 8, end - at - 8));
		at = end;
		if (!line.ends_with("[mod]")) continue;  // [global] plugins are not the mod's
		line = Trim(line.substr(0, line.size() - 5));
		const size_t by = line.rfind(" by ");
		if (by == std::string_view::npos) continue;
		ModPlugin p;
		p.author = std::string(Trim(line.substr(by + 4)));
		line = Trim(line.substr(0, by));
		const size_t space = line.rfind(' ');
		if (space == std::string_view::npos) continue;
		const std::string_view v = line.substr(space + 1);
		if (v.empty() || v.size() > 24 || v[0] < '0' || v[0] > '9') continue;
		p.version = std::string(v);
		p.name = std::string(Trim(line.substr(0, space)));
		if (!p.name.empty()) out.push_back(std::move(p));
	}
	return out;
}

std::string VrModId(const ModPlugin& plugin) {
	const std::string name = Lower(plugin.name), author = Lower(plugin.author);
	if (name == "d2r vr" && author == "natpoh") return "d2r-vr";
	if (name == "d2r pcvr") return "d2r-pcvr";  // Unleashed's plugin since 0.3.16
	return {};
}

Mod ResolveMod(const Mod& fromCommandLine, std::string_view loaderLog) {
	const std::vector<ModPlugin> plugins = ModPlugins(loaderLog);

	// A VR mod's own plugin line names the mod, whatever the command line says;
	// with two of them, the one -mod names.
	const ModPlugin* vr = nullptr;
	for (const ModPlugin& p : plugins) {
		const std::string id = VrModId(p);
		if (id.empty()) continue;
		if (!vr || id == fromCommandLine.id) vr = &p;
		if (id == fromCommandLine.id) break;
	}
	// Unleashed before 0.3.16 still called its plugin D2R VR by natpoh: with
	// -mod D2R-PCVR that line is Unleashed's.
	if (vr) return {fromCommandLine.id == "d2r-pcvr" ? "d2r-pcvr" : VrModId(*vr), vr->version};

	// Another mod: -mod, else D2RLoader's active mod, else its only plugin's
	// name. The version only from a plugin of that same name.
	Mod mod;
	if (fromCommandLine.id != "none") mod.id = fromCommandLine.id;
	else if (const std::string active = ModSlug(ActiveModName(loaderLog)); !active.empty()) mod.id = active;
	else if (plugins.size() == 1 && !ModSlug(plugins[0].name).empty()) mod.id = ModSlug(plugins[0].name);
	for (const ModPlugin& p : plugins)
		if (ModSlug(p.name) == mod.id) mod.version = p.version;
	return mod;
}

std::string ModLabel(const Mod& mod) {
	std::string label;
	if (mod.id == "d2r-vr") label = "Authentic";
	else if (mod.id == "d2r-pcvr") label = "Unleashed";
	else if (mod.id == "none") label = "no VR mod";
	else label = mod.id;
	if (!mod.version.empty()) label += " " + mod.version;
	return label;
}

std::optional<std::vector<Room>> ParseRooms(std::string_view body) {
	const auto doc = json::Parse(body);
	if (!doc || (*doc)["rooms"].kind() != json::Value::Kind::Array) return std::nullopt;
	std::vector<Room> out;
	const json::Value& list = (*doc)["rooms"];
	for (size_t i = 0; i < list.size(); ++i) {
		const json::Value& r = list[i];
		Room room;
		room.id = r["room_id"].asString();
		room.code = r["code"].asString();
		room.name = r["name"].asString();
		room.mod = ReadMod(r["mod"]);
		room.difficulty = r["difficulty"].asString("normal");
		room.mode = r["mode"].asString("softcore");
		room.act = r["act"].asInt();
		room.maxPlayers = r["max_players"].asInt(8);
		room.playersCount = r["players_count"].asInt(1);
		room.locked = r["locked"].asBool();
		room.relayUrl = r["relay_url"].asString();
		const json::Value& players = r["players"];
		for (size_t j = 0; j < players.size(); ++j) {
			const json::Value& p = players[j];
			Player pl;
			pl.name = p["name"].asString();
			pl.cls = p["class"].asString("unknown");
			pl.level = p["level"].asInt();
			pl.act = p["act"].asInt();
			pl.area = p["area"].asString();
			pl.host = p["host"].asBool();
			if (!p["mod"].isNull()) pl.mod = ReadMod(p["mod"]);
			room.players.push_back(std::move(pl));
		}
		if (!room.id.empty()) out.push_back(std::move(room));
	}
	return out;
}

std::optional<JoinAnswer> ParseJoin(std::string_view body) {
	const auto doc = json::Parse(body);
	if (!doc) return std::nullopt;
	JoinAnswer a;
	a.address = (*doc)["address"].asString();
	a.port = (*doc)["port"].asInt(4000);
	a.ipv6 = (*doc)["address_ipv6"].asBool();
	a.modMismatch = (*doc)["mod_mismatch"].asBool();
	const json::Value& relay = (*doc)["relay"];
	if (ParseRelayUrl(relay["url"].asString()) && !relay["room"].asString().empty() && !relay["key"].asString().empty())
		a.relay = RelayRef {relay["url"].asString(), relay["room"].asString(), relay["key"].asString()};
	// A room behind a relay comes without the host's address.
	if (a.address.empty() && !a.relay) return std::nullopt;
	return a;
}

std::optional<Hosted> ParseCreate(std::string_view body) {
	const auto doc = json::Parse(body);
	if (!doc || (*doc)["host_token"].asString().empty()) return std::nullopt;
	Hosted h;
	h.roomId = (*doc)["room_id"].asString();
	h.code = (*doc)["code"].asString();
	h.token = (*doc)["host_token"].asString();
	h.address = (*doc)["address"].asString();
	h.port = (*doc)["port"].asInt(4000);
	h.ipv6 = (*doc)["address_ipv6"].asBool();
	h.heartbeatEvery = (*doc)["heartbeat_every"].asInt(60);
	if (h.heartbeatEvery < 10) h.heartbeatEvery = 10;
	return h;
}

std::string ParseError(std::string_view body) {
	const auto doc = json::Parse(body);
	return doc ? (*doc)["error"].asString() : std::string();
}

static std::string ModJson(const Mod& mod) {
	return "{\"id\":" + json::Quote(mod.id) + ",\"version\":" + json::Quote(mod.version) + "}";
}

std::string CreateBody(std::string_view name, const Mod& mod, std::string_view password, int act) {
	return CreateBody(name, mod, password, act, {});
}

std::string CreateBody(std::string_view name, const Mod& mod, std::string_view password, int act, std::string_view relayJson) {
	std::string body = "{\"v\":1,\"name\":" + json::Quote(name) + ",\"mod\":" + ModJson(mod) + ",\"public\":true";
	if (!password.empty()) body += ",\"password\":" + json::Quote(password);
	if (act >= 1 && act <= 5) body += ",\"act\":" + std::to_string(act);
	if (!relayJson.empty()) body += ",\"relay\":" + std::string(relayJson);
	return body + "}";
}

std::string JoinBody(std::string_view roomId, std::string_view password, const Mod& mod) {
	std::string body = "{\"v\":1,\"room_id\":" + json::Quote(roomId) + ",\"mod\":" + ModJson(mod);
	if (!password.empty()) body += ",\"password\":" + json::Quote(password);
	return body + "}";
}

std::string HeartbeatBody(int act, int level, std::string_view relayJson, int players) {
	std::string body = "{\"v\":1";
	if (act >= 1 && act <= 5) body += ",\"act\":" + std::to_string(act);
	if (players >= 1 && players <= 8) body += ",\"players_count\":" + std::to_string(players);
	(void)level;  // the host's own row needs a name the plugin cannot read yet
	if (!relayJson.empty()) body += ",\"relay\":" + std::string(relayJson);
	return body + "}";
}

std::string RoomLine(const Room& room) {
	std::string line = room.name + "   [" + room.code + "]";
	if (room.locked) line += "  (password)";
	line += "   " + Capital(room.difficulty);
	if (room.mode == "hardcore") line += " HC";
	if (room.act >= 1) line += "   Act " + std::to_string(room.act);
	line += "   " + std::to_string(room.playersCount) + "/" + std::to_string(room.maxPlayers);
	line += "   " + ModLabel(room.mod);
	if (!room.relayUrl.empty()) line += std::string("   relay: ") + RelayStateLabel(room.relayState);
	return line;
}

namespace {

const char* StateCode(RelayState state) {
	switch (state) {
		case RelayState::Active: return "active";
		case RelayState::NearLimit: return "near_limit";
		case RelayState::Limit: return "limit";
		case RelayState::NoAnswer: return "no_answer";
		default: return "unknown";
	}
}

}  // namespace

std::string StatusJson(const BridgeStatus& s) {
	using json::Quote;
	std::string out = "{\"plugin\":" + Quote(s.plugin) + ",\"updated\":" + Quote(s.updated) + ",\"board\":" + Quote(s.board);
	out += ",\"bridge\":{\"enabled\":" + std::string(s.config.enabled ? "true" : "false") + ",\"mode\":" + Quote(RelayModeName(s.config.mode)) +
	       ",\"use\":" + Quote(s.config.use) + "}";
	out += ",\"relays\":[";
	const std::vector<RelayServer> order = HostOrder(s.config);
	for (size_t i = 0; i < s.config.relays.size(); ++i) {
		const RelayServer& r = s.config.relays[i];
		const RelayHealth h = i < s.health.size() ? s.health[i] : RelayHealth {};
		char usage[32] = "null";
		if (h.usage >= 0) std::snprintf(usage, sizeof(usage), "%.3f", h.usage);
		out += std::string(i ? "," : "") + "{\"name\":" + Quote(r.name) + ",\"url\":" + Quote(r.url) + ",\"state\":" + Quote(StateCode(h.state)) +
		       ",\"usage\":" + usage + ",\"first\":" + (order[0].url == r.url ? "true" : "false") + "}";
	}
	out += "],\"hosting\":";
	out += s.hostingCode.empty() ? "null"
	                             : "{\"room\":" + Quote(s.hostingCode) + ",\"relay\":" + (s.hostingRelay.empty() ? "null" : Quote(s.hostingRelay)) + "}";
	out += ",\"rooms\":[";
	for (size_t i = 0; i < s.rooms.size(); ++i) {
		const RoomColumns c = Columns(s.rooms[i]);
		out += std::string(i ? "," : "") + "{\"name\":" + Quote(c.name) + ",\"game\":" + Quote(c.game) + ",\"players\":" + Quote(c.players) +
		       ",\"version\":" + Quote(c.version) + ",\"connection\":" + Quote(c.connection) + ",\"who\":" + Quote(PlayersLine(s.rooms[i])) + "}";
	}
	return out + "]}";
}

RoomColumns Columns(const Room& room) {
	RoomColumns c;
	c.name = room.name + " [" + room.code + "]" + (room.locked ? " (password)" : "");
	c.game = Capital(room.difficulty) + (room.mode == "hardcore" ? " HC" : "") + (room.act >= 1 ? ", Act " + std::to_string(room.act) : "");
	c.players = std::to_string(room.playersCount) + "/" + std::to_string(room.maxPlayers);
	c.version = ModLabel(room.mod);
	c.connection = room.relayUrl.empty() ? "direct" : std::string("relay: ") + RelayStateLabel(room.relayState);
	return c;
}

std::string PlayersLine(const Room& room) {
	std::string line;
	for (const Player& p : room.players) {
		if (!line.empty()) line += "  -  ";
		line += p.name;
		if (p.cls != "unknown" && !p.cls.empty()) line += " " + Capital(p.cls);
		if (p.level > 0) line += " " + std::to_string(p.level);
		if (p.act > 0) line += " (A" + std::to_string(p.act) + ")";
		if (p.mod && p.mod->id != room.mod.id) line += " [" + ModLabel(*p.mod) + "]";
	}
	return line;
}

}  // namespace rooms
