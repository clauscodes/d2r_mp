#include "layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

// The D2R Rooms window: in place of D2RLoader's Multiplayer window and about
// its width (its MainTabs are 1730 wide), built from the game's own pieces as
// D2RLoader's window is: no single art of the game is that wide, and an image
// only scales evenly. A dark backing with the game's gold edge (a
// RectangleWidget, as the game's Join Game list draws its selection), the
// tooltip frame's gold corners, the pause menu's gold rules, the game's title
// style, close button and front-end buttons. The rooms are a table: name,
// game, mod, players, connection, Join; the players' line under each name. In
// the front-end a plugin panel gets no clicks and no typing, so the plugin
// matches clicks against WindowButtons itself, and the window has no text
// field (a password goes into D2RLoader's Direct Join box).
constexpr int kWindowWidth = 1800, kWindowHeight = 1500;
constexpr int kWindowLeft = -kWindowWidth / 2;  // anchored at (0.5, 0.1)
constexpr int kWindowTop = 216;
constexpr int kInnerLeft = 70, kInnerRight = kWindowWidth - 70;
constexpr int kCloseX = kWindowWidth - 105, kCloseY = 18;  // closebtn_4x, about 70 x 70
// The table: a header row, then kRowsPerPage rows.
constexpr int kHeaderY = 228;
constexpr int kWindowRowTop = 300, kWindowRowStep = 150;
struct Column {
	const char* title;
	int x, width;
};
constexpr Column kColName {"Room", 90, 480}, kColGame {"Game", 590, 240}, kColMod {"Mod", 850, 310}, kColPlayers {"Players", 1180, 140};
constexpr int kJoinX = kInnerRight - kButtonWidth - 10;  // 1370
constexpr int kConnectionY = 60;                    // the second line of a row, right part
// The bottom: a hint, then the buttons on one row.
constexpr int kHintY = 1085, kButtonRowY = 1360;
constexpr int kLeftX = kInnerLeft, kMiddleX = kInnerLeft + kButtonWidth + 40, kRightX = kJoinX;

std::string Image(const std::string& name, const char* file, int x, int y, double scale = 1, int frame = -1) {
	char s[32];
	std::snprintf(s, sizeof(s), "%.3g", scale);
	return "{\"type\":\"ImageWidget\",\"name\":" + json::Quote(name) + ",\"fields\":{\"rect\":{\"x\":" + std::to_string(x) + ",\"y\":" +
		std::to_string(y) + (scale != 1 ? std::string(",\"scale\":") + s : "") + "},\"filename\":" + json::Quote(file) +
		(frame >= 0 ? ",\"frame\":" + std::to_string(frame) : "") + "}}";
}

std::string Rectangle(const std::string& name, int x, int y, int w, int h, const char* color, const char* border) {
	return "{\"type\":\"RectangleWidget\",\"name\":" + json::Quote(name) + ",\"fields\":{\"rect\":" + Rect(x, y, w, h) + ",\"color\":" + color +
		",\"borderColor\":" + border + "}}";
}

// The game's gold (its Join Game selection's edge) and a stone-dark backing.
constexpr const char* kGold = "[0.84,0.73,0.48,1.0]";
constexpr const char* kGoldFaint = "[0.84,0.73,0.48,0.35]";
constexpr const char* kBacking = "[0.035,0.03,0.025,0.94]";
constexpr const char* kRowShade = "[1.0,0.9,0.7,0.04]";
constexpr const char* kNone = "[0.0,0.0,0.0,0.0]";

std::vector<ButtonSpec> WindowButtons(const PanelView& view, int pages, int page) {
	std::vector<ButtonSpec> b;
	const size_t first = static_cast<size_t>(page) * kRowsPerPage;
	for (int i = 0; i < kRowsPerPage && first + i < view.rooms.size(); ++i)
		b.push_back({"Join" + std::to_string(i), kJoinX, kWindowRowTop + i * kWindowRowStep + 20, "Join", "Join", std::to_string(i)});
	b.push_back({"HostTab", kLeftX, kButtonRowY, "Host Game", "ToHost", ""});
	b.push_back({"BrowseTab", kMiddleX, kButtonRowY, "Browse Games", "ToBrowse", ""});
	if (view.hosting) b.push_back({"CloseRoom", kRightX, kButtonRowY, "Close room", "CloseRoom", ""});
	else if (pages > 1) b.push_back({"More", kRightX, kButtonRowY, "More", "Next", ""});
	else b.push_back({"Refresh", kRightX, kButtonRowY, "Refresh", "Refresh", ""});
	return b;
}

