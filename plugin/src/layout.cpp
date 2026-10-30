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
// the screen, in layout units (2160 high): its tabs HOST GAME and BROWSE GAMES
// span x -264..264 at y 285..384, its tab content x -866..866, y 437..1651.
// Measured from a screenshot: the plugin cannot read that window's own rect.
constexpr int kTabX = 270;
constexpr int kTabY = 285;
constexpr int kTabWidth = 260;
constexpr int kTabHeight = 100;
constexpr int kContentX = -866;
constexpr int kContentY = 437;
constexpr int kContentWidth = 1732;
constexpr int kContentHeight = 1214;
// Above D2RLoader's own layers (its front-end buttons use 2000000001), so its
// window's dimming does not cover the tab and the list.
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
		children.push_back(Button("Join" + n, buttonX, y + 10, "Join", "Join", n));
	}
}

// Inside D2RLoader's window, in place of its tab content.
std::string TabContentLayout(const PanelView& view, int pages, int page) {
	const int buttonX = kContentWidth - 290;
	std::vector<std::string> children;
	if (!view.status.empty()) children.push_back(Text("Status", Rect(40, 0, kContentWidth - 80, 70), view.status, Style("$SmallFontSize", "center", true)));
	Rows(view, page, 40, 90, buttonX - 80, buttonX, children);

	const int bottom = 90 + kRowsPerPage * kRowStep + 20;
	if (view.hosting) {
		children.push_back(Text("Hosting", Rect(40, bottom, buttonX - 80, 120), view.hostingText, Style("$SmallFontSize", "left", true)));
		children.push_back(Button("CloseRoom", buttonX, bottom + 10, "Close room", "CloseRoom"));
	} else {
		children.push_back(Text("NameLabel", Rect(40, bottom, 700, 40), "Password of a locked room", Style("$SmallFontSize", "left", false)));
		children.push_back(PasswordField(80, bottom + 75));
	}
	const int y = kContentHeight - 100;
	if (page > 0) children.push_back(Button("Prev", 40, y, "Back", "Prev"));
	children.push_back(Button("Refresh", (kContentWidth - 250) / 2, y, "Refresh", "Refresh"));
	if (page + 1 < pages) children.push_back(Button("Next", buttonX, y, "More", "Next"));
	return Panel(kPanelName, "{\"x\":0.5,\"y\":0}", Rect(kContentX, kContentY, kContentWidth, kContentHeight), kLoaderPriority, children);
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
	if (view.tab) return TabContentLayout(view, pages, page);

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

std::string TabLayout() {
	return Panel(kTabPanelName, "{\"x\":0.5,\"y\":0}", Rect(kTabX, kTabY, kTabWidth, kTabHeight), kLoaderPriority,
	             {Button("Tab", 0, 0, "D2R Rooms", "Tab")});
}

}  // namespace rooms
