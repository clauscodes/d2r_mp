// Unit tests for the parts without D2RLoader: JSON, the board protocol and the
// panel layout. Plain asserts with a report; no framework needed.

#include <cstdio>
#include <fstream>
#include <string>

#include "board.h"
#include "json.h"
#include "layout.h"
#include "relay_config.h"

namespace {

int g_failed = 0;
int g_checks = 0;

void Check(bool ok, const char* what, int line) {
	++g_checks;
	if (!ok) {
		++g_failed;
		std::printf("FAIL line %d: %s\n", line, what);
	}
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

using namespace rooms;

void TestJson() {
	auto v = json::Parse(R"({"a":1,"b":[true,null,"x\"y\\z\u00e9\ud83d\ude00"],"c":{"d":-2.5e1}})");
	CHECK(v.has_value());
	CHECK((*v)["a"].asInt() == 1);
	CHECK((*v)["b"].size() == 3);
	CHECK((*v)["b"][0].asBool());
	CHECK((*v)["b"][1].isNull());
	CHECK((*v)["b"][2].asString() == "x\"y\\z\xC3\xA9\xF0\x9F\x98\x80");
	CHECK((*v)["c"]["d"].asNumber() == -25.0);
	CHECK((*v)["missing"]["deeper"].isNull());
	CHECK((*v)["b"][99].isNull());
	CHECK(!json::Parse("{\"a\":1,}").has_value());
	CHECK(!json::Parse("{\"a\":1} x").has_value());
	CHECK(!json::Parse("\"unterminated").has_value());
	CHECK(!json::Parse("[\"\x01\"]").has_value());
	CHECK(!json::Parse(std::string(100, '[') + std::string(100, ']')).has_value());  // too deep
	CHECK(json::Quote("a\"b\\c\nd\x01") == "\"a\\\"b\\\\c\\nd\\u0001\"");
	// Quote and Parse round-trip.
	const std::string odd = "Baal \"runs\" \\ \t \xD0\x9F\xD1\x80\xD0\xB8";
	CHECK(json::Parse(json::Quote(odd))->asString() == odd);
}

void TestDetectMod() {
	CHECK(DetectMod("D2RLoader.exe").id == "none");
	CHECK(DetectMod("\"D:\\Games\\D2R\\D2RLoader.exe\" -mod D2R-VR -txt").id == "d2r-vr");
	CHECK(DetectMod("D2RLoader.exe -MOD d2r-pcvr").id == "d2r-pcvr");
	CHECK(DetectMod("D2RLoader.exe -mod \"My Mod_2\"").id == "mymod2");
	CHECK(DetectMod("D2RLoader.exe -mod").id == "none");
	CHECK(ModLabel({"d2r-vr", "0.175.0"}) == "Authentic 0.175.0");
	CHECK(ModLabel({"d2r-pcvr", ""}) == "Unleashed");
	CHECK(ModLabel({"none", ""}) == "no VR mod");
}

// d2rloader.log as D2RLoader 1.3.1 writes it, with `mod` as the active mod's
// name and `plugins` as the lines under "Loading plugins".
std::string LoaderLog(const std::string& mod, const std::string& plugins) {
	std::string log = "\xEF\xBB\xBF[2026-11-27 00:11:22.454] [INFO] D2RLoader v1.3.1-beta (Release, Release) started.\r\n"
	                  "[2026-11-27 00:11:22.505] [INFO] Active mod:\r\n"
	                  "[2026-11-27 00:11:22.506] [INFO]   +-----------------+-----------------+\r\n"
	                  "[2026-11-27 00:11:22.507] [INFO]   | Setting         | Value           |\r\n"
	                  "[2026-11-27 00:11:22.508] [INFO]   +-----------------+-----------------+\r\n"
	                  "[2026-11-27 00:11:22.509] [INFO]   | name            | " + mod + "          |\r\n"
	                  "[2026-11-27 00:11:22.511] [INFO]   | mod build       | null            |\r\n"
	                  "[2026-11-27 00:11:22.512] [INFO]   | modinfo.name    | " + mod + "          |\r\n"
	                  "[2026-11-27 00:11:22.704] [INFO] D2RCore v1.3.1-beta (Release) loaded.\r\n"
	                  "[2026-11-27 00:11:22.754] [STARTUP] \xE2\x94\x8C Loading plugins and memory patches\r\n";
	log += plugins;
	log += "[2026-11-27 00:11:24.317]           \xE2\x94\x94 [DONE]       2 plugins loaded; no memory patches.\r\n";
	return log;
}

std::string PluginLine(const std::string& text) {
	return "[2026-11-27 00:11:24.299]           \xE2\x94\x9C [PLUGIN]     " + text + "\r\n";
}

void TestResolveMod() {
	const std::string ours = PluginLine("D2R VR 0.225.0 by natpoh [mod]");
	const std::string unleashed = PluginLine("D2R PCVR 0.3.36 by pcvr [mod]");
	const std::string rooms = PluginLine("D2R Rooms 0.9.11 by D2R Rooms [global]");
	const std::string pyre = PluginLine("Floating Damage 1.3.1+rev.4 by PyreFly [mod]");
	const Mod noMod = DetectMod("\"D:\\Games\\D2R\\D2RLoader.exe\"");
	const Mod vrMod = DetectMod("D2RLoader.exe -mod D2R-VR");
	const Mod pcvrMod = DetectMod("D2RLoader.exe -mod D2R-PCVR");

	const std::vector<ModPlugin> lines = ModPlugins(LoaderLog("D2R-VR", ours + rooms + pyre));
	CHECK(lines.size() == 2);  // [global] left out
	CHECK(lines.size() == 2 && lines[0].name == "D2R VR" && lines[0].version == "0.225.0" && lines[0].author == "natpoh");
	CHECK(lines.size() == 2 && lines[1].name == "Floating Damage" && lines[1].version == "1.3.1+rev.4" && lines[1].author == "PyreFly");
	CHECK(VrModId({"D2R VR", "0.225.0", "natpoh"}) == "d2r-vr");
	CHECK(VrModId({"D2R PCVR", "0.3.36", "pcvr"}) == "d2r-pcvr");
	CHECK(VrModId({"D2R VR", "1.0", "someone"}).empty());

	// Room EKNR: Unleashed started as D2RLoader's default mod, no -mod: was "none 0.3.36".
	Mod m = ResolveMod(noMod, LoaderLog("D2R-PCVR", rooms + unleashed));
	CHECK(m.id == "d2r-pcvr" && m.version == "0.3.36");
	m = ResolveMod(noMod, LoaderLog("<none>", unleashed));  // no active-mod table name either
	CHECK(m.id == "d2r-pcvr" && m.version == "0.3.36");
	// Room ACA8: Authentic with a PyreFly plugin in its mod folder after ours: was "d2r-vr 1.3.1+rev.4".
	m = ResolveMod(vrMod, LoaderLog("D2R-VR", ours + rooms + pyre));
	CHECK(m.id == "d2r-vr" && m.version == "0.225.0");
	m = ResolveMod(vrMod, LoaderLog("D2R-VR", pyre + ours));  // before ours too
	CHECK(m.id == "d2r-vr" && m.version == "0.225.0");
	// -mod and the plugin disagree: the plugin line wins.
	m = ResolveMod(vrMod, LoaderLog("D2R-VR", unleashed));
	CHECK(m.id == "d2r-pcvr" && m.version == "0.3.36");
	m = ResolveMod(DetectMod("D2RLoader.exe -mod Other"), LoaderLog("Other", ours));
	CHECK(m.id == "d2r-vr" && m.version == "0.225.0");
	// ...except an old Unleashed (before 0.3.16) that still called itself D2R VR by natpoh.
	m = ResolveMod(pcvrMod, LoaderLog("D2R-PCVR", PluginLine("D2R VR 0.152.0 by natpoh [mod]")));
	CHECK(m.id == "d2r-pcvr" && m.version == "0.152.0");
	// Both VR plugins listed: the one -mod names.
	m = ResolveMod(vrMod, LoaderLog("D2R-VR", unleashed + ours));
	CHECK(m.id == "d2r-vr" && m.version == "0.225.0");
	m = ResolveMod(noMod, LoaderLog("D2R-VR", unleashed + ours));
	CHECK(m.id == "d2r-pcvr");  // without -mod the first listed

	// Never D2RLoader's own version or a [global] plugin's.
	m = ResolveMod(vrMod, LoaderLog("D2R-VR", rooms));
	CHECK(m.id == "d2r-vr" && m.version.empty());
	m = ResolveMod(noMod, LoaderLog("<none>", rooms));
	CHECK(m.id == "none" && m.version.empty());
	m = ResolveMod(noMod, "");
	CHECK(m.id == "none" && m.version.empty());

	// Another mod: -mod, else the active mod, else its only plugin; the version only from a plugin of its name.
	m = ResolveMod(DetectMod("D2RLoader.exe -mod \"Pyre Mod\""), LoaderLog("Pyre Mod", pyre));
	CHECK(m.id == "pyremod" && m.version.empty());
	m = ResolveMod(noMod, LoaderLog("FloatingDamage", pyre));
	CHECK(m.id == "floatingdamage" && m.version == "1.3.1+rev.4");
	m = ResolveMod(noMod, LoaderLog("<none>", pyre));
	CHECK(m.id == "floatingdamage" && m.version == "1.3.1+rev.4");
	m = ResolveMod(noMod, LoaderLog("<none>", pyre + PluginLine("Ultrawide 1.3.5+rev.3 by PyreFly [mod]")));
	CHECK(m.id == "none" && m.version.empty());  // two plugins, no name: no guess
	// Malformed lines are skipped.
	CHECK(ModPlugins(PluginLine("D2R VR by natpoh [mod]") + PluginLine("D2R VR invalid by natpoh [mod]")).empty());
}

