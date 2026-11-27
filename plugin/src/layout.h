// The rooms panel as a D2R layout (the game's own widgets, built as JSON).
// D2R widgets cannot change their text from a plugin, and D2R reads a panel's
// layout only the first time it opens in a session. So the plugin registers a
// pool of panels and shows every new layout in a panel not opened before.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "board.h"
#include "ui_band.h"

namespace rooms {

inline constexpr const char* kPluginId = "d2r-rooms";
// The pool: panel "Rooms<slot>" with its layout at its own path.
// D2RLoader limits a plugin's panels (65 did not all register): the D2R
// Rooms button and the TCP/IP panel's slot come first, then up to 48 more.
inline constexpr int kPanelSlots = 48;
std::string PanelLocalId(int slot);  // "Rooms7"
std::string PanelName(int slot);     // "d2r-rooms/Rooms7"
std::string PanelPath(int slot);     // "data/global/ui/layouts/d2r-rooms/Rooms7hd.json"
inline constexpr const char* kTabPanelLocalId = "RoomsTab";
inline constexpr const char* kTabPanelName = "d2r-rooms/RoomsTab";
inline constexpr const char* kTabPanelPath = "data/global/ui/layouts/d2r-rooms/RoomsTabhd.json";
// Target of the panel's own button messages ("D2RRooms:Join:3").
inline constexpr const char* kMessageTarget = "D2RRooms";
// Five rows fit the TCP/IP art's list box (its inner box ends near y 1014).
inline constexpr int kRowsPerPage = 5;

struct PanelView {
	std::vector<Room> rooms;  // the whole list; the page picks the rows
	int page = 0;
	std::string status;       // one line under the title
	bool hosting = false;
	std::string hostingText;  // shown instead of the create form while hosting
	// The D2R Rooms window, in place of D2RLoader's own Multiplayer window
	// with the same tabs: its CREATE GAME hosts, so no create form here.
	bool tab = false;
	// Names the title widget ("Title12"): the plugin checks which layout D2R shows.
	int generation = 0;
	int slot = 0;  // the pool's panel this layout is for
};

int PageCount(size_t roomCount);

// Text safe for a D2R text field: a leading '@' or '$' would be read as a
// string or style reference, so it gets a space in front.
std::string FieldText(std::string_view text);

std::string PanelLayout(const PanelView& view);

// The D2R Rooms tab button, beside BROWSE GAMES in D2RLoader's window.
std::string TabLayout();

// The tab button, in layout units (2160 high) from the top middle of the
// screen. D2RLoader's window is modal and takes the mouse, so the plugin
// hit-tests the cursor against it on a click.
struct HitArea {
	int x, y, width, height;
	std::string command, argument;  // as the button's own message
};
std::vector<HitArea> TabHitAreas();
// The D2R Rooms window's buttons and its close button: in the front-end a
// plugin panel gets no clicks at all, so the plugin matches them itself.
std::vector<HitArea> WindowHitAreas(const PanelView& view);

// The window's client area (real pixels) and layout units:
// px = cx + x * kx, py = top + y * ky. The two scales differ when the game's
// picture is stretched over the window (D2R VR's render size: the game's
// cursor is the window's, scaled on each axis to the picture).
struct ClickMap {
	double kx = 0, ky = 0, cx = 0, top = 0;
};
struct LayoutPoint {
	int x, y;
};
struct PixelPoint {
	double x, y;
};
LayoutPoint ToLayout(const ClickMap& map, double px, double py);
PixelPoint FromLayout(const ClickMap& map, double x, double y);

// The first guess: the game lays its interface out on its own picture
// (`pictureW` x `pictureH`, the overlay's screen size; 0 = the window's),
// centred on the window when the two differ (a game resolution below the
// window's). The interface is 3840 x 2160 units fitted into the picture and
// centred on it, shrunk to `safe` of that round its middle (the game's
// "Safe Screen Percent", SafeShare).
ClickMap PictureMap(int windowW, int windowH, int pictureW, int pictureH, double safe = 1.0);
// D2R's Settings.json "Safe Screen Percent" (Options: the screen's safe area)
// as a share, 0.5..1; 1 without it. The game draws its whole interface, D2R
// and D2RLoader's windows included, that much smaller round the middle.
double SafeShare(std::string_view settingsJson);
LayoutPoint ToLayout(double px, double py, int windowW, int windowH, int pictureW, int pictureH);

// A press D2RLoader's window answered (its HOST GAME tab, CREATE GAME...): the
// window pixel pressed and the middle of that button in layout units.
struct ClickSample {
	double px, py;
	int x, y;
};
// Where the D2RLoader buttons whose presses teach the map are (their middles):
// ShowHost, ShowBrowse, CreateGame, JoinDirect. Measured on flat games.
std::optional<LayoutPoint> LoaderTarget(std::string_view command);
// The game's client-size query (GetClientRect of its window, the size the
// game lays its interface out in) answered `canvasW` x `canvasH` on a
// `pictureW` x `pictureH` picture (the overlay's size, else the window's): a
// 16:9 answer wider than the picture's shape is D2R VR's menus laid out at
// 16:9 at the picture's top. The answer is in the game's own pixels, which
// may be bigger than the window (a 1920x1080 band for a 1920 render size in a
// 1280 window), so only its shape is compared, never the window's size alone.
bool IsBand(int pictureW, int pictureH, int canvasW, int canvasH);
// The map for that band: 3840 x 2160 units fitted into the band from the
// window's top and shrunk to `safe` round the band's middle. The band spans
// the window's width (stretched on each axis with the picture when it is as
// wide as the known picture).
ClickMap BandMap(int windowW, int windowH, int pictureW, int pictureH, int canvasW, int canvasH, double safe = 1.0);

// A VR mod's word on where the interface is (ui_band.h): the block when it is
// whole and consistent, else none.
std::optional<UiBandBlock> CheckUiBand(const UiBandBlock& block);
// The map for an active block: 3840 x 2160 units fitted into its canvas,
// shrunk to its safe share (or `safe` when it gives none) round the middle,
// and placed on its rect in window pixels.
ClickMap SharedBandMap(const UiBandBlock& band, double safe);

// The presses' rows must be this far apart (units) to give a height.
inline constexpr int kMinRowSpan = 300;
// The map the presses show, `guess` until they lie on two rows at least:
// presses on one row (D2RLoader's HOST GAME and BROWSE GAMES tabs) cannot tell
// the interface's top from its scale. The rows give its height and top; its
// width is the guess's shape when the height is the guess's, and the window's
// whole width when it is taller than the guess: the picture is stretched over
// the window (D2R VR), and the interface spans a picture narrower than 16:9
// from side to side. Centred across the window.
ClickMap LearnMap(const ClickMap& guess, int windowW, int windowH, const std::vector<ClickSample>& samples);

// What the presses are kept apart by: the window, the game's picture, the
// client-size query's answer (0 = unknown) and the safe area's percent. A menu
// laid out square and the same window's menu laid out as a 16:9 band are
// different maps.
struct MapKey {
	int windowW = 0, windowH = 0, pictureW = 0, pictureH = 0, canvasW = 0, canvasH = 0, safePercent = 100;
	bool operator==(const MapKey&) const = default;
};
// d2r-rooms-clicks.txt: one press a line, "windowW windowH pictureW pictureH
// canvasW canvasH safePercent px py x y". Lines of 0.9.12 and older (6
// numbers, keyed by the window alone) and of 0.9.13 (10, no safe area) are
// dropped: they mixed layouts.
struct KeyedSample {
	MapKey key;
	ClickSample sample;
};
std::vector<KeyedSample> ParseClickSamples(std::string_view text);

// Where the game's client-size query starts in `code` (D2R.exe's .text as
// read): sub rsp,38h; call <the window>; lea rdx,[rsp+20h]; mov rcx,rax; call
// <GetClientRect>; mov eax,[rsp+28h]; mov [rsp+40h],eax. Its first 14 bytes
// may be a VR mod's jump (D2R VR and Unleashed hook it); the rest must match.
// Every match's offset (the caller uses it only when there is exactly one).
std::vector<size_t> FindClientSizeQuery(std::string_view code);
std::string ClickSamplesText(const std::vector<KeyedSample>& samples);

}  // namespace rooms
