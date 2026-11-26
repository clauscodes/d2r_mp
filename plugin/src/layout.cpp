#include "layout.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <iterator>
#include <sstream>

#include "json.h"

namespace rooms {

namespace {

// Same frame as the TCP/IP panel, placed to its left: both are anchored at
// (0.5, 0.1) and the TCP/IP panel spans x -581..581.
constexpr int kPanelX = -1763;
constexpr int kPanelWidth = 1162;
constexpr int kPanelHeight = 1507;
constexpr int kRowTop = 330;
constexpr int kRowStep = 135;

// D2RLoader's Multiplayer window (1.3.1-beta) is anchored at the top middle of
// the screen, in layout units (2160 high): x -960..960, y 131..1751, its title
// bar y 203..285, its MainTabs x -865..865 at y 282..403 (as it reports), its
// Public IP box x 344..900 below the title bar. The D2R Rooms button sits right
// of its title, above that box (between BROWSE GAMES and it are 80 units).
constexpr int kTabX = 520;
constexpr int kTabY = 196;
// FrontEnd_ButtonSmall as drawn in the game (measured on a screenshot).
constexpr int kButtonWidth = 350;
constexpr int kButtonHeight = 85;
// Above D2RLoader's own layers (its front-end buttons use 2000000001), so its
// window's dimming does not cover the tab button.
constexpr int kLoaderPriority = 2100000000;

std::string Rect(int x, int y, int w = -1, int h = -1) {
	std::string r = "{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y);
	if (w >= 0) r += ",\"width\":" + std::to_string(w);
	if (h >= 0) r += ",\"height\":" + std::to_string(h);
	return r + "}";
}

std::string Style(const char* size, const char* align, bool wrap) {
	return std::string("{\"fontColor\":\"$FontColorLightGold\",\"pointSize\":\"") + size +
		"\",\"alignment\":{\"h\":\"" + align + "\",\"v\":\"center\"},\"options\":{\"newlineHandling\":\"standard\"" +
		(wrap ? ",\"lineWrap\":true" : "") + "}}";
}

std::string Text(const std::string& name, const std::string& rect, std::string_view text, const std::string& style) {
	return "{\"type\":\"TextBoxWidget\",\"name\":" + json::Quote(name) + ",\"fields\":{\"rect\":" + rect +
		",\"text\":" + json::Quote(FieldText(text)) + ",\"style\":" + style + "}}";
}

// No onClickMessage: in the front-end a plugin panel gets no clicks (the
// plugin matches them itself), and D2RLoader asserts on a message target it
// has no registration for ("Missing message value list registration for
// D2RRooms"). `command` and `argument` name the button for HitArea.
std::string Button(const std::string& name, int x, int y, std::string_view label, const std::string& command,
                   const std::string& argument = {}) {
	(void)command;
	(void)argument;
	return "{\"type\":\"ButtonWidget\",\"name\":" + json::Quote(name) + ",\"fields\":{\"rect\":" + Rect(x, y) +
		",\"filename\":\"FrontEnd\\\\HD\\\\Final\\\\FrontEnd_ButtonSmall\",\"textString\":" + json::Quote(FieldText(label)) +
		",\"text/style\":\"$StyleFEButtonText\",\"textColor\":\"$LightButtonTextColor\",\"pressedFrame\":1,\"hoveredFrame\":3}}";
}

std::string PasswordField(int x, int y) {
	return "{\"type\":\"ImageWidget\",\"name\":\"NameBackground\",\"fields\":{\"rect\":" + Rect(x - 40, y - 35) +
		",\"filename\":\"PANEL\\\\TCP_IP\\\\TCP_IP_TextEntry\"}},"
		"{\"type\":\"InputTextBoxWidget\",\"name\":\"RoomName\",\"fields\":{\"rect\":" + Rect(x, y, 520, 60) +
		",\"backgroundColor\":{\"r\":0,\"g\":0,\"b\":0,\"a\":0},\"fontStyle\":{\"alignment\":{\"h\":\"center\",\"v\":\"top\"},"
		"\"pointSize\":\"$MediumFontSize\",\"fontColor\":\"$FontColorLightGold\"},\"fontType\":\"16pt\"}}";
}

std::string Panel(const std::string& name, const std::string& anchor, const std::string& rect, int priority,
                  const std::vector<std::string>& children) {
	std::string out = "{\"type\":\"Panel\",\"name\":" + json::Quote(name) + ",\"fields\":{\"anchor\":" + anchor + ",\"rect\":" + rect +
		",\"priority\":" + std::to_string(priority) + "},\"children\":[";
	for (size_t i = 0; i < children.size(); ++i) {
		if (i) out += ",";
		out += children[i];
	}
	return out + "]}";
}

// The rows of the current page, from `top`, with Join at `buttonX`. A room
// without player details gets no players line: no empty text widgets.
void Rows(const PanelView& view, int page, int x, int top, int textW, int buttonX, std::vector<std::string>& children) {
	const size_t first = static_cast<size_t>(page) * kRowsPerPage;
	for (int i = 0; i < kRowsPerPage; ++i) {
		const size_t index = first + i;
		if (index >= view.rooms.size()) break;
		const Room& room = view.rooms[index];
		const int y = top + i * kRowStep;
		const std::string n = std::to_string(i);
		children.push_back(Text("Room" + n, Rect(x, y, textW, 60), RoomLine(room), Style("$MediumFontSize", "left", false)));
		const std::string players = PlayersLine(room);
		if (!players.empty())
			children.push_back(Text("Players" + n, Rect(x, y + 60, textW, 70), players, Style("$SmallFontSize", "left", true)));
		if (buttonX >= 0) children.push_back(Button("Join" + n, buttonX, y + 10, "Join", "Join", n));
	}
}

struct ButtonSpec {
	std::string name;
	int x, y;  // in its panel
	std::string label, command, argument;
};

constexpr int kCloseX = 1075, kCloseY = 9;  // the art's close button, about 70 x 70

// The D2R Rooms window: in place of D2RLoader's Multiplayer window, where D2R
// puts its own TCP/IP screen, on that screen's art (PANEL\TCP_IP\TCP_IP_PanelBG
// draws at its own size, 1162 x 1507, and does not stretch). Everything is on
// that art's grid: the list in its upper box, a hint in its lower box, the
// buttons on its bottom row. In the front-end a plugin panel gets no clicks
// and no typing, so the plugin matches clicks against WindowButtons itself,
// and the window has no text field (a password goes into D2RLoader's Direct
// Join box).
constexpr int kWindowLeft = -kPanelWidth / 2;  // anchored at (0.5, 0.1)
constexpr int kWindowTop = 216;
// Measured on the art in the game: its inner edges are near x 74 and 1088, a
// button is about 350 x 85. So two buttons a row: left at 74, right at 730.
constexpr int kLeftX = 74, kRightX = 730;
constexpr int kTextX = 90, kTextW = 620;

std::vector<ButtonSpec> WindowButtons(const PanelView& view, int pages, int page) {
	std::vector<ButtonSpec> b;
	const size_t first = static_cast<size_t>(page) * kRowsPerPage;
	for (int i = 0; i < kRowsPerPage && first + i < view.rooms.size(); ++i)
		b.push_back({"Join" + std::to_string(i), kRightX, kRowTop + i * kRowStep + 15, "Join", "Join", std::to_string(i)});
	if (view.hosting) b.push_back({"CloseRoom", kRightX, 1260, "Close room", "CloseRoom", ""});
	else if (pages > 1) b.push_back({"More", kRightX, 1260, "More", "Next", ""});
	else b.push_back({"Refresh", kRightX, 1260, "Refresh", "Refresh", ""});
	b.push_back({"HostTab", kLeftX, 1390, "Host Game", "ToHost", ""});
	b.push_back({"BrowseTab", kRightX, 1390, "Browse Games", "ToBrowse", ""});
	return b;
}

std::string WindowLayout(const PanelView& view, int pages, int page) {
	std::vector<std::string> children;
	children.push_back("{\"type\":\"GridImageWidget\",\"name\":\"Background\",\"fields\":{\"filename\":\"PANEL\\\\TCP_IP\\\\TCP_IP_PanelBG\"}}");
	children.push_back("{\"type\":\"TextBoxWidget\",\"name\":\"Title" + std::to_string(view.generation) +
		"\",\"fields\":{\"rect\":\"$InGamePanelTitleRect\",\"text\":\"D2R Rooms\",\"style\":\"$StyleTitleBlock\"}}");
	children.push_back(
		"{\"type\":\"ButtonWidget\",\"name\":\"Close\",\"fields\":{\"rect\":" + Rect(kCloseX, kCloseY) + ",\"filename\":\"PANEL\\\\closebtn_4x\","
		"\"hoveredFrame\":3}}");
	if (!view.status.empty())
		children.push_back(Text("Status", Rect(kTextX, 240, 982, 60), view.status, Style("$SmallFontSize", "center", false)));

	// Two lines a room: its name, then difficulty, players, version, connection
	// (wrapping to a second line); the list box holds five.
	const size_t first = static_cast<size_t>(page) * kRowsPerPage;
	for (int i = 0; i < kRowsPerPage && first + i < view.rooms.size(); ++i) {
		const RoomColumns c = Columns(view.rooms[first + i]);
		const int y = kRowTop + i * kRowStep;
		const std::string n = std::to_string(i);
		children.push_back(Text("Room" + n, Rect(kTextX, y, kTextW, 60), c.name, Style("$MediumFontSize", "left", false)));
		const std::string details = c.game + " - " + c.players + " - " + c.version + " - " + c.connection;
		children.push_back(Text("Details" + n, Rect(kTextX, y + 55, kTextW, 75), details, Style("$SmallFontSize", "left", true)));
	}
	children.push_back(Text("Hint", Rect(kTextX, 1200, kTextW, 150),
	                        view.hosting ? view.hostingText
	                                     : "Host with Create Game in Host Game: the game is listed here, through a relay when friends cannot reach "
	                                       "you. A room's password goes into Direct Join.",
	                        Style("$SmallFontSize", "left", true)));
	for (const ButtonSpec& b : WindowButtons(view, pages, page)) children.push_back(Button(b.name, b.x, b.y, b.label, b.command, b.argument));
	return Panel(PanelName(view.slot), "{\"x\":0.5,\"y\":0.1}", Rect(kWindowLeft, 0, kPanelWidth, kPanelHeight), kLoaderPriority, children);
}

}  // namespace

std::string PanelLocalId(int slot) {
	return "Rooms" + std::to_string(slot);
}

std::string PanelName(int slot) {
	return std::string(kPluginId) + "/" + PanelLocalId(slot);
}

std::string PanelPath(int slot) {
	return "data/global/ui/layouts/" + std::string(kPluginId) + "/" + PanelLocalId(slot) + "hd.json";
}

int PageCount(size_t roomCount) {
	return std::max(1, static_cast<int>((roomCount + kRowsPerPage - 1) / kRowsPerPage));
}

std::string FieldText(std::string_view text) {
	std::string out(text);
	if (!out.empty() && (out[0] == '@' || out[0] == '$')) out.insert(out.begin(), ' ');
	return out;
}

std::string PanelLayout(const PanelView& view) {
	const int pages = PageCount(view.rooms.size());
	const int page = std::clamp(view.page, 0, pages - 1);
	if (view.tab) return WindowLayout(view, pages, page);

	const int buttonX = 870;
	std::vector<std::string> children;
	children.push_back("{\"type\":\"GridImageWidget\",\"name\":\"Background\",\"fields\":{\"filename\":\"PANEL\\\\TCP_IP\\\\TCP_IP_PanelBG\"}}");
	children.push_back("{\"type\":\"TextBoxWidget\",\"name\":\"Title" + std::to_string(view.generation) +
		"\",\"fields\":{\"rect\":\"$InGamePanelTitleRect\",\"text\":\"D2R Rooms\",\"style\":\"$StyleTitleBlock\"}}");
	children.push_back(
		"{\"type\":\"ButtonWidget\",\"name\":\"Close\",\"fields\":{\"rect\":{\"x\":1075,\"y\":9},\"filename\":\"PANEL\\\\closebtn_4x\","
		"\"hoveredFrame\":3}}");
	if (!view.status.empty()) children.push_back(Text("Status", Rect(90, 240, 982, 60), view.status, Style("$SmallFontSize", "center", false)));
	Rows(view, page, 90, kRowTop, 760, buttonX, children);

	if (view.hosting) {
		children.push_back(Text("Hosting", Rect(90, 1230, 760, 120), view.hostingText, Style("$SmallFontSize", "left", true)));
		children.push_back(Button("CloseRoom", buttonX, 1250, "Close room", "CloseRoom"));
	} else {
		children.push_back(Text("NameLabel", Rect(90, 1200, 760, 50), "Room name (or the password of a locked room)", Style("$SmallFontSize", "left", false)));
		children.push_back(PasswordField(100, 1275));
		children.push_back(Button("Create", buttonX, 1260, "Create", "Create"));
	}

	const int y = 1390;
	if (page > 0) children.push_back(Button("Prev", 90, y, "Back", "Prev"));
	children.push_back(Button("Refresh", 480, y, "Refresh", "Refresh"));
	if (page + 1 < pages) children.push_back(Button("Next", buttonX, y, "More", "Next"));
	return Panel(PanelName(view.slot), "{\"x\":0.5,\"y\":0.1}", Rect(kPanelX, 0, kPanelWidth, kPanelHeight), 8500, children);
}

std::vector<HitArea> TabHitAreas() {
	// Larger than the button: its sprite was measured from a screenshot, and a
	// VR pointer's press lands a little beside where it aims (left of it, in the
	// player's log). Left of the button is the empty title bar.
	return {{kTabX - 80, kTabY - 10, kButtonWidth + 100, kButtonHeight + 50, "Tab", ""}};
}

std::vector<HitArea> WindowHitAreas(const PanelView& view) {
	const int pages = PageCount(view.rooms.size());
	// The close button's area reaches past the art's corner: a VR pointer's
	// press lands beside it (left of it and above, in the player's log).
	std::vector<HitArea> areas {{kWindowLeft + kCloseX - 80, kWindowTop + kCloseY - 30, 160, 120, "Close", ""}};
	for (const ButtonSpec& b : WindowButtons(view, pages, std::clamp(view.page, 0, pages - 1)))
		areas.push_back({kWindowLeft + b.x, kWindowTop + b.y, kButtonWidth, kButtonHeight, b.command, b.argument});
	return areas;
}

LayoutPoint ToLayout(const ClickMap& map, double px, double py) {
	if (map.kx <= 0 || map.ky <= 0) return {0, 0};
	return {static_cast<int>(std::lround((px - map.cx) / map.kx)), static_cast<int>(std::lround((py - map.top) / map.ky))};
}

PixelPoint FromLayout(const ClickMap& map, double x, double y) {
	return {map.cx + x * map.kx, map.top + y * map.ky};
}

ClickMap PictureMap(int windowW, int windowH, int pictureW, int pictureH) {
	if (pictureW <= 0 || pictureH <= 0) {
		pictureW = windowW;
		pictureH = windowH;
	}
	const double k = std::min(pictureW / 3840.0, pictureH / 2160.0);
	return {k, k, windowW / 2.0, windowH / 2.0 - 1080 * k};
}

LayoutPoint ToLayout(double px, double py, int windowW, int windowH, int pictureW, int pictureH) {
	return ToLayout(PictureMap(windowW, windowH, pictureW, pictureH), px, py);
}

std::optional<LayoutPoint> LoaderTarget(std::string_view command) {
	// The tabs: D2RLoader's MainTabs row is 282..403; the buttons from a
	// player's flat game (1366x768) and the Direct Join box's measured corner.
	if (command == "ShowHost") return LayoutPoint {-125, 342};
	if (command == "ShowBrowse") return LayoutPoint {114, 342};
	if (command == "CreateGame") return LayoutPoint {-6, 1510};
	if (command == "JoinDirect") return LayoutPoint {-209, 1181};
	return std::nullopt;
}

bool IsBand(int pictureW, int pictureH, int canvasW, int canvasH) {
	return pictureW > 0 && canvasW == pictureW && canvasH > 0 && canvasH < pictureH && std::abs(canvasH - canvasW * 9 / 16) <= 1;
}

ClickMap BandMap(int windowW, int windowH, int pictureW, int pictureH, int canvasW, int canvasH) {
	if (pictureW <= 0 || pictureH <= 0) {
		pictureW = windowW;
		pictureH = windowH;
	}
	const double sx = static_cast<double>(windowW) / pictureW, sy = static_cast<double>(windowH) / pictureH;
	const double k = std::min(canvasW / 3840.0, canvasH / 2160.0);
	return {k * sx, k * sy, canvasW / 2.0 * sx, (canvasH / 2.0 - 1080 * k) * sy};
}

ClickMap LearnMap(const ClickMap& guess, int windowW, int windowH, const std::vector<ClickSample>& samples) {
	if (samples.size() < 2 || windowW <= 0 || windowH <= 0 || guess.kx <= 0 || guess.ky <= 0) return guess;
	// The height from two rows at least (a row alone says nothing of where the
	// interface's top is): the median slope of the pairs on different rows.
	std::vector<double> ks;
	for (size_t i = 0; i < samples.size(); ++i)
		for (size_t j = i + 1; j < samples.size(); ++j)
			if (std::abs(samples[j].y - samples[i].y) >= kMinRowSpan) ks.push_back((samples[j].py - samples[i].py) / (samples[j].y - samples[i].y));
	if (ks.empty()) return guess;
	std::sort(ks.begin(), ks.end());
	const double ky = ks[ks.size() / 2];
	// Presses far from where they were taken for (a stray click) leave the guess.
	if (!(ky > 0.05 && ky < 5)) return guess;
	std::vector<double> tops;
	for (const ClickSample& s : samples) tops.push_back(s.py - s.y * ky);
	std::sort(tops.begin(), tops.end());
	const double top = tops[tops.size() / 2];
	// The width: the guess's shape, unless the box is clearly taller than the
	// guess (a press is anywhere on a tab, some 8% of the height either way).
	const double kx = ky > guess.ky * 1.15 ? windowW / 3840.0 : guess.kx * ky / guess.ky;
	return {kx, ky, windowW / 2.0, top};
}

std::vector<KeyedSample> ParseClickSamples(std::string_view text) {
	std::vector<KeyedSample> out;
	std::istringstream lines {std::string(text)};
	std::string line;
	while (std::getline(lines, line)) {
		std::istringstream in(line);
		std::vector<double> v;
		double d = 0;
		while (in >> d) v.push_back(d);
		if (v.size() != 10) continue;
		const auto i = [&](size_t n) { return static_cast<int>(std::lround(v[n])); };
		const MapKey key {i(0), i(1), i(2), i(3), i(4), i(5)};
		if (key.windowW <= 0 || key.windowH <= 0) continue;
		out.push_back({key, {v[6], v[7], i(8), i(9)}});
	}
	return out;
}

std::vector<size_t> FindClientSizeQuery(std::string_view code) {
	// From byte 14: mov rcx,rax; call ?; mov eax,[rsp+28h]; mov [rsp+40h],eax.
	static constexpr int kTail[] = {0x48, 0x8B, 0xC8, 0xE8, -1, -1, -1, -1, 0x8B, 0x44, 0x24, 0x28, 0x89, 0x44, 0x24, 0x40};
	static constexpr int kOriginal[] = {0x48, 0x83, 0xEC, 0x38, 0xE8, -1, -1, -1, -1, 0x48, 0x8D, 0x54, 0x24, 0x20};
	constexpr size_t kTailAt = 14;
	const auto byte = [&](size_t i) { return static_cast<unsigned char>(code[i]); };
	std::vector<size_t> hits;
	for (size_t at = code.find('\x48'); at != std::string_view::npos && at + std::size(kTail) <= code.size(); at = code.find('\x48', at + 1)) {
		if (at < kTailAt) continue;
		const size_t fn = at - kTailAt;
		bool tail = true;
		for (size_t i = 0; i < std::size(kTail) && tail; ++i) tail = kTail[i] < 0 || byte(at + i) == kTail[i];
		if (!tail) continue;
		bool original = true;
		for (size_t i = 0; i < std::size(kOriginal) && original; ++i) original = kOriginal[i] < 0 || byte(fn + i) == kOriginal[i];
		// A hook's jump: jmp rel32, jmp [rip+0], or mov rax/r11, imm64 then jmp.
		const unsigned char b0 = byte(fn), b1 = byte(fn + 1);
		const bool hooked = b0 == 0xE9 || (b0 == 0xFF && b1 == 0x25) || ((b0 == 0x48 || b0 == 0x49) && b1 >= 0xB8 && b1 <= 0xBF);
		if (original || hooked) hits.push_back(fn);
	}
	return hits;
}

std::string ClickSamplesText(const std::vector<KeyedSample>& samples) {
	std::ostringstream out;
	for (const KeyedSample& s : samples)
		out << s.key.windowW << ' ' << s.key.windowH << ' ' << s.key.pictureW << ' ' << s.key.pictureH << ' ' << s.key.canvasW << ' '
		    << s.key.canvasH << ' ' << s.sample.px << ' ' << s.sample.py << ' ' << s.sample.x << ' ' << s.sample.y << '\n';
	return out.str();
}

std::string TabLayout() {
	return Panel(kTabPanelName, "{\"x\":0.5,\"y\":0}", Rect(kTabX, kTabY, kButtonWidth, kButtonHeight), kLoaderPriority,
	             {Button("Tab", 0, 0, "D2R Rooms (F9)", "Tab")});
}

}  // namespace rooms