const char* kRooms = R"({"v":1,"rooms":[
	{"room_id":"r1","code":"K7QX","name":"Baal runs","mod":{"id":"d2r-vr","version":"0.175.0"},"difficulty":"hell",
	 "mode":"softcore","act":5,"max_players":8,"players_count":2,"locked":true,"age":10,
	 "players":[{"name":"Aria","class":"paladin","level":85,"act":5,"area":"Worldstone Keep","vr":true,"host":true,"mod":{"id":"d2r-vr","version":"0.175.0"}},
	            {"name":"Bob","class":"sorceress","level":82,"act":5,"area":"","vr":null,"host":false,"mod":{"id":"d2r-pcvr","version":""}}]},
	{"room_id":"r2","code":"AB23","name":"Cows","mod":{"id":"none","version":""},"difficulty":"normal","mode":"hardcore",
	 "act":null,"max_players":4,"players_count":1,"locked":false,"players":[]},
	{"code":"NOID","name":"no id is skipped"}]})";

void TestBoard() {
	auto rooms = ParseRooms(kRooms);
	CHECK(rooms.has_value());
	CHECK(rooms->size() == 2);
	const Room& r = (*rooms)[0];
	CHECK(r.code == "K7QX" && r.locked && r.act == 5 && r.playersCount == 2);
	CHECK(r.players.size() == 2 && r.players[0].host && r.players[1].mod->id == "d2r-pcvr");
	CHECK((*rooms)[1].act == 0 && (*rooms)[1].mode == "hardcore");
	CHECK(RoomLine(r) == "Baal runs   [K7QX]  (password)   Hell   Act 5   2/8   Authentic 0.175.0");
	CHECK(RoomLine((*rooms)[1]) == "Cows   [AB23]   Normal HC   1/4   no VR mod");
	CHECK(PlayersLine(r) == "Aria Paladin 85 (A5)  -  Bob Sorceress 82 (A5) [Unleashed]");
	CHECK(!ParseRooms("{\"rooms\":5}").has_value());
	CHECK(!ParseRooms("not json").has_value());

	auto join = ParseJoin(R"({"v":1,"room_id":"r1","address":"26.1.2.3","port":4000,"address_ipv6":false,"mod_mismatch":true})");
	CHECK(join && join->address == "26.1.2.3" && join->port == 4000 && join->modMismatch && !join->ipv6);
	CHECK(!ParseJoin(R"({"v":1,"error":"bad_password"})"));
	CHECK(ParseError(R"({"v":1,"error":"bad_password"})") == "bad_password");
	CHECK(ParseError("garbage").empty());

	auto hosted = ParseCreate(R"({"v":1,"room_id":"r9","code":"ZZ22","host_token":"tok","address":"2001:db8::1","port":4000,"address_ipv6":true,"heartbeat_every":3})");
	CHECK(hosted && hosted->token == "tok" && hosted->ipv6 && hosted->heartbeatEvery == 10);  // clamped to 10 s
	CHECK(!ParseCreate(R"({"v":1,"error":"too_many_rooms"})"));

	// Bodies are valid JSON with the fields the server reads.
	auto create = json::Parse(CreateBody("Baal \"runs\"", {"d2r-vr", "0.175.0"}, "", 5));
	CHECK(create && (*create)["name"].asString() == "Baal \"runs\"" && (*create)["mod"]["id"].asString() == "d2r-vr");
	CHECK((*create)["act"].asInt() == 5 && !create->has("password") && (*create)["public"].asBool());
	CHECK(!json::Parse(CreateBody("x", {}, "pw", 0))->has("act"));
	CHECK((*json::Parse(CreateBody("x", {}, "pw", 0)))["password"].asString() == "pw");
	auto jb = json::Parse(JoinBody("r1", "pw", {"d2r-pcvr", ""}));
	CHECK(jb && (*jb)["room_id"].asString() == "r1" && (*jb)["password"].asString() == "pw");
	CHECK(!json::Parse(JoinBody("r1", "", {}))->has("password"));
	CHECK(json::Parse(HeartbeatBody(3, 40)).has_value());
	CHECK((*json::Parse(HeartbeatBody(3, 40)))["act"].asInt() == 3);
	CHECK(!json::Parse(HeartbeatBody(0, 0))->has("act"));
}

