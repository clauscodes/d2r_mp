// The rooms panel as a D2R layout (the game's own widgets, built as JSON).
// D2R widgets cannot change their text from a plugin, and D2R reads a panel's
// layout only the first time it opens in a session. So the plugin registers a
// pool of panels and shows every new layout in a panel not opened before.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "board.h"

namespace rooms {

inline constexpr const char* kPluginId = "d2r-rooms";
// The pool: panel "Rooms<slot>" with its layout at its own path.
inline constexpr int kPanelSlots = 64;
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

}  // namespace rooms
