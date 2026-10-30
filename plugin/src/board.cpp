#include "board.h"

#include <cctype>

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
		const std::string name = Lower(tokens[i + 1]);
		if (name == "d2r-vr") mod.id = "d2r-vr";
		else if (name == "d2r-pcvr") mod.id = "d2r-pcvr";
		else {
			std::string id;
			for (const char c : name)
				if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') id += c;
			mod.id = id.empty() ? "none" : id.substr(0, 32);
		}
	}
	return mod;
}

std::string ModPluginVersion(std::string_view loaderLog) {
	std::string version;
	size_t at = 0;
	while ((at = loaderLog.find("[PLUGIN]", at)) != std::string_view::npos) {
		const size_t end = std::min(loaderLog.find('\n', at), loaderLog.size());
		std::string_view line = loaderLog.substr(at + 8, end - at - 8);
		at = end;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
		if (!line.ends_with("[mod]")) continue;
		const size_t by = line.rfind(" by ");
		if (by == std::string_view::npos) continue;
		line = line.substr(0, by);
		const size_t space = line.rfind(' ');
		const std::string_view v = space == std::string_view::npos ? line : line.substr(space + 1);
		if (!v.empty() && v.size() <= 24 && v[0] >= '0' && v[0] <= '9') version = std::string(v);
	}
	return version;
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

std::string HeartbeatBody(int act, int level, std::string_view relayJson) {
	std::string body = "{\"v\":1";
	if (act >= 1 && act <= 5) body += ",\"act\":" + std::to_string(act);
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