void TestLayout() {
	CHECK(PageCount(0) == 1 && PageCount(5) == 1 && PageCount(6) == 2);
	CHECK(FieldText("@strHack") == " @strHack" && FieldText("$Style") == " $Style" && FieldText("ok") == "ok");

	PanelView view;
	view.rooms = *ParseRooms(kRooms);
	for (int i = 0; i < 6; ++i) view.rooms.push_back(view.rooms[1]);  // 8 rooms -> 2 pages of 5
	view.status = "8 open rooms";

	auto layout = json::Parse(PanelLayout(view));
	CHECK(layout.has_value());
	CHECK((*layout)["name"].asString() == PanelName(0));
	const json::Value& kids = (*layout)["children"];
	int joins = 0;
	bool next = false, prev = false, input = false;
	std::string firstRoom;
	for (size_t i = 0; i < kids.size(); ++i) {
		const std::string name = kids[i]["name"].asString();
		if (name.rfind("Join", 0) == 0) {
			++joins;
			CHECK(!kids[i]["fields"].has("onClickMessage"));  // D2RLoader asserts on an unknown target
		}
		if (name == "Next") next = true;
		if (name == "Prev") prev = true;
		if (name == "RoomName") input = kids[i]["type"].asString() == "InputTextBoxWidget";
		if (name == "Room0") firstRoom = kids[i]["fields"]["text"].asString();
	}
	CHECK(joins == kRowsPerPage && next && !prev && input);
	CHECK(firstRoom == RoomLine(view.rooms[0]));

	view.page = 5;  // clamped to the last page: 3 rooms there, Back but no More
	layout = json::Parse(PanelLayout(view));
	joins = 0;
	next = prev = false;
	for (size_t i = 0; i < (*layout)["children"].size(); ++i) {
		const std::string name = (*layout)["children"][i]["name"].asString();
		if (name.rfind("Join", 0) == 0) ++joins;
		if (name == "Next") next = true;
		if (name == "Prev") prev = true;
	}
	CHECK(joins == 3 && prev && !next);

	// A hostile room name stays one JSON string and cannot become a reference.
	view = {};
	Room evil;
	evil.id = "x";
	evil.code = "EV11";
	evil.name = "@strCreateServer\",\"onClickMessage\":\"FrontEndNavigation:ToMainMenu";
	view.rooms.push_back(evil);
	layout = json::Parse(PanelLayout(view));
	CHECK(layout.has_value());
	bool found = false;
	for (size_t i = 0; i < (*layout)["children"].size(); ++i) {
		const json::Value& c = (*layout)["children"][i];
		if (c["name"].asString() == "Room0") {
			found = true;
			CHECK(c["fields"]["text"].asString()[0] == ' ');
			CHECK(!c["fields"].has("onClickMessage"));
		}
	}
	CHECK(found);

	// While hosting the create form gives way to the hosting text.
	view.hosting = true;
	view.hostingText = "You host room [K7QX].";
	layout = json::Parse(PanelLayout(view));
	bool hostingShown = false, formShown = false;
	for (size_t i = 0; i < (*layout)["children"].size(); ++i) {
		const std::string name = (*layout)["children"][i]["name"].asString();
		hostingShown |= name == "CloseRoom";
		formShown |= name == "RoomName" || name == "Create";
	}
	CHECK(hostingShown && !formShown);

	// The D2R Rooms window in place of D2RLoader's: its CREATE GAME hosts, so
	// only the password field is left; the three tabs; no empty texts.
	view.hosting = false;
	view.tab = true;
	view.rooms[0].players.clear();
	layout = json::Parse(PanelLayout(view));
	CHECK(layout.has_value());
	bool passwordShown = false, createShown = false, tabs = false, emptyText = false;
	size_t joins2 = 0;
	for (size_t i = 0; i < (*layout)["children"].size(); ++i) {
		const json::Value& c = (*layout)["children"][i];
		const std::string name = c["name"].asString();
		passwordShown |= name == "RoomName";
		createShown |= name == "Create";
		tabs |= name == "HostTab" || name == "BrowseTab";
		joins2 += name.rfind("Join", 0) == 0;
		emptyText |= c["type"].asString() == "TextBoxWidget" && c["fields"]["text"].asString().empty();
	}
	CHECK(!passwordShown && !createShown && tabs);
	CHECK(joins2 == std::min<size_t>(view.rooms.size(), kRowsPerPage));
	CHECK(!emptyText);
	// About D2RLoader's window's width (its MainTabs are 1730), centred; named by its pool slot.
	CHECK((*layout)["fields"]["rect"]["width"].asInt() == 1800 && (*layout)["fields"]["rect"]["x"].asInt() == -900);
	view.slot = 9;
	CHECK((*json::Parse(PanelLayout(view)))["name"].asString() == "d2r-rooms/Rooms9" && PanelPath(9) == "data/global/ui/layouts/d2r-rooms/Rooms9hd.json");
	view.slot = 0;

	// Clicks are matched by the plugin: the close button, each Join, Refresh,
	// Host Game and Browse Games, all inside the window's art (the close
	// button's area a little past its corner).
	size_t joinAreas = 0;
	bool close = false, refresh = false, host = false, browse = false, inside = true;
	for (const HitArea& a : WindowHitAreas(view)) {
		joinAreas += a.command == "Join";
		close |= a.command == "Close";
		refresh |= a.command == "Refresh";
		host |= a.command == "ToHost";
		browse |= a.command == "ToBrowse";
		const int margin = a.command == "Close" ? 40 : 0;
		inside &= a.x >= -900 && a.x + a.width <= 900 && a.y >= 216 - margin && a.y + a.height <= 216 + 1500;
	}
	CHECK(joinAreas == std::min<size_t>(view.rooms.size(), kRowsPerPage) && close && refresh && host && browse);
	CHECK(inside);

	// The title carries the layout's number: the plugin checks D2R shows the latest.
	view.generation = 7;
	layout = json::Parse(PanelLayout(view));
	bool title7 = false;
	for (size_t i = 0; i < (*layout)["children"].size(); ++i) title7 |= (*layout)["children"][i]["name"].asString() == "Title7";
	CHECK(title7);

	// The tab button is a panel of its own, named as the plugin registers it,
	// and the plugin matches clicks on it itself.
	const auto tab = json::Parse(TabLayout());
	CHECK(tab && (*tab)["name"].asString() == kTabPanelName && (*tab)["children"][0]["name"].asString() == "Tab");
	CHECK(TabHitAreas().size() == 1 && TabHitAreas()[0].command == "Tab");
}

// The wide D2R Rooms window (Serg, 2026-11-27: "can we make it wider"): about
// D2RLoader's window's width, the rooms as a table, the status line inside.
void TestWideWindow() {
	PanelView view;
	view.tab = true;
	view.status = "No open rooms right now - Relay: D2R Rooms active - the bridge is enabled (mode auto)";
	view.rooms = *ParseRooms(R"({"rooms":[
		{"room_id":"a","code":"EL9A","name":"Baal runs with a long name","mod":{"id":"d2r-vr","version":"0.225.0"},"difficulty":"hell",
		 "mode":"hardcore","act":5,"max_players":8,"players_count":3,"locked":true,
		 "players":[{"name":"Kaela","class":"paladin","level":92,"act":5,"host":true},{"name":"Ama","class":"amazon","level":88,"act":5}]},
		{"room_id":"b","code":"K7QX","name":"cows","mod":{"id":"d2r-pcvr","version":"0.3.36"},"difficulty":"normal","mode":"softcore",
		 "act":1,"max_players":8,"players_count":1,"locked":false,"players":[],"relay_url":"wss://r.example.net"}]})");
	const auto layout = json::Parse(PanelLayout(view));
	CHECK(layout.has_value());
	const int width = (*layout)["fields"]["rect"]["width"].asInt(), height = (*layout)["fields"]["rect"]["height"].asInt();
	CHECK(width >= 1730 && width <= 1920 && height <= 1600);
	// Every piece inside the window; the status line almost as wide as it; one cell per column a room.
	bool inside = true, statusWide = false;
	int names = 0, games = 0, mods = 0, players = 0, joins = 0, heads = 0;
	for (size_t i = 0; i < (*layout)["children"].size(); ++i) {
		const json::Value& c = (*layout)["children"][i];
		const std::string name = c["name"].asString();
		const json::Value& r = c["fields"]["rect"];
		if (r.kind() == json::Value::Kind::Object) {
			const int x = r["x"].asInt(), y = r["y"].asInt(), w = r["width"].asInt(), h = r["height"].asInt();
			inside &= x >= 0 && y >= 0 && x + w <= width && y + h <= height;
			if (name == "Status") statusWide = w >= width - 200;
		}
		names += name.rfind("Room", 0) == 0;
		games += name.rfind("Game", 0) == 0;
		mods += name.rfind("Mod", 0) == 0;
		players += name.rfind("Players", 0) == 0;
		joins += name.rfind("Join", 0) == 0;
		heads += name.rfind("Head", 0) == 0;
	}
	CHECK(inside && statusWide);
	CHECK(names == 2 && games == 2 && mods == 2 && players == 2 && joins == 2 && heads >= 4);
	// The plugin's hit areas do not overlap, and Join is right of the table's columns.
	const std::vector<HitArea> areas = WindowHitAreas(view);
	bool overlap = false;
	for (size_t i = 0; i < areas.size(); ++i)
		for (size_t j = i + 1; j < areas.size(); ++j) {
			const HitArea &a = areas[i], &b = areas[j];
			overlap |= a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
		}
	CHECK(!overlap && areas.size() == 2 + 1 + 3);  // two Joins, Close, Host Game / Browse Games / Refresh
	for (const HitArea& a : areas)
		if (a.command == "Join") CHECK(a.x - (-width / 2) >= 1180 + 140);
	// A full page: five rows above the bottom rule.
	view.rooms.resize(kRowsPerPage, view.rooms[0]);
	for (const HitArea& a : WindowHitAreas(view))
		if (a.command == "Join") CHECK(a.y + a.height <= 216 + 1085 - 30);
}

