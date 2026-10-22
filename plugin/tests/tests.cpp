// Unit tests for the parts without D2RLoader: JSON, the board protocol and the
// panel layout. Plain asserts with a report; no framework needed.

#include <cstdio>
#include <string>

#include "board.h"
#include "json.h"
#include "layout.h"

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

}  // namespace

int main() {
	TestJson();
	TestDetectMod();
	TestBoard();
	TestLayout();
	std::printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
