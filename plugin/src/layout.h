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
// px = cx + x * k, py = top + y * k.
struct ClickMap {
	double k = 0, cx = 0, top = 0;
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
// window's). The interface is 3840 x 2160 units fitted into the picture.
ClickMap PictureMap(int windowW, int windowH, int pictureW, int pictureH);
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
// The map the presses on this window size show, `guess` until there are any.
// The interface is a 16:9 box centred on the window (a flat game at a lower
// resolution, D2R VR's picture): one press gives its scale; presses at two
// heights give scale and top without that assumption.
ClickMap LearnMap(const ClickMap& guess, int windowW, int windowH, const std::vector<ClickSample>& samples);

}  // namespace rooms