// Clicks on the tab button: its middle is 695,238 in layout units.
void TestToLayout() {
	const auto near = [](LayoutPoint p, int x, int y) { return std::abs(p.x - x) <= 8 && std::abs(p.y - y) <= 8; };
	const auto onTab = [](LayoutPoint p) {
		const HitArea a = TabHitAreas()[0];
		return p.x >= a.x && p.x < a.x + a.width && p.y >= a.y && p.y < a.y + a.height;
	};
	// A 16:9 window, the picture its own size: as before.
	CHECK(near(ToLayout(960 + 695 * 0.5, 238 * 0.5, 1920, 1080, 0, 0), 695, 238));
	CHECK(near(ToLayout(960 + 695 * 0.5, 238 * 0.5, 1920, 1080, 1920, 1080), 695, 238));
	// A player's flat game: 1366x768 in a 2538x1323 window (his clicks on the
	// button: 1512,363 in the window).
	CHECK(onTab(ToLayout(1512, 363, 2538, 1323, 1366, 768)));
	// D2R VR in the square 1920x1920 window, a 2734x1538 picture (clicks at 1455,367).
	CHECK(onTab(ToLayout(1455, 367, 1920, 1920, 2734, 1538)));
	// The same clicks measured against the window alone missed the button.
	CHECK(!onTab(ToLayout(1512, 363, 2538, 1323, 0, 0)) && !onTab(ToLayout(1455, 367, 1920, 1920, 0, 0)));
	// A square picture the window's size: the interface is the picture's width,
	// centred on its height.
	CHECK(near(ToLayout(960 + 695 * 0.5, 420 + 238 * 0.5, 1920, 1920, 0, 0), 695, 238));

	// Learning from D2RLoader's own presses. D2R VR on the 1920x1920 window
	// (the player's log, 2026-10-31): its picture, narrower than 16:9 and
	// wider than the window, is stretched over the window, so the interface
	// spans the window's width (0.5 px a unit across) and is 0.682 px a unit
	// high, centred.
	const ClickMap vr {0.5, 0.682, 960, 960 - 1080 * 0.682};
	const auto press = [&](const char* command, int dx, int dy) {
		const LayoutPoint t = *LoaderTarget(command);
		const PixelPoint p = FromLayout(vr, t.x + dx, t.y + dy);
		return ClickSample {p.x, p.y, t.x, t.y};
	};
	const ClickMap guess = PictureMap(1920, 1920, 0, 0);
	const PixelPoint button = FromLayout(vr, 695, 238);
	CHECK(!onTab(ToLayout(guess, button.x, button.y)));
	CHECK(near(ToLayout(LearnMap(guess, 1920, 1920, {}), button.x, button.y), ToLayout(guess, button.x, button.y).x,
	           ToLayout(guess, button.x, button.y).y));
	// The player's own presses: BROWSE GAMES at 1031,457 and HOST GAME at
	// 928,464 (his d2r-rooms-clicks.txt), one row: they cannot tell the top
	// from the scale, so the guess stays (0.9.10 took the window's middle row
	// for the interface's and drifted on a 16:9 band at the top).
	CHECK(LearnMap(guess, 1920, 1920, {{1031, 457, 114, 342}, {928, 464, -125, 342}}).ky == guess.ky);
	// With a CREATE GAME press as well, then his clicks aimed at the D2R Rooms
	// button, which 0.9.9 (one scale for both axes) put left of it.
	ClickMap learned = LearnMap(guess, 1920, 1920, {{1031, 457, 114, 342}, {928, 464, -125, 342}, press("CreateGame", 0, 0)});
	CHECK(std::abs(learned.kx - 0.5) < 0.001 && std::abs(learned.ky - 0.682) < 0.01 && std::abs(learned.top - 224) < 10);
	const std::vector<std::pair<int, int>> aimed {{1259, 364}, {1246, 375}, {1237, 376}, {1229, 396}, {1223, 386}, {1262, 375},
	                                              {1253, 392}, {1260, 366}, {1250, 377}, {1201, 368}, {1371, 376}};
	for (const auto& [px, py] : aimed) CHECK(onTab(ToLayout(learned, px, py)));
	// ...and a press on the middle of the D2R Rooms window's close button and Refresh.
	const PanelView shown;
	const auto hit = [&](const char* command) {
		for (const HitArea& target : WindowHitAreas(shown)) {
			if (target.command != command) continue;
			const PixelPoint px = FromLayout(learned, target.x + target.width / 2.0, target.y + target.height / 2.0);
			const LayoutPoint p = ToLayout(learned, px.x, px.y);
			for (const HitArea& a : WindowHitAreas(shown))
				if (p.x >= a.x && p.x < a.x + a.width && p.y >= a.y && p.y < a.y + a.height) return a.command == command;
		}
		return false;
	};
	CHECK(hit("Close") && hit("Refresh"));
	// One press on BROWSE GAMES: one row, the guess stays.
	learned = LearnMap(guess, 1920, 1920, {press("ShowBrowse", 80, -40)});
	CHECK(learned.kx == guess.kx && learned.ky == guess.ky && learned.top == guess.top);
	// HOST GAME and CREATE GAME: two heights.
	learned = LearnMap(guess, 1920, 1920, {press("ShowHost", -60, 30), press("CreateGame", 100, 20)});
	CHECK(std::abs(learned.kx - 0.5) < 0.001 && std::abs(learned.ky - 0.682) < 0.05 && onTab(ToLayout(learned, button.x, button.y)));
	// ...and with JOIN (Direct Join's Join Game) too, a press off by a tab's height.
	learned = LearnMap(guess, 1920, 1920, {press("ShowHost", -60, 30), press("CreateGame", 100, 20), press("JoinDirect", 0, 60), press("ShowBrowse", 0, 0)});
	CHECK(std::abs(learned.ky - 0.682) < 0.05 && onTab(ToLayout(learned, button.x, button.y)));
	// The flat 1366x768 game in its 2538x1323 window: two rows keep the right guess right.
	const ClickMap flat = PictureMap(2538, 1323, 1366, 768);
	const LayoutPoint host = *LoaderTarget("ShowHost"), create = *LoaderTarget("CreateGame");
	const PixelPoint hp = FromLayout(flat, host.x, host.y + 20), cp = FromLayout(flat, create.x, create.y - 10);
	learned = LearnMap(flat, 2538, 1323, {{hp.x, hp.y, host.x, host.y}, {cp.x, cp.y, create.x, create.y}});
	CHECK(std::abs(learned.kx - flat.kx) < 0.03 && std::abs(learned.ky - flat.ky) < 0.03 && onTab(ToLayout(learned, 1512, 363)));
	// A flat 16:9 window: presses off the buttons' middles keep one scale for both axes.
	const ClickMap wide = PictureMap(1920, 1080, 0, 0);
	const PixelPoint bp = FromLayout(wide, 114 + 60, 342 - 30), wp = FromLayout(wide, create.x, create.y + 25);
	learned = LearnMap(wide, 1920, 1080, {{bp.x, bp.y, 114, 342}, {wp.x, wp.y, create.x, create.y}});
	CHECK(std::abs(learned.kx - learned.ky) < 0.001 && std::abs(learned.kx - 0.5) < 0.05);
	// Stray presses (both on the same pixel) leave the guess.
	CHECK(LearnMap(guess, 1920, 1920, {{960, 960, 114, 342}, {960, 960, create.x, create.y}}).ky == guess.ky);
	// The window's own clicks go back to the same pixel.
	const PixelPoint back = FromLayout(learned, 300, 900);
	CHECK(near(ToLayout(learned, back.x, back.y), 300, 900));
}

