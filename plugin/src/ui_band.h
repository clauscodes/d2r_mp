// Where the game's interface is in its window, as a VR mod tells D2R Rooms.
//
// A VR mod that lays the game's menus out somewhere other than the whole
// window (D2R VR's main menu as a 16:9 band at the window's top) publishes it
// in a small named shared-memory block, so D2R Rooms matches clicks against
// its own buttons where the game draws them. Plain C layout, little-endian,
// 48 bytes; no D2R Rooms code is needed to write it.
//
// Name: L"Local\\D2RRooms.UiBand.v1.<pid>", <pid> the game process's id in
// decimal (GetCurrentProcessId() inside the game), e.g.
// L"Local\\D2RRooms.UiBand.v1.27876". The writer creates it
// (CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 48,
// name)) and keeps the handle for the game's life; D2R Rooms opens it
// read-only on each click (OpenFileMappingW(FILE_MAP_READ, ...)).
//
// Writing (a sequence lock): seq += 1 (odd), write the fields, seq += 1
// (even). Write whenever the band, the window or `active` changes. D2R Rooms
// takes a copy only when seq is even and the same before and after it.
#pragma once

#include <cstdint>

namespace rooms {

inline constexpr uint32_t kUiBandMagic = 0x31424955;  // "UIB1"
inline constexpr const wchar_t* kUiBandNamePrefix = L"Local\\D2RRooms.UiBand.v1.";

struct UiBandBlock {
	uint32_t magic;    // kUiBandMagic
	uint32_t size;     // sizeof(UiBandBlock) = 48
	uint32_t seq;      // even when stable
	uint32_t active;   // 1: the game's interface is laid out in the rect below now; 0: as usual (the whole client area)
	int32_t  left;     // the rect the game lays its interface out in (its client-size answer), in the game
	int32_t  top;      // window's client pixels: where the cursor (GetCursorPos + ScreenToClient) is when
	int32_t  width;    // it is on the rect's corners. D2R VR's 16:9 main menu on a 1280x1280 window:
	int32_t  height;   // 0, 0, 1280, 720.
	uint32_t canvasW;  // the same rect in the game's own pixels (the client-size answer the game's interface
	uint32_t canvasH;  // gets, e.g. 1920 x 1080 on a 1920x1920 render size); 0 = the rect's own size
	float    safe;     // the interface's share of the rect (D2R's "Safe Screen Percent" / 100); 0 = unknown:
	                   // D2R Rooms then reads it from D2R's Settings.json
	uint32_t reserved; // 0
};
static_assert(sizeof(UiBandBlock) == 48);

}  // namespace rooms
