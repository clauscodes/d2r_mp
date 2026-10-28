// Unit tests for the parts without D2RLoader: JSON, the board protocol and the
// panel layout. Plain asserts with a report; no framework needed.

#include <cstdio>
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
	CHECK(PageCount(0) == 1 && PageCount(6) == 1 && PageCount(7) == 2);
	CHECK(FieldText("@strHack") == " @strHack" && FieldText("$Style") == " $Style" && FieldText("ok") == "ok");

	PanelView view;
	view.rooms = *ParseRooms(kRooms);
	for (int i = 0; i < 6; ++i) view.rooms.push_back(view.rooms[1]);  // 8 rooms -> 2 pages
	view.status = "8 open rooms";

	auto layout = json::Parse(PanelLayout(view));
	CHECK(layout.has_value());
	CHECK((*layout)["name"].asString() == kPanelName);
	const json::Value& kids = (*layout)["children"];
	int joins = 0;
	bool next = false, prev = false, input = false;
	std::string firstRoom;
	for (size_t i = 0; i < kids.size(); ++i) {
		const std::string name = kids[i]["name"].asString();
		if (name.rfind("Join", 0) == 0) {
			++joins;
			CHECK(kids[i]["fields"]["onClickMessage"].asString() == std::string(kMessageTarget) + ":Join:" + name.substr(4));
		}
		if (name == "Next") next = true;
		if (name == "Prev") prev = true;
		if (name == "RoomName") input = kids[i]["type"].asString() == "InputTextBoxWidget";
		if (name == "Room0") firstRoom = kids[i]["fields"]["text"].asString();
	}
	CHECK(joins == kRowsPerPage && next && !prev && input);
	CHECK(firstRoom == RoomLine(view.rooms[0]));

	view.page = 5;  // clamped to the last page: 2 rooms there, Back but no More
	layout = json::Parse(PanelLayout(view));
	joins = 0;
	next = prev = false;
	for (size_t i = 0; i < (*layout)["children"].size(); ++i) {
		const std::string name = (*layout)["children"][i]["name"].asString();
		if (name.rfind("Join", 0) == 0) ++joins;
		if (name == "Next") next = true;
		if (name == "Prev") prev = true;
	}
	CHECK(joins == 2 && prev && !next);

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

int main() {
	TestJson();
	TestDetectMod();
	TestBoard();
	TestLayout();
	TestRelay();
	std::printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