// D2R VR's main menu laid out as a 16:9 band (Serg's PC, 2026-11-27: the
// 1280x1280 window, the game's client-size query answering 1280x720): the
// interface is 0.333 px a unit both ways from the window's top.
void TestBand() {
	CHECK(IsBand(1280, 1280, 1280, 720));
	CHECK(IsBand(2496, 2688, 2496, 1404));
	CHECK(!IsBand(1280, 1280, 1280, 1280));  // the square menu
	CHECK(!IsBand(1920, 1080, 1920, 1080));  // a flat 16:9 game
	CHECK(!IsBand(1280, 1280, 0, 0));        // no answer
	CHECK(IsBand(1280, 1280, 1920, 1080));   // in the game's pixels, bigger than the window (Serg 13:13)
	CHECK(IsBand(1280, 1280, 1000, 562));    // smaller: still a band by its shape
	CHECK(!IsBand(2560, 1080, 1920, 1080));  // an ultrawide picture: narrower than it
	CHECK(!IsBand(1280, 1280, 1280, 1000));  // not 16:9

	const ClickMap band = BandMap(1280, 1280, 1280, 1280, 1280, 720);
	CHECK(std::abs(band.kx - 1.0 / 3) < 1e-6 && std::abs(band.ky - 1.0 / 3) < 1e-6 && band.cx == 640 && std::abs(band.top) < 1e-6);
	CHECK(BandMap(1280, 1280, 0, 0, 1280, 720).ky == band.ky);  // no picture size: the window's
	// A picture larger than the window, stretched over it.
	const ClickMap big = BandMap(1280, 1280, 2496, 2688, 2496, 1404);
	CHECK(std::abs(big.kx - 1.0 / 3) < 1e-3 && std::abs(big.ky - 0.65 * 1280 / 2688) < 1e-3 && std::abs(big.top) < 1e-6);

	// The tab, Join and Refresh where the band draws them.
	const auto hits = [&](const std::vector<HitArea>& areas, int lx, int ly, const char* command) {
		const PixelPoint px = FromLayout(band, lx, ly);
		const LayoutPoint p = ToLayout(band, px.x, px.y);
		for (const HitArea& a : areas)
			if (p.x >= a.x && p.x < a.x + a.width && p.y >= a.y && p.y < a.y + a.height) return a.command == command;
		return false;
	};
	const HitArea tab = TabHitAreas()[0];
	CHECK(hits(TabHitAreas(), tab.x + tab.width / 2, tab.y + tab.height / 2, "Tab"));
	PanelView view;
	view.rooms.resize(2);
	for (const HitArea& a : WindowHitAreas(view))
		if (a.command == "Join" || a.command == "Refresh") CHECK(hits(WindowHitAreas(view), a.x + a.width / 2, a.y + a.height / 2, a.command.c_str()));
	// The square map 0.9.10 had learned for the same window (0.333 x 0.514, top 85) put the band's tab press elsewhere.
	const ClickMap square {1.0 / 3, 0.514, 640, 85};
	const PixelPoint tabPx = FromLayout(band, tab.x + tab.width / 2, tab.y + tab.height / 2);
	const LayoutPoint wrong = ToLayout(square, tabPx.x, tabPx.y);
	CHECK(!(wrong.y >= tab.y && wrong.y < tab.y + tab.height));

	// Presses on the band's tab row alone (HOST GAME, BROWSE GAMES) no longer
	// drift the vertical scale (0.9.10: 0.542, then 0.648, top -60).
	const ClickMap guess = PictureMap(1280, 1280, 1280, 1280);
	const auto bandPress = [&](const char* command, int dx, int dy) {
		const LayoutPoint t = *LoaderTarget(command);
		const PixelPoint p = FromLayout(band, t.x + dx, t.y + dy);
		return ClickSample {p.x, p.y, t.x, t.y};
	};
	const ClickMap rowOnly = LearnMap(guess, 1280, 1280, {bandPress("ShowHost", 20, 25), bandPress("ShowBrowse", -30, -20), bandPress("ShowHost", 0, 40)});
	CHECK(rowOnly.ky == guess.ky && rowOnly.top == guess.top);
	// Two rows find the band even without the query's answer (Unleashed answers only the game's own callers).
	const ClickMap twoRows = LearnMap(guess, 1280, 1280, {bandPress("ShowHost", 20, 25), bandPress("CreateGame", 0, -15), bandPress("ShowBrowse", 0, 0)});
	CHECK(std::abs(twoRows.ky - 1.0 / 3) < 0.02 && std::abs(twoRows.top) < 15 && std::abs(twoRows.kx - 1.0 / 3) < 0.02);

	// The presses are kept per window, picture, query answer and safe area.
	const MapKey squareKey {1280, 1280, 1280, 1280, 1280, 1280, 100}, bandKey {1280, 1280, 1280, 1280, 1280, 720, 100};
	const MapKey squareAgain {1280, 1280, 1280, 1280, 1280, 1280, 100}, squareSafe80 {1280, 1280, 1280, 1280, 1280, 1280, 80};
	CHECK(!(squareKey == bandKey) && squareKey == squareAgain && !(squareKey == squareSafe80));
	const std::vector<KeyedSample> kept {{squareKey, {928.5, 464, -125, 342}}, {bandKey, {600, 114, -6, 1510}}, {squareSafe80, {1, 2, 3, 4}}};
	const std::vector<KeyedSample> read = ParseClickSamples(ClickSamplesText(kept));
	CHECK(read.size() == 3 && read[0].key == squareKey && read[1].key == bandKey && read[2].key == squareSafe80 && read[0].sample.px == 928.5 &&
	      read[1].sample.y == 1510);
	// 0.9.12's lines (the window alone), 0.9.13's (no safe area) and junk are dropped.
	CHECK(ParseClickSamples("1280 1280 928 464 -125 342\r\n1920 1920 1031 457 114 342\n").empty());
	CHECK(ParseClickSamples("1920 1920 1920 1920 1920 1080 1028 243 114 342\n").empty());
	CHECK(ParseClickSamples("x y\n0 0 0 0 0 0 100 1 2 3 4\n1280 1280 0 0 0 0 100 1 2 3 4\r\n").size() == 1);
}