std::string WindowLayout(const PanelView& view, int pages, int page) {
	std::vector<std::string> children;
	// The frame.
	children.push_back(Rectangle("Background", 0, 0, kWindowWidth, kWindowHeight, kBacking, kGold));
	const double corner = 1.5;  // the tooltip frame's 48-unit corners: TL 0, TR 2, BL 6, BR 8
	const int c = static_cast<int>(48 * corner);
	children.push_back(Image("CornerTL", "PANEL\\TooltipBorder\\TooltipBorder", 0, 0, corner, 0));
	children.push_back(Image("CornerTR", "PANEL\\TooltipBorder\\TooltipBorder", kWindowWidth - c, 0, corner, 2));
	children.push_back(Image("CornerBL", "PANEL\\TooltipBorder\\TooltipBorder", 0, kWindowHeight - c, corner, 6));
	children.push_back(Image("CornerBR", "PANEL\\TooltipBorder\\TooltipBorder", kWindowWidth - c, kWindowHeight - c, corner, 8));
	children.push_back("{\"type\":\"TextBoxWidget\",\"name\":\"Title" + std::to_string(view.generation) + "\",\"fields\":{\"rect\":" +
		Rect(0, 30, kWindowWidth, 80) + ",\"text\":\"D2R Rooms\",\"style\":\"$StyleTitleBlock\"}}");
	children.push_back("{\"type\":\"ButtonWidget\",\"name\":\"Close\",\"fields\":{\"rect\":" + Rect(kCloseX, kCloseY) +
		",\"filename\":\"PANEL\\\\closebtn_4x\",\"hoveredFrame\":3}}");
	if (!view.status.empty())
		children.push_back(Text("Status", Rect(kInnerLeft, 120, kInnerRight - kInnerLeft, 80), view.status, Style("$SmallFontSize", "center", true)));
	const int ruleX = (kWindowWidth - 1663) / 2;  // PauseMenu\Divider is 1663 wide
	children.push_back(Image("RuleTop", "PauseMenu\\Divider", ruleX, 205));

	// The table's header and its rows.
	for (const Column& col : {kColName, kColGame, kColMod, kColPlayers})
		children.push_back(Text(std::string("Head") + col.title, Rect(col.x, kHeaderY, col.width, 50), col.title, Style("$SmallFontSize", "left", false)));
	children.push_back(Rectangle("HeadRule", kInnerLeft, kHeaderY + 58, kInnerRight - kInnerLeft, 2, kGoldFaint, kNone));
	const size_t first = static_cast<size_t>(page) * kRowsPerPage;
	for (int i = 0; i < kRowsPerPage && first + i < view.rooms.size(); ++i) {
		const Room& room = view.rooms[first + i];
		const RoomColumns cols = Columns(room);
		const int y = kWindowRowTop + i * kWindowRowStep;
		const std::string n = std::to_string(i);
		if (i % 2 == 0) children.push_back(Rectangle("Shade" + n, kInnerLeft, y, kInnerRight - kInnerLeft, kWindowRowStep - 10, kRowShade, kNone));
		// No empty text widgets (0.3.2: a room row with one never showed).
		const auto cell = [&](const std::string& name, int x, int dy, int w, const std::string& text, const char* size) {
			if (!text.empty()) children.push_back(Text(name, Rect(x, y + dy, w, 60), text, Style(size, "left", false)));
		};
		cell("Room" + n, kColName.x, 8, kColName.width, cols.name, "$MediumFontSize");
		cell("Game" + n, kColGame.x, 8, kColGame.width, cols.game, "$SmallFontSize");
		cell("Mod" + n, kColMod.x, 8, kColMod.width, cols.version, "$SmallFontSize");
		cell("Players" + n, kColPlayers.x, 8, kColPlayers.width, cols.players, "$SmallFontSize");
		// The second line: who plays under the name and the game (two lines at most), the connection under the mod.
		cell("Connection" + n, kColMod.x, kConnectionY + 8, kColPlayers.x + kColPlayers.width - kColMod.x, cols.connection, "$SmallFontSize");
		const std::string who = PlayersLine(room);
		if (!who.empty())
			children.push_back(Text("Who" + n, Rect(kColName.x, y + kConnectionY + 4, kColMod.x - 30 - kColName.x, 76), who, Style("$SmallFontSize", "left", true)));
	}

	// The bottom: a hint, the buttons.
	children.push_back(Image("RuleBottom", "PauseMenu\\Divider", ruleX, kHintY - 30));
	children.push_back(Text("Hint", Rect(kColName.x, kHintY, kInnerRight - kColName.x - 20, 230),
	                        view.hosting ? view.hostingText
	                                     : "Host with Create Game in Host Game: the game is listed here, through a relay when friends cannot reach "
	                                       "you. A room's password goes into Direct Join.",
	                        Style("$SmallFontSize", "left", true)));
	for (const ButtonSpec& b : WindowButtons(view, pages, page)) children.push_back(Button(b.name, b.x, b.y, b.label, b.command, b.argument));
	return Panel(PanelName(view.slot), "{\"x\":0.5,\"y\":0.1}", Rect(kWindowLeft, 0, kWindowWidth, kWindowHeight), kLoaderPriority, children);
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

ClickMap PictureMap(int windowW, int windowH, int pictureW, int pictureH, double safe) {
	if (pictureW <= 0 || pictureH <= 0) {
		pictureW = windowW;
		pictureH = windowH;
	}
	const double k = std::min(pictureW / 3840.0, pictureH / 2160.0) * safe;
	return {k, k, windowW / 2.0, windowH / 2.0 - 1080 * k};
}

double SafeShare(std::string_view settingsJson) {
	const auto doc = json::Parse(settingsJson);
	if (!doc) return 1.0;
	const int percent = (*doc)["Safe Screen Percent"].asInt(100);
	return std::clamp(percent, 50, 100) / 100.0;
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
	if (pictureW <= 0 || pictureH <= 0 || canvasW <= 0 || canvasH <= 0 || std::abs(canvasH - canvasW * 9 / 16) > 1) return false;
	// Wider than the picture's shape, by more than rounding.
	return static_cast<double>(canvasH) * pictureW < 0.98 * static_cast<double>(canvasW) * pictureH;
}

ClickMap BandMap(int windowW, int windowH, int pictureW, int pictureH, int canvasW, int canvasH, double safe) {
	// The answer is in the game's own pixels. When the picture's size is known
	// and the band is as wide as it, the picture is stretched over the window
	// on each axis; otherwise (the overlay gives the window's size, a render
	// size bigger than the window) the band spans the window's width and keeps
	// its shape.
	double sx = canvasW > 0 ? static_cast<double>(windowW) / canvasW : 1, sy = sx;
	if (pictureW > 0 && pictureH > 0 && pictureW == canvasW) {
		sx = static_cast<double>(windowW) / pictureW;
		sy = static_cast<double>(windowH) / pictureH;
	}
	const double k = std::min(canvasW / 3840.0, canvasH / 2160.0) * safe;
	return {k * sx, k * sy, canvasW / 2.0 * sx, (canvasH / 2.0 - 1080 * k) * sy};
}

std::optional<UiBandBlock> CheckUiBand(const UiBandBlock& block) {
	if (block.magic != kUiBandMagic || block.size != sizeof(UiBandBlock) || (block.seq & 1) != 0) return std::nullopt;
	if (block.active && (block.width <= 0 || block.height <= 0 || block.width > 32768 || block.height > 32768)) return std::nullopt;
	return block;
}

ClickMap SharedBandMap(const UiBandBlock& band, double safe) {
	if (band.safe > 0 && band.safe <= 1) safe = band.safe;
	const double cw = band.canvasW ? band.canvasW : band.width, ch = band.canvasH ? band.canvasH : band.height;
	const double k = std::min(cw / 3840.0, ch / 2160.0) * safe;  // in the game's pixels
	const double kx = k * band.width / cw, ky = k * band.height / ch;
	return {kx, ky, band.left + band.width / 2.0, band.top + band.height / 2.0 - 1080 * ky};
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
		if (v.size() != 11) continue;
		const auto i = [&](size_t n) { return static_cast<int>(std::lround(v[n])); };
		const MapKey key {i(0), i(1), i(2), i(3), i(4), i(5), i(6)};
		if (key.windowW <= 0 || key.windowH <= 0) continue;
		out.push_back({key, {v[7], v[8], i(9), i(10)}});
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
		    << s.key.canvasH << ' ' << s.key.safePercent << ' ' << s.sample.px << ' ' << s.sample.py << ' ' << s.sample.x << ' ' << s.sample.y << '\n';
	return out.str();
}

std::string TabLayout() {
	return Panel(kTabPanelName, "{\"x\":0.5,\"y\":0}", Rect(kTabX, kTabY, kButtonWidth, kButtonHeight), kLoaderPriority,
	             {Button("Tab", 0, 0, "D2R Rooms (F9)", "Tab")});
}

}  // namespace rooms
