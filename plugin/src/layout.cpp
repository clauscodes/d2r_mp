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
	std::vector<std::string> children;

	children.push_back("{\"type\":\"GridImageWidget\",\"name\":\"Background\",\"fields\":{\"filename\":\"PANEL\\\\TCP_IP\\\\TCP_IP_PanelBG\"}}");
	children.push_back("{\"type\":\"TextBoxWidget\",\"name\":\"Title\",\"fields\":{\"rect\":\"$InGamePanelTitleRect\",\"text\":\"D2R Rooms\",\"style\":\"$StyleTitleBlock\"}}");
	children.push_back(
		"{\"type\":\"ButtonWidget\",\"name\":\"Close\",\"fields\":{\"rect\":{\"x\":1075,\"y\":9},\"filename\":\"PANEL\\\\closebtn_4x\","
		"\"hoveredFrame\":3,\"onClickMessage\":\"" + std::string(kMessageTarget) + ":Close\"}}");
	children.push_back(Text("Status", Rect(90, 240, 982, 60), view.status, Style("$SmallFontSize", "center", false)));

	const size_t first = static_cast<size_t>(page) * kRowsPerPage;
	for (int i = 0; i < kRowsPerPage; ++i) {
		const size_t index = first + i;
		if (index >= view.rooms.size()) break;
		const Room& room = view.rooms[index];
		const int y = kRowTop + i * kRowStep;
		const std::string n = std::to_string(i);
		children.push_back(Text("Room" + n, Rect(90, y, 760, 60), RoomLine(room), Style("$MediumFontSize", "left", false)));
		children.push_back(Text("Players" + n, Rect(90, y + 60, 760, 70), PlayersLine(room), Style("$SmallFontSize", "left", true)));
		children.push_back(Button("Join" + n, 870, y + 10, "Join", "Join", n));
	}

	if (view.hosting) {
		children.push_back(Text("Hosting", Rect(90, 1230, 760, 120), view.hostingText, Style("$SmallFontSize", "left", true)));
		children.push_back(Button("CloseRoom", 870, 1250, "Close room", "CloseRoom"));
	} else {
		children.push_back(Text("NameLabel", Rect(90, 1200, 760, 50), "Room name (or the password of a locked room)", Style("$SmallFontSize", "left", false)));
		children.push_back("{\"type\":\"ImageWidget\",\"name\":\"NameBackground\",\"fields\":{\"rect\":" + Rect(60, 1240) +
			",\"filename\":\"PANEL\\\\TCP_IP\\\\TCP_IP_TextEntry\"}}");
		children.push_back(
			"{\"type\":\"InputTextBoxWidget\",\"name\":\"RoomName\",\"fields\":{\"rect\":" + Rect(100, 1275, 520, 60) +
			",\"backgroundColor\":{\"r\":0,\"g\":0,\"b\":0,\"a\":0},\"fontStyle\":{\"alignment\":{\"h\":\"center\",\"v\":\"top\"},"
			"\"pointSize\":\"$MediumFontSize\",\"fontColor\":\"$FontColorLightGold\"},\"fontType\":\"16pt\"}}");
		children.push_back(Button("Create", 870, 1260, "Create", "Create"));
	}

	const int y = 1390;
	if (page > 0) children.push_back(Button("Prev", 90, y, "Back", "Prev"));
	children.push_back(Button("Refresh", 480, y, "Refresh", "Refresh"));
	if (page + 1 < pages) children.push_back(Button("Next", 870, y, "More", "Next"));

	std::string out = "{\"type\":\"Panel\",\"name\":\"" + std::string(kPanelName) +
		"\",\"fields\":{\"anchor\":{\"x\":0.5,\"y\":0.1},\"rect\":{\"x\":" + std::to_string(kPanelX) +
		",\"width\":" + std::to_string(kPanelWidth) + ",\"height\":" + std::to_string(kPanelHeight) +
		"},\"priority\":8500},\"children\":[";
	for (size_t i = 0; i < children.size(); ++i) {
		if (i) out += ",";
		out += children[i];
	}
	return out + "]}";
}

}  // namespace rooms