// The game's "Safe Screen Percent" (Serg's Settings.json, 2026-11-27: 80) draws
// the whole interface that much smaller round the middle. 0.9.13 left it out:
// on his 1920x1920 window with the 1920x1080 band it hit-tested at 0.5 px a
// unit from the top, while the game drew at 0.4 from 108 px, and the D2R Rooms
// tab missed.
void TestSafeArea() {
	const std::string serg = "{\"Screen Resolution (Windowed)\": \"1920x1920\", \"Safe Screen Percent\": 80, \"Window Mode\": 0}";
	const std::string none = "{\"Window Mode\": 0}", low = "{\"Safe Screen Percent\": 10}", high = "{\"Safe Screen Percent\": 150}";
	CHECK(SafeShare(serg) == 0.8);
	CHECK(SafeShare(none) == 1.0);
	CHECK(SafeShare("not json") == 1.0);
	CHECK(SafeShare(low) == 0.5);
	CHECK(SafeShare(high) == 1.0);

	const ClickMap band = BandMap(1920, 1920, 1920, 1920, 1920, 1080, 0.8);
	CHECK(std::abs(band.kx - 0.4) < 1e-9 && std::abs(band.ky - 0.4) < 1e-9 && band.cx == 960 && std::abs(band.top - 108) < 1e-9);
	CHECK(BandMap(1920, 1920, 1920, 1920, 1920, 1080).ky == 0.5);  // 100 %: as 0.9.13
	const ClickMap pic = PictureMap(1920, 1080, 0, 0, 0.8);
	CHECK(std::abs(pic.kx - 0.4) < 1e-9 && std::abs(pic.top - (540 - 1080 * 0.4)) < 1e-9);

	// His log, 11:14-11:16: every click he aimed at the D2R Rooms tab is on it.
	const HitArea tab = TabHitAreas()[0];
	const auto onTab = [&](const ClickMap& map, double px, double py) {
		const LayoutPoint p = ToLayout(map, px, py);
		return p.x >= tab.x && p.x < tab.x + tab.width && p.y >= tab.y && p.y < tab.y + tab.height;
	};
	const std::vector<std::pair<int, int>> aimed {{1255, 204}, {1248, 206}, {1217, 206}, {1256, 201}, {1259, 199}, {1267, 190}, {1252, 200}, {1244, 208},
	                                              {1226, 207}, {1231, 213}, {1227, 220}, {1224, 205}, {1213, 197}, {1209, 205}, {1198, 205}};
	int missed = 0;
	for (const auto& [px, py] : aimed) {
		CHECK(onTab(band, px, py));
		missed += !onTab(BandMap(1920, 1920, 1920, 1920, 1920, 1080), px, py);
	}
	CHECK(missed == static_cast<int>(aimed.size()));  // 0.9.13's map missed them all
	// ...and D2RLoader's own HOST GAME / BROWSE GAMES presses, which the game
	// took, are on those tabs (MainTabs 282..403; a tab about 230 wide).
	const std::vector<std::pair<int, int>> host {{923, 226}, {923, 228}, {934, 233}}, browse {{979, 258}, {1028, 243}, {1013, 263}};
	for (const auto& [px, py] : host) {
		const LayoutPoint p = ToLayout(band, px, py);
		CHECK(p.y >= 282 && p.y < 403 && std::abs(p.x - LoaderTarget("ShowHost")->x) < 115);
	}
	for (const auto& [px, py] : browse) {
		const LayoutPoint p = ToLayout(band, px, py);
		CHECK(p.y >= 282 && p.y < 403 && std::abs(p.x - LoaderTarget("ShowBrowse")->x) < 115);
	}
}

// Serg's PC, 13:13 (0.9.14): the 1280x1280 window, the game answering
// 1920x1080 (its 1920 render size's band), the overlay 1280x1280. 0.9.14 took
// the answer for no band (not as wide as the 1280 picture) and centred the
// interface on the square: every click came out at y -500 .. -900.
void TestBandBiggerThanWindow() {
	const ClickMap band = BandMap(1280, 1280, 1280, 1280, 1920, 1080, 0.8);
	CHECK(std::abs(band.kx - 0.8 / 3) < 1e-9 && std::abs(band.ky - 0.8 / 3) < 1e-9 && band.cx == 640 && std::abs(band.top - 72) < 1e-9);
	const HitArea tab = TabHitAreas()[0];
	const auto onTab = [&](const ClickMap& map, double px, double py) {
		const LayoutPoint p = ToLayout(map, px, py);
		return p.x >= tab.x && p.x < tab.x + tab.width && p.y >= tab.y && p.y < tab.y + tab.height;
	};
	// Every click he aimed at the D2R Rooms tab (13:13:21 - 13:13:40).
	const std::vector<std::pair<int, int>> aimed {{821, 136}, {822, 131}, {820, 134}, {821, 139}, {823, 143}, {822, 143},
	                                              {821, 145}, {818, 152}, {820, 151}, {843, 145}, {849, 140}};
	const ClickMap old = PictureMap(1280, 1280, 1280, 1280, 0.8);  // what 0.9.14 used
	for (const auto& [px, py] : aimed) CHECK(onTab(band, px, py) && !onTab(old, px, py));
	// D2RLoader's own close button took 888,115 (its corner, right of the tabs' row, above it).
	const LayoutPoint close = ToLayout(band, 888, 115);
	CHECK(close.x > 865 && close.y < 282);
}

// A VR mod's block (ui_band.h): the band in window pixels, at the click.
void TestSharedBand() {
	UiBandBlock b {};
	b.magic = kUiBandMagic;
	b.size = sizeof(UiBandBlock);
	b.seq = 2;
	b.active = 1;
	b.left = 0;
	b.top = 0;
	b.width = 1280;
	b.height = 720;
	b.canvasW = 1920;
	b.canvasH = 1080;
	CHECK(CheckUiBand(b).has_value());
	// The same map as the game's answer gives on that window.
	const ClickMap shared = SharedBandMap(b, 0.8), game = BandMap(1280, 1280, 1280, 1280, 1920, 1080, 0.8);
	CHECK(std::abs(shared.kx - game.kx) < 1e-9 && std::abs(shared.ky - game.ky) < 1e-9 && shared.cx == game.cx && std::abs(shared.top - game.top) < 1e-9);
	// Its own safe share wins over Settings.json's.
	b.safe = 1.0f;
	CHECK(std::abs(SharedBandMap(b, 0.8).ky - 1.0 / 3) < 1e-6 && std::abs(SharedBandMap(b, 0.8).top) < 1e-6);
	b.safe = 0;
	// No canvas size: the rect's own; a rect lower in the window and stretched (a 2496x2688 picture in a square window).
	UiBandBlock own = b;
	own.canvasW = own.canvasH = 0;
	CHECK(std::abs(SharedBandMap(own, 1.0).ky - 1.0 / 3) < 1e-9);
	UiBandBlock stretched = b;
	stretched.top = 100;
	stretched.width = 1280;
	stretched.height = 669;  // 1404 rows of 2688 in 1280
	stretched.canvasW = 2496;
	stretched.canvasH = 1404;
	const ClickMap s = SharedBandMap(stretched, 1.0);
	CHECK(std::abs(s.kx - 1280.0 / 3840) < 1e-3 && std::abs(s.ky - 0.65 * 669 / 1404) < 1e-3 && std::abs(s.top - (100 + 669 / 2.0 - 1080 * s.ky)) < 1e-9);
	// Torn, half-written or foreign blocks are not used; an inactive one is (it says "no band").
	UiBandBlock bad = b;
	bad.seq = 3;
	CHECK(!CheckUiBand(bad));
	bad = b;
	bad.magic = 0;
	CHECK(!CheckUiBand(bad));
	bad = b;
	bad.size = 40;
	CHECK(!CheckUiBand(bad));
	bad = b;
	bad.width = 0;
	CHECK(!CheckUiBand(bad));
	UiBandBlock off = b;
	off.active = 0;
	off.width = off.height = 0;
	CHECK(CheckUiBand(off).has_value() && !CheckUiBand(off)->active);
}

