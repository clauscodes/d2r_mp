#include "layout.h"

#include <algorithm>

#include "json.h"

namespace rooms {

namespace {

// Same frame as the TCP/IP panel, placed to its left: both are anchored at
// (0.5, 0.1) and the TCP/IP panel spans x -581..581.
constexpr int kPanelX = -1763;
constexpr int kPanelWidth = 1162;
constexpr int kPanelHeight = 1507;
constexpr int kRowTop = 330;
constexpr int kRowStep = 150;

// D2RLoader's Multiplayer window (1.3.1-beta), anchored at the top middle of
// the screen, in layout units (2160 high): the window spans x -960..960,
// y 131..1751, its tabs HOST GAME and BROWSE GAMES x -264..264 at y 285..384,
// its tab content y 437..1651. Measured from screenshots and logged clicks:
// the plugin cannot read that window's own rect.
constexpr int kWindowX = -960;
constexpr int kWindowY = 131;
constexpr int kWindowWidth = 1920;
constexpr int kWindowHeight = 1620;
constexpr int kTabX = 270;
constexpr int kTabY = 285;
constexpr int kTabWidth = 260;
constexpr int kTabHeight = 100;
constexpr int kContentY = 437;
// FrontEnd_ButtonSmall as drawn there (measured).
constexpr int kButtonWidth = 340;
constexpr int kButtonHeight = 90;
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

std::string Button(const std::string& name, int x, int y, std::string_view label, const std::string& command,
                   const std::string& argument = {}) {
	std::string message = std::string(kMessageTarget) + ":" + command;
	if (!argument.empty()) message += ":" + argument;
	return "{\"type\":\"ButtonWidget\",\"name\":" + json::Quote(name) + ",\"fields\":{\"rect\":" + Rect(x, y) +
		",\"filename\":\"FrontEnd\\\\HD\\\\Final\\\\FrontEnd_ButtonSmall\",\"textString\":" + json::Quote(FieldText(label)) +
		",\"text/style\":\"$StyleFEButtonText\",\"textColor\":\"$LightButtonTextColor\",\"pressedFrame\":1,\"hoveredFrame\":3"
		",\"onClickMessage\":" + json::Quote(message) + "}}";
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

// The D2R Rooms window: in place of D2RLoader's Multiplayer window, the same
// size and spot, with the same three tabs. HOST GAME and BROWSE GAMES go back
// to D2RLoader's window; this one is an ordinary panel, so its buttons and
// its password field take clicks and typing.
std::string WindowLayout(const PanelView& view, int pages, int page) {
	const int left = 134, right = kWindowWidth - 134;
	const int buttonX = right - kButtonWidth;
	const int textW = buttonX - left - 40;
	std::vector<std::string> children;
	children.push_back("{\"type\":\"GridImageWidget\",\"name\":\"Background\",\"fields\":{\"filename\":\"PANEL\\\\TCP_IP\\\\TCP_IP_PanelBG\"}}");
	children.push_back("{\"type\":\"TextBoxWidget\",\"name\":\"Title\",\"fields\":{\"rect\":" + Rect(0, 30, kWindowWidth, 90) +
		",\"text\":\"Multiplayer\",\"style\":\"$StyleTitleBlock\"}}");
	children.push_back(
		"{\"type\":\"ButtonWidget\",\"name\":\"Close\",\"fields\":{\"rect\":" + Rect(kWindowWidth - 90, 9) +
		",\"filename\":\"PANEL\\\\closebtn_4x\",\"hoveredFrame\":3,\"onClickMessage\":\"" + std::string(kMessageTarget) + ":Close\"}}");
	// The tabs where D2RLoader's are, ours selected.
	children.push_back(Button("HostTab", kTabX - kWindowX - 2 * (kButtonWidth + 10), kTabY - kWindowY, "Host Game", "ToHost"));
	children.push_back(Button("BrowseTab", kTabX - kWindowX - (kButtonWidth + 10), kTabY - kWindowY, "Browse Games", "ToBrowse"));
	children.push_back(Button("RoomsTab", kTabX - kWindowX, kTabY - kWindowY, "D2R Rooms", "Tab"));

	const int top = kContentY - kWindowY;
	if (!view.status.empty())
		children.push_back(Text("Status", Rect(left, top, right - left, 70), view.status, Style("$SmallFontSize", "center", true)));
	Rows(view, page, left, top + 90, textW, buttonX, children);

	const int bottom = top + 90 + kRowsPerPage * kRowStep + 20;
	if (view.hosting) {
		children.push_back(Text("Hosting", Rect(left, bottom, textW, 120), view.hostingText, Style("$SmallFontSize", "left", true)));
		children.push_back(Button("CloseRoom", buttonX, bottom + 10, "Close room", "CloseRoom"));
	} else {
		children.push_back(Text("NameLabel", Rect(left, bottom, 700, 40), "Password of a locked room", Style("$SmallFontSize", "left", false)));
		children.push_back(PasswordField(left + 40, bottom + 75));
	}
	const int y = kWindowHeight - kButtonHeight - 40;
	if (page > 0) children.push_back(Button("Prev", left, y, "Back", "Prev"));
	children.push_back(Button("Refresh", (kWindowWidth - kButtonWidth) / 2, y, "Refresh", "Refresh"));
	if (page + 1 < pages) children.push_back(Button("Next", buttonX, y, "More", "Next"));
	return Panel(kPanelName, "{\"x\":0.5,\"y\":0}", Rect(kWindowX, kWindowY, kWindowWidth, kWindowHeight), kLoaderPriority, children);
}

}  // namespace

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
	children.push_back("{\"type\":\"TextBoxWidget\",\"name\":\"Title\",\"fields\":{\"rect\":\"$InGamePanelTitleRect\",\"text\":\"D2R Rooms\",\"style\":\"$StyleTitleBlock\"}}");
	children.push_back(
		"{\"type\":\"ButtonWidget\",\"name\":\"Close\",\"fields\":{\"rect\":{\"x\":1075,\"y\":9},\"filename\":\"PANEL\\\\closebtn_4x\","
		"\"hoveredFrame\":3,\"onClickMessage\":\"" + std::string(kMessageTarget) + ":Close\"}}");
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
	return Panel(kPanelName, "{\"x\":0.5,\"y\":0.1}", Rect(kPanelX, 0, kPanelWidth, kPanelHeight), 8500, children);
}

std::vector<HitArea> TabHitAreas() {
	// A little larger than the button: its sprite was measured from a screenshot.
	return {{kTabX - 10, kTabY - 10, kButtonWidth + 20, kButtonHeight + 50, "Tab", ""}};
}

std::string TabLayout() {
	return Panel(kTabPanelName, "{\"x\":0.5,\"y\":0}", Rect(kTabX, kTabY, kTabWidth, kTabHeight), kLoaderPriority,
	             {Button("Tab", 0, 0, "D2R Rooms", "Tab")});
}

}  // namespace rooms