// The game's client-size query found in D2R.exe's code, as it is and with a
// VR mod's hook on its first bytes.
void TestClientSizeQuery() {
	const std::string body("\x48\x83\xEC\x38\xE8\x97\xD1\x2A\x00\x48\x8D\x54\x24\x20\x48\x8B\xC8\xE8\x11\x22\x33\x44\x8B\x44\x24\x28\x89\x44\x24\x40", 30);
	CHECK(body.size() == 30);
	const std::string pad(37, '\xCC');
	CHECK((FindClientSizeQuery(pad + body + pad) == std::vector<size_t> {37}));
	std::string hooked = body;
	hooked.replace(0, 5, "\xE9\x01\x02\x03\x04");  // jmp rel32
	CHECK((FindClientSizeQuery(pad + hooked + pad) == std::vector<size_t> {37}));
	hooked = body;
	hooked.replace(0, 14, std::string("\xFF\x25\x00\x00\x00\x00", 6) + "\x10\x20\x30\x40\x50\x60\x70\x7F");  // jmp [rip]
	CHECK((FindClientSizeQuery(pad + hooked + pad) == std::vector<size_t> {37}));
	// Two copies: both reported (the plugin then uses neither).
	CHECK(FindClientSizeQuery(pad + body + pad + body).size() == 2);
	// Something else at the start, or a different tail: not it.
	std::string other = body;
	other[0] = '\x55';
	CHECK(FindClientSizeQuery(pad + other + pad).empty());
	other = body;
	other[25] = '\x2C';
	CHECK(FindClientSizeQuery(pad + other + pad).empty());
	CHECK(FindClientSizeQuery(body.substr(0, 29)).empty());
	CHECK(FindClientSizeQuery("").empty());
}

void TestPlayerCount() {
	CHECK(HeartbeatBody(1, 0).find("players_count") == std::string::npos);
	const auto hb = json::Parse(HeartbeatBody(2, 0, {}, 3));
	CHECK(hb && (*hb)["players_count"].asInt() == 3 && (*hb)["act"].asInt() == 2);
	CHECK(HeartbeatBody(1, 0, {}, 9).find("players_count") == std::string::npos);
}

void TestRelay() {
	// The relay list: ours alone without entries, the player's order, keys, bad lines skipped.
	auto cfg = ParseRelayIni("");
	CHECK(cfg.mode == RelayMode::Auto && cfg.relays.size() == 1 && cfg.relays[0].url == kDefaultRelayUrl);
	cfg = ParseRelayIni(DefaultRelayIni());
	CHECK(cfg.relays.size() == 1 && cfg.relays[0].name == kDefaultRelayName && cfg.relays[0].key.empty());
	cfg = ParseRelayIni(
		"[relay]\r\nmode = Always\r\n[relays]\r\n; comment\r\nFriends = wss://relay.friends.net/ secret-key-123\r\n"
		"Bad = https://nope.example\r\nOurs = wss://d2r-rooms-relay.d2r-rooms.workers.dev\r\nAgain = wss://relay.friends.net\r\n"
		"Local = ws://127.0.0.1:8080\r\n");
	CHECK(cfg.mode == RelayMode::Always);
	CHECK(cfg.relays.size() == 3);
	CHECK(cfg.relays[0].name == "Friends" && cfg.relays[0].url == "wss://relay.friends.net" && cfg.relays[0].key == "secret-key-123");
	CHECK(cfg.relays[2].url == "ws://127.0.0.1:8080");
	CHECK(ParseRelayIni("[relay]\nmode=never\n").mode == RelayMode::Never);

	// [board] url: a group's own rooms server (docs/SELF_HOST.md); empty or bad = ours.
	CHECK(ParseRelayIni(DefaultRelayIni()).boardUrl.empty());
	CHECK(ParseRelayIni("").boardUrl.empty());
	CHECK(ParseRelayIni("[board]\r\nURL = https://rooms.friends.workers.dev/ \r\n").boardUrl == "https://rooms.friends.workers.dev");
	CHECK(ParseRelayIni("[board]\nurl = http://rooms.friends.net\n").boardUrl.empty());
	CHECK(ParseRelayIni("[board]\nurl = https://\n").boardUrl.empty());
	CHECK(ParseRelayIni("[board]\nurl = https://a.net b\n").boardUrl.empty());
	CHECK(ParseRelayIni("[relay]\nurl = https://a.net\n").boardUrl.empty());
	CHECK(ParseRelayIni(DefaultRelayIni() + "[board]\nurl = https://a.net\n").relays.size() == 1);

	// The bridge on or off, and the relay chosen to host on.
	CHECK(ParseRelayIni("").enabled && ParseRelayIni(DefaultRelayIni()).enabled && ParseRelayIni(DefaultRelayIni()).use.empty());
	CHECK(!ParseRelayIni("[relay]\nenabled = no\n").enabled && !ParseRelayIni("[relay]\r\nEnabled=Off\r\n").enabled);
	CHECK(ParseRelayIni("[relay]\nenabled = yes\n").enabled && ParseRelayIni("[relays]\nenabled = no\n").enabled);
	cfg = ParseRelayIni("[relay]\nuse = local\n[relays]\nA = wss://a.net\nB = wss://b.net\nLocal = ws://127.0.0.1:8080\n");
	auto order = HostOrder(cfg);
	CHECK(order.size() == 3 && order[0].name == "Local" && order[1].name == "A" && order[2].name == "B");
	cfg.use = "nobody";
	CHECK(HostOrder(cfg)[0].name == "A");

	// The status file D2R VR Settings reads.
	BridgeStatus st;
	st.plugin = "0.9.5";
	st.config = ParseRelayIni("[relay]\nenabled = no\nuse = B\n[relays]\nA = wss://a.net\nB = wss://b.net\n");
	st.health = {RelayHealth {RelayState::Active, 0.25, "A"}};
	st.rooms.push_back(Room {.code = "EL9A", .name = "test \"1\"", .relayUrl = "wss://b.net"});
	st.hostingCode = "EL9A";
	st.hostingRelay = "B";
	const auto sj = json::Parse(StatusJson(st));
	CHECK(sj && !(*sj)["bridge"]["enabled"].asBool(true) && (*sj)["bridge"]["use"].asString() == "B" && (*sj)["plugin"].asString() == "0.9.5");
	CHECK(sj && (*sj)["relays"].size() == 2 && (*sj)["relays"][0]["state"].asString() == "active" && (*sj)["relays"][0]["usage"].asNumber() == 0.25);
	CHECK(sj && (*sj)["relays"][1]["usage"].isNull() && (*sj)["relays"][1]["first"].asBool() && !(*sj)["relays"][0]["first"].asBool(true));
	CHECK(sj && (*sj)["rooms"].size() == 1 && (*sj)["rooms"][0]["name"].asString().find("test \"1\"") == 0);
	CHECK(sj && (*sj)["hosting"]["room"].asString() == "EL9A" && (*sj)["hosting"]["relay"].asString() == "B");
	st.hostingCode.clear();
	CHECK(json::Parse(StatusJson(st)) && (*json::Parse(StatusJson(st)))["hosting"].isNull());

	auto u = ParseRelayUrl("wss://relay.example.net:8443/d2r");
	CHECK(u && u->secure && u->host == "relay.example.net" && u->port == 8443 && u->path == "/d2r");
	u = ParseRelayUrl("ws://127.0.0.1:8787");
	CHECK(u && !u->secure && u->port == 8787 && u->path.empty());
	CHECK(ParseRelayUrl("wss://x.net")->port == 443);
	CHECK(!ParseRelayUrl("https://x.net") && !ParseRelayUrl("wss://") && !ParseRelayUrl("wss://x.net:99999") && !ParseRelayUrl("wss://a b"));

	// d2rloader.toml: only [d2rloader.advanced] game_port counts.
	CHECK(ParseGamePort("") == 4000);
	CHECK(ParseGamePort("[d2rloader.advanced]\r\n# TCP port\r\ngame_port = 4001\r\n") == 4001);
	CHECK(ParseGamePort("[other]\ngame_port = 5000\n[d2rloader.advanced]\nautomatic_port_mapping = true\n") == 4000);
	CHECK(ParseGamePort("[d2rloader.advanced]\ngame_port = 70000\n") == 4000);

	// d2rloader.log lines (D2RLoader 1.3.1).
	CHECK(ParseHostingVerdict("[INFO] TCP/IP hosting is using TCP port 4001.\n") == HostingVerdict::Unknown);
	CHECK(ParseHostingVerdict("[INFO] TCP port 4001 is reachable from outside; no router changes were needed.\n") == HostingVerdict::Reachable);
	CHECK(ParseHostingVerdict("D2RLoader: Automatic router mapping failed: no compatible router was found. The game is still hosting\n") ==
	      HostingVerdict::Suspect);
	CHECK(ParseHostingVerdict("Automatic router mapping failed: no compatible router was found.\nHosting verification finished: Unreachable.\n") ==
	      HostingVerdict::Unreachable);
	CHECK(ParseHostingVerdict("Hosting readiness mapped via UPnP, but TCP port 4001 remained unreachable from outside.\n") ==
	      HostingVerdict::Unreachable);
	CHECK(ParseHostingVerdict("mapping failed\nAutomatic router mapping succeeded via UPnP on TCP port 4001.\nTCP port 4001 is reachable from outside\n") ==
	      HostingVerdict::Reachable);
	CHECK(ParseHostingVerdict("[HOSTING] x\nHosting verification finished: Verified.\n") == HostingVerdict::Reachable);
	CHECK(ParseHostingVerdict("Hosting verification finished: Service unavailable.\n") == HostingVerdict::Unknown);

	// /healthz
	auto h = ParseHealth(200, R"({"ok":true,"relay":1,"name":"Ours","status":"near_limit","open":true,"usage":0.85,"pipes":7})");
	CHECK(h.state == RelayState::NearLimit && h.usage > 0.84 && h.name == "Ours");
	CHECK(ParseHealth(200, R"({"ok":true,"relay":1,"status":"limit","open":false})").state == RelayState::Limit);
	CHECK(ParseHealth(200, R"({"ok":true,"relay":1,"open":true})").state == RelayState::Active);
	CHECK(ParseHealth(0, "").state == RelayState::NoAnswer && ParseHealth(502, "bad gateway").state == RelayState::NoAnswer);
	CHECK(ParseHealth(200, R"({"ok":true,"relay":2})").state == RelayState::NoAnswer);  // a protocol we do not speak
	CHECK(std::string(RelayStateLabel(RelayState::Limit)) == "limit reached");

	// The board carries the relay.
	auto join = ParseJoin(R"({"address":"1.2.3.4","port":4000,"relay":{"url":"wss://r.example","room":"room-id-0123456789ab","key":"key-0123456789abcdef"}})");
	CHECK(join && join->relay && join->relay->url == "wss://r.example" && join->relay->key == "key-0123456789abcdef");
	CHECK(!ParseJoin(R"({"address":"1.2.3.4","relay":null})")->relay);
	// Behind a relay the board keeps the host's address to itself.
	join = ParseJoin(R"({"address":"","port":4000,"relay":{"url":"wss://r.example","room":"room-id-0123456789ab","key":"key-0123456789abcdef"}})");
	CHECK(join && join->relay && join->address.empty());
	CHECK(!ParseJoin(R"({"address":"","relay":null})"));
	const RelayRef ref {"wss://r.example", "room-id-0123456789ab", "key-0123456789abcdef"};
	auto hb = json::Parse(HeartbeatBody(2, 0, RelayRefJson(ref)));
	CHECK(hb && (*hb)["relay"]["room"].asString() == ref.room);
	CHECK((*json::Parse(HeartbeatBody(2, 0, "null"))).has("relay") && (*json::Parse(HeartbeatBody(2, 0, "null")))["relay"].isNull());
	CHECK(!json::Parse(HeartbeatBody(2, 0))->has("relay"));
	CHECK((*json::Parse(CreateBody("x", {}, "", 0, RelayRefJson(ref))))["relay"]["url"].asString() == ref.url);
	auto rooms = ParseRooms(R"({"rooms":[{"room_id":"a","code":"AB23","name":"R","relay_url":"wss://r.example"}]})");
	CHECK(rooms && (*rooms)[0].relayUrl == "wss://r.example");
	(*rooms)[0].relayState = RelayState::NearLimit;
	CHECK(RoomLine((*rooms)[0]).find("relay: near its limit") != std::string::npos);

	// Frames, tokens, hashes.
	CHECK(Frame(kFrameData, 3, "ab") == std::string("\x02\x03" "ab", 4));
	CHECK(Frame(kFrameOpen, 1).size() == 2);
	const uint8_t sixteen[16] {};
	CHECK(Base64Url(sixteen, 16) == "AAAAAAAAAAAAAAAAAAAAAA");
	const uint8_t man[3] {'M', 'a', 'n'};
	CHECK(Base64Url(man, 3) == "TWFu");
	CHECK(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(Sha256Hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

}  // namespace

// tests --dump <dir>: the D2R Rooms window (a list, and a password asked for)
// and the tab button as JSON, for a picture of the layout without the game.
int Dump(const std::string& dir) {
	PanelView view;
	view.rooms = *ParseRooms(R"({"rooms":[
		{"room_id":"a","code":"EL9A","name":"test","mod":{"id":"d2r-vr","version":"0.189.0"},"difficulty":"normal","mode":"softcore",
		 "act":1,"max_players":8,"players_count":1,"locked":true,"players":[],"relay_url":"wss://d2r-rooms-relay.d2r-rooms.workers.dev"},
		{"room_id":"b","code":"K7QX","name":"Baal runs","mod":{"id":"d2r-vr","version":"0.189.0"},"difficulty":"hell","mode":"softcore",
		 "act":5,"max_players":8,"players_count":3,"locked":false,
		 "players":[{"name":"Kaela","class":"paladin","level":92,"act":5,"host":true},{"name":"Ama","class":"amazon","level":88,"act":5}]}]})");
	view.rooms[0].relayState = RelayState::Active;
	view.status = "2 open room(s) - page 1/1     Relays: D2R Rooms active";
	view.tab = true;
	view.generation = 1;
	std::ofstream(dir + "/window.json") << PanelLayout(view);
	std::ofstream(dir + "/tab.json") << TabLayout();
	return 0;
}

int main(int argc, char** argv) {
	if (argc == 3 && std::string(argv[1]) == "--dump") return Dump(argv[2]);
	TestJson();
	TestDetectMod();
	TestResolveMod();
	TestBoard();
	TestLayout();
	TestWideWindow();
	TestToLayout();
	TestBand();
	TestSafeArea();
	TestBandBiggerThanWindow();
	TestSharedBand();
	TestClientSizeQuery();
	TestPlayerCount();
	TestRelay();
	std::printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
