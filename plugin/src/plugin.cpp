// D2R Rooms: the board's room list inside the game, next to D2R's own TCP/IP
// screen. Join types the host's address into D2R's Join Game box and presses
// OK; Create lists a room and opens D2R's Host Game path. Everything goes
// through the game's own screens and messages - nothing in a game changes.
//
// D2RLoader 1.3.1's Multiplayer button opens its own window instead
// (D2RLoaderMultiplayerPanel: HOST GAME / BROWSE GAMES). Then the list opens
// beside that window, its CREATE GAME lists the room (and opens the relay),
// and Join goes through its Direct Join box.
//
// When D2RLoader reports that the host's port cannot be reached from outside,
// the host's room goes through a relay (relay/PROTOCOL.md): both PCs dial out
// to it, the joiner's game joins a listener on loopback, and the bytes are
// piped as they are. The relay list is d2r-rooms.ini beside this DLL.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include <D2RLPlugin/api.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "board.h"
#include "json.h"
#include "layout.h"
#include "relay_config.h"
#include "relay_net.h"

namespace {

using namespace rooms;

constexpr D2RL::PluginInfo kInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = kPluginId,
	.name        = "D2R Rooms",
	.version     = "0.9.6",
	.author      = "D2R Rooms",
	.description = "Open TCP/IP rooms: list, join and create beside the Multiplayer or TCP/IP screen.",
	.flags       = D2RL::PluginFlags::Client,
};

const D2RL::PluginContext*      g_ctx = nullptr;
const D2RL::PanelService*       g_panels = nullptr;
const D2RL::ResourceService*    g_resources = nullptr;
const D2RL::SharedEventService* g_events = nullptr;
const D2RL::WidgetService*      g_widgets = nullptr;
const D2RL::ThreadService*      g_threads = nullptr;
const D2RL::HttpService*        g_http = nullptr;
const D2RL::LifecycleService*   g_life = nullptr;

// The pool of rooms panels (layout.h); g_slot is the one shown or last shown.
D2RL::Panels::RegistrationHandle    g_pool[kPanelSlots] {};
D2RL::Resources::RegistrationHandle g_poolLayout[kPanelSlots] {};
int g_slot = -1;
int g_nextSlot = 0;
int g_poolSize = 0;  // as many as D2RLoader took, up to kPanelSlots
D2RL::Panels::RegistrationHandle    g_tabPanel = D2RL::Panels::InvalidHandle;
D2RL::Resources::RegistrationHandle g_tabLayout = D2RL::Resources::InvalidHandle;

std::string g_server = kDefaultServer;
Mod g_mod;

// Shared between the UI thread, HTTP callbacks and the heartbeat thread.
std::mutex g_mu;
std::vector<Room> g_rooms;
std::string g_status = "Loading the rooms...";
int g_page = 0;
std::optional<Hosted> g_hosted;

// D2RLoader's own Multiplayer window and its messages (D2RCore.dll 1.3.1).
constexpr const char* kLoaderPanel = "D2RLoaderMultiplayerPanel";
constexpr const char* kLoaderMessage = "D2RLoaderMultiplayerPanelMessage";

// UI thread only.
bool g_panelOpen = false;
bool g_loaderUi = false;  // D2RLoader's Multiplayer window is open
bool g_tabButtonOpen = false;  // the D2R Rooms tab button beside its BROWSE GAMES
bool g_tabActive = false;      // the D2R Rooms window is open in place of D2RLoader's
// Join was pressed on this locked room: its password goes into D2RLoader's
// Direct Join box, and that box's Join is taken over (OnUiMessage).
std::optional<Room> g_lockedJoin;
std::atomic<bool> g_resendJoinDirect {false};  // the join then presses Direct Join's Join again
std::chrono::steady_clock::time_point g_resendUntil;  // that press is not taken over again
// The address a Direct Join the plugin started must carry (127.0.0.1 through a
// relay): its Join does not go out with another one (on 0.8.1 D2RLoader's
// remembered address stayed in the box and the game went there).
std::string g_joinAddress;
int g_noticeSlot = -1;  // the pool panel showing a notice at the top, or -1
struct JoinFlow {
	int step = 0;  // 0 = idle
	int ticks = 0;
	std::string address;
	bool loader = false;  // through D2RLoader's Direct Join instead of D2R's Join Game
	bool press = true;    // press its Join (a locked room: the player does, after the password)
	size_t typed = 0;     // characters of `address` typed so far, one a frame
	int tries = 0;        // clicks into the box so far
} g_join;

std::atomic<int> g_act {0};
std::atomic<int> g_level {0};
std::atomic<bool> g_inGame {false};
std::atomic<bool> g_hostedGameStarted {false};

std::thread g_heartbeat;
std::mutex g_heartbeatMu;
std::condition_variable g_heartbeatCv;
bool g_heartbeatStop = false;

// Relays: d2r-rooms.ini, read at load and again on every CREATE GAME (D2R VR
// Settings may have changed it). Under g_mu; threads take a copy.
RelayConfig g_relayCfg;
std::string g_gameDir;  // the folder of D2R.exe, with a trailing backslash
// Under g_mu: each relay's last /healthz answer, and when it came.
struct RelayCheck {
	RelayHealth health;
	std::chrono::steady_clock::time_point at;
};
std::map<std::string, RelayCheck> g_relayStates;
std::shared_ptr<net::HostRelay> g_hostRelay;  // the host's relay, while hosting through one
std::string g_hostRelayName;
std::shared_ptr<net::JoinRelay> g_joinRelay;  // the joiner's pipe, while joining through one
std::atomic<int> g_hostSession {0};          // bumps on every Create and StopHosting

void Log(const char* format, auto... args) {
	D2RL::LogInfoF(g_ctx, format, args...);
}

void LogLine(const std::string& line) {
	D2RL::LogInfoF(g_ctx, "%s", line.c_str());
}

std::string ReadText(const std::string& path, size_t from = 0, size_t* size = nullptr) {
	// D2RLoader keeps its log open for writing: share both ways.
	HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return {};
	LARGE_INTEGER len {};
	GetFileSizeEx(f, &len);
	if (size) *size = static_cast<size_t>(len.QuadPart);
	std::string out;
	if (static_cast<size_t>(len.QuadPart) > from && static_cast<size_t>(len.QuadPart) - from < (64u << 20)) {
		LARGE_INTEGER pos {};
		pos.QuadPart = static_cast<LONGLONG>(from);
		SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
		out.resize(static_cast<size_t>(len.QuadPart) - from);
		DWORD read = 0;
		::ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
		out.resize(read);
	}
	CloseHandle(f);
	return out;
}

std::string DllDir() {
	HMODULE self = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   reinterpret_cast<LPCSTR>(&DllDir), &self);
	char path[MAX_PATH] {};
	GetModuleFileNameA(self, path, MAX_PATH);
	std::string dir(path);
	return dir.substr(0, dir.find_last_of("\\/") + 1);
}

std::string ExeDir() {
	char path[MAX_PATH] {};
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	std::string dir(path);
	return dir.substr(0, dir.find_last_of("\\/") + 1);
}

// D2RLoader's game_port, read again before each host or join: the player may change it.
int GamePort() {
	return ParseGamePort(ReadText(g_gameDir + "d2rloader\\config\\d2rloader.toml"));
}

std::string LoaderLog() {
	return g_gameDir + "d2rloader\\logs\\d2rloader.log";
}

// The mod's plugin version (D2R VR 0.189.0) from D2RLoader's startup lines,
// read once it is there: the board shows it, and joining needs the same one.
void ReadModVersion() {
	if (!g_mod.version.empty()) return;
	g_mod.version = ModPluginVersion(ReadText(LoaderLog()));
	if (!g_mod.version.empty()) Log("mod: %s", ModLabel(g_mod).c_str());
}

// --- threads ------------------------------------------------------------

void __cdecl RunQueued(const D2RL::PluginContext*, void* userData) noexcept {
	auto* fn = static_cast<std::function<void()>*>(userData);
	try {
		(*fn)();
	} catch (...) {
	}
	delete fn;
}

// Runs `fn` during a later UI update (widgets and panels need that thread).
void OnUi(std::function<void()> fn) {
	auto* queued = new std::function<void()>(std::move(fn));
	if (g_threads->runOnUiThread(g_ctx, RunQueued, queued) != D2RL::Threads::Result::Success) delete queued;
}

// --- HTTP ---------------------------------------------------------------

using HttpDone = std::function<void(int status, const std::string& body)>;

void __cdecl OnResponse(const D2RL::PluginContext*, const D2RL::Http::Response* response, void* userData) noexcept {
	auto* done = static_cast<HttpDone*>(userData);
	try {
		const bool ok = response->result == D2RL::Http::Result::Success;
		const std::string body = ok && response->body ? std::string(static_cast<const char*>(response->body), response->bodySize) : std::string();
		if (!ok) Log("http: transport failure %u", static_cast<unsigned>(response->result));
		(*done)(ok ? static_cast<int>(response->statusCode) : 0, body);
	} catch (...) {
	}
	delete done;
}

void Send(D2RL::Http::Method method, const std::string& path, const std::string& body, const std::string& token, HttpDone done) {
	const std::string url = g_server + path;
	const std::string auth = token.empty() ? std::string() : "Bearer " + token;
	std::vector<D2RL::Http::Header> headers {{"content-type", "application/json"}};
	if (!auth.empty()) headers.push_back({"authorization", auth.c_str()});
	const D2RL::Http::Request request {
		.structSize      = D2RL::Http::RequestSize,
		.flags           = 0,
		.method          = method,
		.timeoutMs       = 15'000,
		.url             = url.c_str(),
		.headerCount     = static_cast<uint32_t>(headers.size()),
		.bodySize        = static_cast<uint32_t>(body.size()),
		.headers         = headers.data(),
		.body            = body.empty() ? nullptr : body.data(),
		.maxResponseSize = 0,
		.reserved        = 0,
	};
	auto* pending = new HttpDone(std::move(done));
	D2RL::Http::RequestHandle handle = D2RL::Http::InvalidRequestHandle;
	if (g_http->send(g_ctx, &request, OnResponse, pending, &handle) != D2RL::Http::Result::Success) {
		Log("http: could not send %s", path.c_str());
		(*pending)(0, {});
		delete pending;
	}
}

std::string Explain(int status, const std::string& body) {
	if (status == 0) return "The rooms server does not answer.";
	const std::string error = ParseError(body);
	if (error == "bad_password") return "Wrong password: type it into the field below, then Join.";
	if (error == "too_many_attempts") return "Too many wrong passwords - wait a minute.";
	if (error == "no_room") return "That room is gone.";
	if (error == "too_many_rooms") return "You already host 3 rooms.";
	if (error == "too_many_creates") return "Too many rooms created - wait a minute.";
	if (error == "invalid") return "The server did not accept that name.";
	return "The rooms server answered " + std::to_string(status) + ".";
}

// --- the panel ----------------------------------------------------------

void SetStatus(std::string status) {
	std::lock_guard lock(g_mu);
	g_status = std::move(status);
}

bool Hosting() {
	std::lock_guard lock(g_mu);
	return g_hosted.has_value();
}

// Rebuilds the layout from the current list and, when the panel is open,
// reopens it so D2R reads the new layout. UI thread.
RelayState StateOf(const std::string& url) {  // under g_mu
	const auto it = g_relayStates.find(url);
	return it == g_relayStates.end() ? RelayState::Unknown : it->second.health.state;
}

// "Relays: D2R Rooms active, Friends not answering"
std::string RelaysLine() {  // under g_mu
	std::string line;
	for (const RelayServer& r : g_relayCfg.relays) {
		line += line.empty() ? "Relays: " : ", ";
		line += r.name + " " + RelayStateLabel(StateOf(r.url));
	}
	if (!g_relayCfg.enabled) line += " (off: enabled = no)";
	else if (g_relayCfg.mode == RelayMode::Never) line += " (off: mode = never)";
	return line;
}

PanelView CurrentView();
void ApplyLayoutText(PanelView& view);
void OpenLater(int gen, int ticks);
int g_layoutGen = 0;  // bumps with every layout, UI thread

// d2r-rooms-status.json beside the DLL, for D2R VR Settings' Multiplayer tab:
// rewritten with every layout (a list, a relay check, hosting). Any thread.
void WriteStatusFile() {
	BridgeStatus s;
	s.plugin = kInfo.version;
	s.board = g_server;
	SYSTEMTIME t {};
	GetSystemTime(&t);
	char when[32];
	std::snprintf(when, sizeof(when), "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	s.updated = when;
	{
		std::lock_guard lock(g_mu);
		s.config = g_relayCfg;
		for (const RelayServer& r : g_relayCfg.relays) {
			const auto it = g_relayStates.find(r.url);
			s.health.push_back(it == g_relayStates.end() ? RelayHealth {} : it->second.health);
		}
		s.rooms = g_rooms;
		for (Room& r : s.rooms)
			if (!r.relayUrl.empty()) r.relayState = StateOf(r.relayUrl);
		if (g_hosted) s.hostingCode = g_hosted->code;
		if (g_hosted && g_hostRelay) s.hostingRelay = g_hostRelayName;
	}
	const std::string path = DllDir() + "d2r-rooms-status.json";
	const std::string tmp = path + ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		f << StatusJson(s);
		if (!f) return;
	}
	MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void ApplyLayout() {
	WriteStatusFile();
	PanelView view = CurrentView();
	view.generation = ++g_layoutGen;
	ApplyLayoutText(view);
}

PanelView CurrentView() {
	PanelView view;
	{
		std::lock_guard lock(g_mu);
		view.rooms = g_rooms;
		for (Room& r : view.rooms)
			if (!r.relayUrl.empty()) r.relayState = StateOf(r.relayUrl);
		view.page = g_page;
		view.status = g_status.empty() ? RelaysLine() : g_status + "     " + RelaysLine();
		view.tab = g_tabActive;
		view.hosting = g_hosted.has_value();
		if (g_hosted && g_hostRelay)
			view.hostingText = "You host room [" + g_hosted->code + "]. Your port cannot be reached from outside, so friends join through relay " +
				g_hostRelayName + " (" + RelayStateLabel(StateOf(g_hostRelay->Ref().url)) + "). The room closes when you leave the game.";
		else if (g_hosted)
			view.hostingText = "You host room [" + g_hosted->code + "]. Players connect to " + g_hosted->address +
				". Pick your character and start the game; the room closes when you leave it.";
	}
	return view;
}

// Every new layout goes to the pool's next panel, one D2R has not opened yet
// this session: D2R reads a panel's layout only on its first open, and showed
// "Loading the rooms..." for good otherwise. After the whole pool, the oldest
// panel comes again (it may show an old layout; the log says so).
PanelView g_shownView;  // the layout last given to D2R: its buttons are matched to clicks

void ApplyLayoutText(PanelView& view) {
	if (g_panelOpen && g_slot >= 0) g_panels->closePanel(g_ctx, g_pool[g_slot]);
	const int slot = g_nextSlot++ % g_poolSize;
	if (g_nextSlot == g_poolSize + 1) Log("panel: the pool of %d panels is used up; reusing them", g_poolSize);
	view.slot = slot;
	g_slot = slot;
	g_shownView = view;
	const std::string layout = PanelLayout(view);
	const std::string path = PanelPath(slot);
	if (g_poolLayout[slot] != D2RL::Resources::InvalidHandle) g_resources->unregisterResource(g_ctx, g_poolLayout[slot]);
	g_poolLayout[slot] = D2RL::Resources::InvalidHandle;
	const D2RL::Resources::ResourceRegistration resource {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.flags      = 0,
		.path       = path.c_str(),
		.bytes      = layout.data(),
		.byteCount  = layout.size(),
	};
	const auto registered = g_resources->registerResource(g_ctx, &resource, &g_poolLayout[slot]);
	if (registered != D2RL::Resources::Result::Success) Log("layout: register failed %u", static_cast<unsigned>(registered));
	if (g_panelOpen) OpenLater(view.generation, 1);
}

// Opens the current panel `ticks` UI updates from now if it is still meant to
// be open with layout `gen`, and logs a frame later whether D2R shows that
// layout (its title widget carries the number).
void OpenLater(int gen, int ticks) {
	if (gen != g_layoutGen || !g_panelOpen) return;
	if (ticks > 0) {
		OnUi([=] { OpenLater(gen, ticks - 1); });
		return;
	}
	const int slot = g_slot;
	const auto opened = g_panels->openPanel(g_ctx, g_pool[slot]);
	if (opened != D2RL::Panels::Result::Success) Log("panel: open failed %u", static_cast<unsigned>(opened));
	OnUi([gen, slot] {
		if (gen != g_layoutGen || !g_panelOpen) return;
		D2RL::Widgets::WidgetHandle panel = D2RL::Widgets::InvalidHandle, title = D2RL::Widgets::InvalidHandle;
		const bool shown = g_widgets->findPanel(g_ctx, PanelName(slot).c_str(), &panel) == D2RL::Widgets::Result::Success &&
		                   g_widgets->findWidget(g_ctx, panel, ("Title" + std::to_string(gen)).c_str(), &title) == D2RL::Widgets::Result::Success;
		Log("panel: layout %d in panel %d %s", gen, slot, shown ? "shown" : "NOT shown");
	});
}

void ShowPanel(bool show) {
	if (show == g_panelOpen) return;
	g_panelOpen = show;
	if (show) {
		ApplyLayout();
		Log("panel: open");
	} else if (g_slot >= 0) {
		const auto closed = g_panels->closePanel(g_ctx, g_pool[g_slot]);
		Log("panel: close -> %u", static_cast<unsigned>(closed));
	}
}

// The current panel's name, for reading its password field.
std::string CurrentPanel() {
	return PanelName(std::max(g_slot, 0));
}

void Refresh();

// --- the D2R Rooms window in place of D2RLoader's Multiplayer window --------
// D2RLoader builds its window in its code and it is modal, so the D2R Rooms
// tab beside its BROWSE GAMES is a panel of its own (clicks matched by
// HandleClick), and pressing it swaps that window for ours: the same size and
// spot, the same tabs, an ordinary panel. HOST GAME / BROWSE GAMES swap back.

bool Dispatch(const char* target, const char* command, const char* text);
// Until then a close of D2RLoader's window is the one EnterTab sends.
std::chrono::steady_clock::time_point g_swapUntil;

void ShowTabButton(bool show) {
	if (show == g_tabButtonOpen) return;
	const auto r = show ? g_panels->openPanel(g_ctx, g_tabPanel) : g_panels->closePanel(g_ctx, g_tabPanel);
	Log("tab: button %s -> %u", show ? "open" : "close", static_cast<unsigned>(r));
	g_tabButtonOpen = show && r == D2RL::Panels::Result::Success;
}

void EnterTab() {
	if (g_tabActive) return;
	g_tabActive = true;
	SetStatus("Loading the rooms...");
	OnUi([] {
		g_swapUntil = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		Dispatch(kLoaderMessage, "Close", "");
		ShowPanel(true);
		Refresh();
	});
}

// D2RLoader's window closed (or D2R left the front-end): its tab button with it,
// and the D2R Rooms window unless it is replacing that window.
void HideNotice();

void CloseLoaderUi() {
	g_loaderUi = false;
	ShowTabButton(false);
	HideNotice();
	if (std::chrono::steady_clock::now() < g_swapUntil) return;
	g_tabActive = false;
	ShowPanel(false);
}

// Back to D2RLoader's window on one of its tabs ("ShowHost", "ShowBrowse").
void OpenLoaderWindow(const char* tab) {
	g_tabActive = false;
	ShowPanel(false);
	OnUi([tab] {
		Dispatch("PanelManager", "OpenPanel", kLoaderPanel);
		OnUi([tab] { Dispatch(kLoaderMessage, tab, ""); });
	});
}

// Asks every relay in the list, and every relay a listed room uses, for its
// status (at most once a minute each), then redraws. On its own thread.
void CheckRelays() {
	std::vector<std::string> urls;
	{
		std::lock_guard lock(g_mu);
		for (const RelayServer& r : g_relayCfg.relays) urls.push_back(r.url);
		for (const Room& r : g_rooms)
			if (!r.relayUrl.empty() && std::find(urls.begin(), urls.end(), r.relayUrl) == urls.end()) urls.push_back(r.relayUrl);
		const auto now = std::chrono::steady_clock::now();
		std::erase_if(urls, [&](const std::string& u) {
			const auto it = g_relayStates.find(u);
			return it != g_relayStates.end() && now - it->second.at < std::chrono::seconds(60);
		});
	}
	if (urls.empty()) return;
	std::thread([urls] {
		for (const std::string& url : urls) {
			const RelayHealth h = net::CheckRelay(url);
			std::lock_guard lock(g_mu);
			g_relayStates[url] = {h, std::chrono::steady_clock::now()};
		}
		OnUi(ApplyLayout);
	}).detach();
}

void Refresh() {
	CheckRelays();
	Send(D2RL::Http::Method::Get, "/api/rooms", {}, {}, [](int status, const std::string& body) {
		auto list = status == 200 ? ParseRooms(body) : std::nullopt;
		{
			std::lock_guard lock(g_mu);
			if (list) {
				g_rooms = std::move(*list);
				g_page = std::min(g_page, PageCount(g_rooms.size()) - 1);
				g_status = g_rooms.empty() ? "No open rooms right now."
				                           : std::to_string(g_rooms.size()) + " open room(s) - page " + std::to_string(g_page + 1) +
				                                 "/" + std::to_string(PageCount(g_rooms.size()));
			} else {
				g_status = Explain(status, body);
			}
		}
		Log("rooms: %d -> %s", status, list ? "ok" : "failed");
		CheckRelays();  // rooms may name relays not asked yet
		OnUi(ApplyLayout);
	});
}

// Text of an InputTextBoxWidget, or "" when it cannot be read. UI thread.
std::string ReadInput(const char* panel, std::initializer_list<const char*> path) {
	D2RL::Widgets::WidgetHandle h = D2RL::Widgets::InvalidHandle;
	if (g_widgets->findPanel(g_ctx, panel, &h) != D2RL::Widgets::Result::Success) return {};
	for (const char* name : path) {
		D2RL::Widgets::WidgetHandle child = D2RL::Widgets::InvalidHandle;
		if (g_widgets->findWidget(g_ctx, h, name, &child) != D2RL::Widgets::Result::Success) return {};
		h = child;
	}
	char buffer[256] {};
	uint32_t required = 0;
	if (g_widgets->getInputText(g_ctx, h, buffer, sizeof(buffer), &required) != D2RL::Widgets::Result::Success) return {};
	return buffer;
}

bool Dispatch(const char* target, const char* command, const char* text = "") {
	const D2RL::Widgets::UiAction action {
		.structSize = D2RL::Widgets::UiActionSize,
		.flags      = 0,
		.target     = target,
		.command    = command,
		.text       = text,
	};
	const auto r = g_widgets->dispatchUiAction(g_ctx, &action);
	Log("dispatch %s:%s -> %u", target, command, static_cast<unsigned>(r));
	return r == D2RL::Widgets::Result::Success;
}

// --- typing into D2R's own Join Game box ---------------------------------

BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM out) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
		*reinterpret_cast<HWND*>(out) = hwnd;
		return FALSE;
	}
	return TRUE;
}

HWND GameWindow() {
	HWND hwnd = nullptr;
	EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&hwnd));
	return hwnd;
}

// Clicks one of D2RLoader's widgets as the player would: its text boxes take
// the keyboard only after a click (typing into the open Direct Join box went
// nowhere on 0.8.1). Its rect is in that window's units from the screen's top
// middle, the child's relative to `container`. The cursor goes back after.
bool ClickLoaderWidget(const char* container, const char* name) {
	D2RL::Widgets::WidgetHandle panel = D2RL::Widgets::InvalidHandle, c = D2RL::Widgets::InvalidHandle, w = D2RL::Widgets::InvalidHandle;
	D2RL::Widgets::Rect rc {}, rw {};
	if (g_widgets->findPanel(g_ctx, kLoaderPanel, &panel) != D2RL::Widgets::Result::Success ||
	    g_widgets->findWidget(g_ctx, panel, container, &c) != D2RL::Widgets::Result::Success ||
	    g_widgets->getWidgetRect(g_ctx, c, &rc) != D2RL::Widgets::Result::Success ||
	    g_widgets->findWidget(g_ctx, panel, name, &w) != D2RL::Widgets::Result::Success ||
	    g_widgets->getWidgetRect(g_ctx, w, &rw) != D2RL::Widgets::Result::Success) {
		Log("click: %s/%s not found", container, name);
		return false;
	}
	HWND hwnd = GameWindow();
	RECT client {};
	if (!hwnd || !GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0) return false;
	const double scale = std::min(client.right / 3840.0, client.bottom / 2160.0);
	// D2RLoader reports its Direct Join box (DirectJoinContainer) at 0,0, while
	// the fields' rects are from the box's corner. Measured in the game: the
	// player's click on its Join Game at -209,1181 and that button at 355,444 in
	// the box put the corner at -564,737 (the box is centred, 1128 wide).
	if (rc.x == 0 && rc.y == 0 && std::string_view(container) == "DirectJoinContainer") {
		rc.x = -564;
		rc.y = 737;
	}
	const int x = rc.x + rw.x + rw.width / 2, y = rc.y + rw.y + rw.height / 2;
	POINT p {static_cast<LONG>(client.right / 2.0 + x * scale), static_cast<LONG>(y * scale)};
	Log("click: %s %d,%d %dx%d in %s %d,%d -> %d,%d px", name, rw.x, rw.y, rw.width, rw.height, container, rc.x, rc.y, p.x, p.y);
	ClientToScreen(hwnd, &p);
	POINT old {};
	GetCursorPos(&old);
	SetCursorPos(p.x, p.y);
	INPUT in[2] {};
	in[0].type = in[1].type = INPUT_MOUSE;
	in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
	in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
	SendInput(2, in, sizeof(INPUT));
	std::thread([old] {
		Sleep(300);
		SetCursorPos(old.x, old.y);
	}).detach();
	return true;
}

// A line at the top of the screen, over D2RLoader's window, in a pool panel.
void HideNotice() {
	if (g_noticeSlot < 0) return;
	g_panels->closePanel(g_ctx, g_pool[g_noticeSlot]);
	g_noticeSlot = -1;
}

void ShowNotice(const std::string& text) {
	HideNotice();
	if (g_poolSize == 0) return;
	const int slot = g_nextSlot++ % g_poolSize;
	if (slot == g_slot) return;
	const std::string layout = "{\"type\":\"Panel\",\"name\":" + json::Quote(PanelName(slot)) +
		",\"fields\":{\"anchor\":{\"x\":0.5,\"y\":0},\"rect\":{\"x\":-1200,\"y\":30,\"width\":2400,\"height\":100},\"priority\":2100000001},"
		"\"children\":[{\"type\":\"TextBoxWidget\",\"name\":\"Notice\",\"fields\":{\"rect\":{\"x\":0,\"y\":0,\"width\":2400,\"height\":100},"
		"\"text\":" + json::Quote(FieldText(text)) + ",\"style\":{\"fontColor\":\"$FontColorLightGold\",\"pointSize\":\"$LargeFontSize\","
		"\"alignment\":{\"h\":\"center\",\"v\":\"center\"}}}}]}";
	if (g_poolLayout[slot] != D2RL::Resources::InvalidHandle) g_resources->unregisterResource(g_ctx, g_poolLayout[slot]);
	g_poolLayout[slot] = D2RL::Resources::InvalidHandle;
	const std::string path = PanelPath(slot);
	const D2RL::Resources::ResourceRegistration resource {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.flags      = 0,
		.path       = path.c_str(),
		.bytes      = layout.data(),
		.byteCount  = layout.size(),
	};
	g_resources->registerResource(g_ctx, &resource, &g_poolLayout[slot]);
	g_noticeSlot = slot;
	OnUi([slot] {
		if (g_noticeSlot == slot) g_panels->openPanel(g_ctx, g_pool[slot]);
	});
	Log("notice: %s", text.c_str());
}

// Clears the focused text box (48 backspaces), as keyboard input.
void ClearFocusedText() {
	HWND hwnd = GameWindow();
	if (!hwnd) return;
	for (int i = 0; i < 48; ++i) {
		PostMessageW(hwnd, WM_KEYDOWN, VK_BACK, 0x000E0001);
		PostMessageW(hwnd, WM_KEYUP, VK_BACK, 0xC00E0001);
	}
}

// One character into the focused text box. The game takes one a frame: a
// burst of them left only the first in Direct Join's box on 0.9.1.
void TypeChar(char c) {
	if (HWND hwnd = GameWindow()) PostMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(static_cast<unsigned char>(c)), 1);
}

// Pastes `text` into the focused text box: the player's clipboard is set to
// it, Ctrl+V pressed, and the clipboard put back a second later. Typed one
// character at a time, the game dropped some ("127.0.1" on 0.9.3).
bool PasteText(const std::string& text) {
	HWND hwnd = GameWindow();
	if (!hwnd || !OpenClipboard(hwnd)) return false;
	std::wstring old;
	if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
		if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
			old = p;
			GlobalUnlock(h);
		}
	}
	const std::wstring wide(text.begin(), text.end());  // an address: ASCII
	HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
	bool set = false;
	if (mem) {
		std::memcpy(GlobalLock(mem), wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
		GlobalUnlock(mem);
		EmptyClipboard();
		set = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
		if (!set) GlobalFree(mem);
	}
	CloseClipboard();
	if (!set) return false;
	INPUT in[4] {};
	for (auto& i : in) i.type = INPUT_KEYBOARD;
	in[0].ki.wVk = VK_CONTROL;
	in[1].ki.wVk = 'V';
	in[2].ki.wVk = 'V';
	in[2].ki.dwFlags = KEYEVENTF_KEYUP;
	in[3].ki.wVk = VK_CONTROL;
	in[3].ki.dwFlags = KEYEVENTF_KEYUP;
	SendInput(4, in, sizeof(INPUT));
	std::thread([hwnd, old] {
		Sleep(1500);
		if (!OpenClipboard(hwnd)) return;
		EmptyClipboard();
		if (!old.empty()) {
			if (HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, (old.size() + 1) * sizeof(wchar_t))) {
				std::memcpy(GlobalLock(m), old.c_str(), (old.size() + 1) * sizeof(wchar_t));
				GlobalUnlock(m);
				if (!SetClipboardData(CF_UNICODETEXT, m)) GlobalFree(m);
			}
		}
		CloseClipboard();
	}).detach();
	return true;
}

// Clears the focused text box and types `text` as keyboard input.
void TypeText(const std::string& text) {
	HWND hwnd = GameWindow();
	if (!hwnd) {
		Log("type: no game window");
		return;
	}
	for (int i = 0; i < 48; ++i) {
		PostMessageW(hwnd, WM_KEYDOWN, VK_BACK, 0x000E0001);
		PostMessageW(hwnd, WM_KEYUP, VK_BACK, 0xC00E0001);
	}
	for (const char c : text) PostMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(static_cast<unsigned char>(c)), 1);
}

constexpr int kJoinWait = 20;  // UI updates between the steps

void JoinTick();

// D2RLoader's Direct Join: open the box, type the address, press Join. Its
// game_port is the joiner's own, so the address has no port either.
void LoaderJoinTick() {
	const auto box = [] { return ReadInput(kLoaderPanel, {"DirectJoinAddressInput"}); };
	if (g_join.step == 1) {
		// From the D2R Rooms window: D2RLoader's window comes back first.
		g_joinAddress = g_join.address;
		if (!g_loaderUi) Dispatch("PanelManager", "OpenPanel", kLoaderPanel);
		g_join.step = 2;
		OnUi(JoinTick);
		return;
	}
	if (g_join.step == 2) {
		Dispatch(kLoaderMessage, "ShowBrowse");
		Dispatch(kLoaderMessage, "OpenDirectJoin");
		g_join.step = 3;
		OnUi(JoinTick);
		return;
	}
	if (g_join.step == 3) {
		Log("join: direct join box had '%s'", box().c_str());
		if (!g_join.address.empty()) ClickLoaderWidget("DirectJoinContainer", "DirectJoinAddressInput");
		g_join.step = 4;
		OnUi(JoinTick);
		return;
	}
	if (g_join.step == 4) {
		// First tries: cleared, then pasted whole. Last try: typed one character
		// at a time, each checked and typed again when the box did not take it.
		if (g_join.typed == 0) ClearFocusedText();
		if (g_join.tries < 2) {
			Log("join: pasting %s -> %s", g_join.address.c_str(), PasteText(g_join.address) ? "ok" : "no clipboard");
			g_join.step = 5;
			OnUi(JoinTick);
			return;
		}
		if (g_join.typed > 0 && box() != g_join.address.substr(0, g_join.typed)) --g_join.typed;
		if (g_join.typed < g_join.address.size()) {
			const std::string now = box();
			if (now.size() > g_join.typed) ClearFocusedText(), g_join.typed = 0;
			TypeChar(g_join.address[g_join.typed++]);
			OnUi(JoinTick);
			return;
		}
		g_join.step = 5;
		OnUi(JoinTick);
		return;
	}
	const std::string typed = box();
	if (!g_join.address.empty() && typed != g_join.address && ++g_join.tries < 3) {
		Log("join: direct join box has '%s', want '%s': again", typed.c_str(), g_join.address.c_str());
		g_join.step = 3;
		g_join.typed = 0;
		OnUi(JoinTick);
		return;
	}
	Log("join: direct join box has '%s', want '%s'", typed.c_str(), g_join.address.c_str());
	if (!g_join.address.empty() && typed != g_join.address) {
		// The box kept another address: the player types it; Join waits for it.
		ShowNotice("D2R Rooms: type " + g_join.address + " into Direct Join's address box" +
		           (g_join.press ? ", then press Join." : ", the room's password below it, then press Join."));
	} else if (!g_join.press) {
		ShowNotice("D2R Rooms: type the room's password into Direct Join, then press Join.");
	} else {
		HideNotice();
		g_resendUntil = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		Dispatch(kLoaderMessage, "JoinDirect", g_join.address.c_str());
	}
	g_join = {};
}

void JoinTick() {
	if (g_join.step == 0) return;
	// Typing goes a character every few frames; the other steps wait longer.
	if (++g_join.ticks < (g_join.loader && g_join.step == 4 && g_join.typed > 0 ? 8 : kJoinWait)) {
		OnUi(JoinTick);
		return;
	}
	g_join.ticks = 0;
	if (g_join.loader) {
		LoaderJoinTick();
		return;
	}
	if (g_join.step == 1) {
		Log("join: box had '%s'", ReadInput("JoinGameModal", {"JoinGameInputScreen", "InputText"}).c_str());
		TypeText(g_join.address);
		g_join.step = 2;
		OnUi(JoinTick);
		return;
	}
	const std::string typed = ReadInput("JoinGameModal", {"JoinGameInputScreen", "InputText"});
	Log("join: box has '%s', want '%s'", typed.c_str(), g_join.address.c_str());
	if (typed == g_join.address) {
		Dispatch("FrontEndNavigation", "ToJoinCharacterSelect");
	} else {
		SetStatus("Type " + g_join.address + " into Join Game and press OK.");
	}
	g_join = {};
}

// The Join of Direct Join that a locked room's join took over goes on to
// D2RLoader now: the relay is up (or the board or relay refused, and
// D2RLoader's own "unable to join" says so).
void ResendJoinDirect() {
	const std::string box = ReadInput(kLoaderPanel, {"DirectJoinAddressInput"});
	if (!g_joinAddress.empty() && box != g_joinAddress) {
		Log("join: Direct Join has '%s', not %s; it waits", box.c_str(), g_joinAddress.c_str());
		ShowNotice("D2R Rooms: type " + g_joinAddress + " into Direct Join's address box, then press Join.");
		return;
	}
	HideNotice();
	g_resendUntil = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	Dispatch(kLoaderMessage, "JoinDirect");
}

void StartJoin(const JoinAnswer& answer) {
	if (g_resendJoinDirect.exchange(false)) {
		Log("join: %s ready; Direct Join goes on", answer.address.c_str());
		ResendJoinDirect();
		return;
	}
	// D2R's TCP/IP join takes an address without a port; the board lists port 4000.
	g_join = {.step = 1, .ticks = 0, .address = answer.address, .loader = g_loaderUi || g_tabActive};
	const bool relayed = answer.relay && answer.address.rfind("127.", 0) == 0;
	SetStatus((relayed ? "Joining through the relay (" + answer.address + ")" : "Joining " + answer.address) +
	          (answer.modMismatch ? " (the host plays another mod)" : ""));
	// Closed so that the typing goes to the join box, not to this panel's field.
	g_tabActive = false;
	ShowPanel(false);
	if (!g_join.loader) Dispatch("FrontEndNavigation", "ToJoinGame");
	OnUi(JoinTick);
}

void StopJoinRelay() {
	std::shared_ptr<net::JoinRelay> old;
	{
		std::lock_guard lock(g_mu);
		old.swap(g_joinRelay);
	}
	if (old) old->Stop();
}

void JoinThroughRelay(JoinAnswer answer) {
	StopJoinRelay();
	SetStatus("Opening the relay to the host...");
	OnUi(ApplyLayout);
	auto relay = std::make_shared<net::JoinRelay>(net::JoinRelay::Options {
		.relay = *answer.relay,
		.gamePort = GamePort(),
		.acceptTimeoutMs = 120'000,
		.log = LogLine,
		.ended = [](const std::string& why) {
			SetStatus("The relay connection ended: " + why);
			OnUi(ApplyLayout);
		},
	});
	std::string address, error;
	if (relay->Start(&address, &error)) {
		{
			std::lock_guard lock(g_mu);
			g_joinRelay = relay;
		}
		answer.address = address;
		OnUi([answer] { StartJoin(answer); });
		return;
	}
	// The relay refused (its limit, the host gone) or does not answer: the
	// host's own address is the last try, when the board gave one.
	if (answer.address.empty()) {
		Log("join: relay %s refused (%s)", answer.relay->url.c_str(), error.c_str());
		SetStatus("The relay did not take you (" + error + ").");
		if (g_resendJoinDirect.exchange(false)) OnUi(ResendJoinDirect);
		OnUi(ApplyLayout);
		return;
	}
	Log("join: relay %s refused (%s), trying %s", answer.relay->url.c_str(), error.c_str(), answer.address.c_str());
	SetStatus("The relay did not take you (" + error + "); trying the host's address.");
	OnUi([answer] { StartJoin(answer); });
}

void JoinRoom(const Room& room, const std::string& password);

void Join(int row) {
	Room room;
	{
		std::lock_guard lock(g_mu);
		const size_t index = static_cast<size_t>(g_page) * kRowsPerPage + row;
		if (row < 0 || index >= g_rooms.size()) return;
		room = g_rooms[index];
	}
	// From the D2R Rooms window a locked room's password goes into D2RLoader's
	// Direct Join box (a plugin panel takes no typing): it opens with the
	// address, and its Join is taken over to ask the board and open the relay.
	if (room.locked && (g_tabActive || g_loaderUi)) {
		Log("join: room %s has a password: Direct Join first", room.code.c_str());
		g_lockedJoin = room;
		g_join = {.step = 1, .ticks = 0, .address = room.relayUrl.empty() ? "" : "127.0.0.1", .loader = true, .press = false};
		g_tabActive = false;
		ShowPanel(false);
		OnUi(JoinTick);
		return;
	}
	const std::string password = room.locked ? ReadInput(CurrentPanel().c_str(), {"RoomName"}) : std::string();
	if (room.locked && password.empty()) {
		Log("join: room %s has a password, the field is empty", room.code.c_str());
		SetStatus("Room " + room.name + " has a password: type it into the field below, then Join.");
		ApplyLayout();
		return;
	}
	JoinRoom(room, password);
}

void JoinRoom(const Room& room, const std::string& password) {
	ReadModVersion();
	Log("join: room %s (%s; mine %s)", room.code.c_str(), ModLabel(room.mod).c_str(), ModLabel(g_mod).c_str());
	// D2RLoader joins only the same shared plugins: say so before it refuses.
	const std::string versionNote = !room.mod.version.empty() && !g_mod.version.empty() && room.mod.version != g_mod.version
		? " The host plays " + ModLabel(room.mod) + ", you " + ModLabel(g_mod) + ": D2RLoader needs the same version."
		: std::string();
	Send(D2RL::Http::Method::Post, "/api/join", JoinBody(room.id, password, g_mod), {}, [versionNote](int status, const std::string& body) {
		auto answer = status == 200 ? ParseJoin(body) : std::nullopt;
		if (!answer) {
			Log("join: the board answered %d", status);
			SetStatus(Explain(status, body));
			if (g_resendJoinDirect.exchange(false)) OnUi(ResendJoinDirect);
			OnUi(ApplyLayout);
			return;
		}
		if (!versionNote.empty()) {
			Log("join:%s", versionNote.c_str());
			SetStatus(versionNote.substr(1));
		}
		if (answer->ipv6) Log("join: host address is IPv6 (%s)", answer->address.c_str());
		// mode = never is about hosting; a room behind a relay is joined through it.
		if (!answer->relay) {
			OnUi([a = *answer] { StartJoin(a); });
			return;
		}
		// The host plays through a relay: open the pipe first (blocking, so on
		// a thread of its own), then let the game join the loopback listener.
		std::thread([a = *answer] { JoinThroughRelay(a); }).detach();
	});
}

// --- hosting ------------------------------------------------------------

// Players in the hosted game: the host and one TCP connection to its game
// port per joiner, straight or through the relay (the relay's pipe connects
// from 127.0.0.1). D2R gives a plugin no player list. Any thread.
int CountPlayers() {
	const DWORD pid = GetCurrentProcessId();
	const u_short port = htons(static_cast<u_short>(GamePort()));
	int joiners = 0;
	std::vector<char> buf;
	for (const ULONG family : {AF_INET, AF_INET6}) {
		DWORD size = 0;
		GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
		buf.resize(size + 4096);
		size = static_cast<DWORD>(buf.size());
		if (GetExtendedTcpTable(buf.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) != NO_ERROR) continue;
		if (family == AF_INET) {
			const auto* t = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
			for (DWORD i = 0; i < t->dwNumEntries; ++i) {
				const auto& r = t->table[i];
				if (r.dwOwningPid == pid && r.dwState == MIB_TCP_STATE_ESTAB && static_cast<u_short>(r.dwLocalPort) == port) ++joiners;
			}
		} else {
			const auto* t = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buf.data());
			for (DWORD i = 0; i < t->dwNumEntries; ++i) {
				const auto& r = t->table[i];
				if (r.dwOwningPid == pid && r.dwState == MIB_TCP_STATE_ESTAB && static_cast<u_short>(r.dwLocalPort) == port) ++joiners;
			}
		}
	}
	return std::min(1 + joiners, 8);
}

void HeartbeatLoop(Hosted hosted) {
	// Every heartbeat_every seconds, and at once when the player count changes
	// (checked every 2 s).
	std::unique_lock lock(g_heartbeatMu);
	int sent = 1;  // the board starts at the host alone
	auto last = std::chrono::steady_clock::now();
	while (!g_heartbeatCv.wait_for(lock, std::chrono::seconds(2), [] { return g_heartbeatStop; })) {
		const int players = CountPlayers();
		if (players == sent && std::chrono::steady_clock::now() - last < std::chrono::seconds(hosted.heartbeatEvery)) continue;
		if (players != sent) Log("host: %d player(s) in the game", players);
		sent = players;
		last = std::chrono::steady_clock::now();
		Send(D2RL::Http::Method::Post, "/api/rooms/" + hosted.roomId + "/heartbeat", HeartbeatBody(g_act, g_level, {}, players), hosted.token,
		     [](int status, const std::string&) {
			     if (status != 404) return;
			     Log("host: the board dropped the room");
			     {
				     std::lock_guard l(g_mu);
				     g_hosted.reset();
				     g_status = "Your room expired on the board.";
			     }
			     OnUi(ApplyLayout);
		     });
	}
}

void StopHeartbeat() {
	{
		std::lock_guard lock(g_heartbeatMu);
		g_heartbeatStop = true;
	}
	g_heartbeatCv.notify_all();
	if (g_heartbeat.joinable()) g_heartbeat.join();
	g_heartbeatStop = false;
}

// --- hosting through a relay ---------------------------------------------

void SendRelayToBoard(const Hosted& hosted, const std::string& relayJson) {
	Send(D2RL::Http::Method::Post, "/api/rooms/" + hosted.roomId + "/heartbeat", HeartbeatBody(g_act, g_level, relayJson), hosted.token,
	     [](int status, const std::string&) { Log("host: relay sent to the board -> %d", status); });
}

// Picks the first relay that takes new rooms, opens the room there and tells
// the board. On a thread of its own; `session` stops it when hosting ended.
void StartHostRelay(int session, int attempt) {
	std::optional<Hosted> hosted;
	{
		std::lock_guard lock(g_mu);
		hosted = g_hosted;
	}
	if (!hosted || session != g_hostSession) return;
	std::vector<RelayServer> order;
	{
		std::lock_guard lock(g_mu);
		order = HostOrder(g_relayCfg);
	}
	std::vector<RelayHealth> states;
	const RelayServer relay = net::PickRelay(order, &states);
	{
		std::lock_guard lock(g_mu);
		for (size_t i = 0; i < states.size() && i < order.size(); ++i) g_relayStates[order[i].url] = {states[i], std::chrono::steady_clock::now()};
	}
	if (relay.url.empty()) {
		Log("host: no relay takes new rooms");
		SetStatus("Friends cannot reach your port, and no relay takes new rooms right now. Open the port, or try later.");
		OnUi(ApplyLayout);
		return;
	}
	auto host = std::make_shared<net::HostRelay>(net::HostRelay::Options {
		.relay = relay,
		.gamePort = GamePort(),
		.log = LogLine,
		.lost = [session, attempt] {
			// The relay socket dropped (network, a relay restart): a new room
			// there, at most 3 times; the board gets the new pipe.
			std::thread([session, attempt] {
				Sleep(3000);
				if (session != g_hostSession) return;
				if (attempt < 3) {
					StartHostRelay(session, attempt + 1);
					return;
				}
				std::optional<Hosted> h;
				{
					std::lock_guard lock(g_mu);
					h = g_hosted;
					g_hostRelay.reset();
				}
				if (h) SendRelayToBoard(*h, "null");
				SetStatus("The relay connection was lost.");
				OnUi(ApplyLayout);
			}).detach();
		},
	});
	std::string error;
	if (!host->Start(&error)) {
		SetStatus("Relay " + relay.name + " refused the room (" + error + ").");
		OnUi(ApplyLayout);
		return;
	}
	std::shared_ptr<net::HostRelay> old;
	{
		std::lock_guard lock(g_mu);
		if (session != g_hostSession || !g_hosted) {
			host->Stop();
			return;
		}
		old.swap(g_hostRelay);
		g_hostRelay = host;
		g_hostRelayName = relay.name;
		g_status = g_relayCfg.mode == RelayMode::Always
		               ? "Friends join through relay " + relay.name + " (mode = always)."
		               : "Your port cannot be reached from outside: friends join through relay " + relay.name + ".";
	}
	if (old) std::thread([old] { old->Stop(); }).detach();
	SendRelayToBoard(*hosted, RelayRefJson(host->Ref()));
	OnUi(ApplyLayout);
}

// Watches d2rloader.log from the moment the room was created: D2RLoader checks
// the hosted port from outside once the game listens. Only a "blocked" answer
// starts a relay (mode = auto). On a thread of its own.
void WatchHosting(int session, size_t logStart) {
	RelayMode mode;
	{
		std::lock_guard lock(g_mu);
		mode = g_relayCfg.mode;
	}
	if (mode == RelayMode::Never) return;
	if (mode == RelayMode::Always) {
		StartHostRelay(session, 0);
		return;
	}
	const auto started = std::chrono::steady_clock::now();
	std::optional<std::chrono::steady_clock::time_point> suspectSince;
	while (session == g_hostSession) {
		Sleep(1000);
		const HostingVerdict v = ParseHostingVerdict(ReadText(LoaderLog(), logStart));
		if (v == HostingVerdict::Reachable) {
			Log("host: D2RLoader says the port is reachable; no relay");
			return;
		}
		if (v == HostingVerdict::Suspect && !suspectSince) suspectSince = std::chrono::steady_clock::now();
		const bool waitedOnSuspect = suspectSince && std::chrono::steady_clock::now() - *suspectSince > std::chrono::seconds(20);
		if (v == HostingVerdict::Unreachable || waitedOnSuspect) {
			Log("host: D2RLoader says the port is blocked; using a relay");
			StartHostRelay(session, 0);
			return;
		}
		// Character select can take a while; after 20 minutes without any
		// answer (hosting_check off?) nothing happens.
		if (std::chrono::steady_clock::now() - started > std::chrono::minutes(20)) {
			Log("host: no port verdict from D2RLoader; no relay");
			return;
		}
	}
}

void StopHosting(bool removeFromBoard) {
	StopHeartbeat();
	++g_hostSession;
	std::optional<Hosted> hosted;
	std::shared_ptr<net::HostRelay> relay;
	{
		std::lock_guard lock(g_mu);
		hosted.swap(g_hosted);
		relay.swap(g_hostRelay);
	}
	if (relay) std::thread([relay] { relay->Stop(); }).detach();
	g_hostedGameStarted = false;
	if (hosted && removeFromBoard) {
		Log("host: closing room %s", hosted->code.c_str());
		Send(D2RL::Http::Method::Delete, "/api/rooms/" + hosted->roomId, {}, hosted->token, [](int, const std::string&) { Refresh(); });
	}
	OnUi(ApplyLayout);
}

// Reads d2r-rooms.ini beside the DLL (written with ours when missing) and
// returns whether the bridge is on.
bool LoadRelayConfig() {
	const std::string ini = DllDir() + kRelayIniName;
	std::string iniText = ReadText(ini);
	if (iniText.empty()) {
		iniText = DefaultRelayIni();
		std::ofstream(ini, std::ios::binary) << iniText;
	}
	RelayConfig cfg = ParseRelayIni(iniText);
	Log("relays: %s, mode %s, use '%s', game_port %d, %zu relay(s) in %s", cfg.enabled ? "on" : "off", RelayModeName(cfg.mode), cfg.use.c_str(),
	    GamePort(), cfg.relays.size(), ini.c_str());
	std::lock_guard lock(g_mu);
	g_relayCfg = std::move(cfg);
	return g_relayCfg.enabled;
}

// Lists the room. `loader`: D2RLoader's CREATE GAME was pressed and goes on to
// host by itself; otherwise this goes on like D2R's own Host Game button.
void CreateRoom(std::string name, const std::string& password, bool loader) {
	if (name.empty()) name = "D2R game";
	ReadModVersion();
	Log("host: creating '%s'%s%s", name.c_str(), password.empty() ? "" : " with a password", loader ? " from D2RLoader's CREATE GAME" : "");
	// D2RLoader's verdict on the port comes after this point in its log.
	size_t logStart = 0;
	ReadText(LoaderLog(), ~size_t(0), &logStart);
	Send(D2RL::Http::Method::Post, "/api/rooms", CreateBody(name, g_mod, password, g_act), {}, [logStart, loader](int status, const std::string& body) {
		auto hosted = status == 200 ? ParseCreate(body) : std::nullopt;
		if (!hosted) {
			SetStatus(Explain(status, body));
			OnUi(ApplyLayout);
			return;
		}
		Log("host: room %s at %s%s", hosted->code.c_str(), hosted->address.c_str(), hosted->ipv6 ? " (IPv6)" : "");
		{
			std::lock_guard lock(g_mu);
			g_hosted = *hosted;
			g_status = "Room created.";
		}
		StopHeartbeat();
		g_heartbeat = std::thread(HeartbeatLoop, *hosted);
		std::thread(WatchHosting, ++g_hostSession, logStart).detach();
		if (loader) {
			OnUi(ApplyLayout);
			return;
		}
		// Same message as D2R's own Host Game button on the TCP/IP screen.
		OnUi([] {
			ShowPanel(false);
			Dispatch("FrontEndNavigation", "ToCharacterSelect");
		});
	});
}

void Create() {
	if (Hosting()) return;
	CreateRoom(ReadInput(CurrentPanel().c_str(), {"RoomName"}), {}, false);
}

// D2RLoader's CREATE GAME: the same room, named as its game. A room still
// listed from an earlier try is replaced. UI thread, before D2RLoader acts.
void CreateFromLoader() {
	if (!LoadRelayConfig()) {
		Log("host: the bridge is off (enabled = no); CREATE GAME is D2RLoader's alone");
		return;
	}
	const std::string name = ReadInput(kLoaderPanel, {"HostGameNameInput"});
	const std::string password = ReadInput(kLoaderPanel, {"HostPasswordInput"});
	if (Hosting()) StopHosting(true);
	CreateRoom(name, password, true);
}

// --- D2R messages -------------------------------------------------------

// Where D2RLoader's Multiplayer window and the fields this plugin uses are,
// for placing the list beside it in a later version. UI thread.
void LogLoaderPanel() {
	D2RL::Widgets::WidgetHandle panel = D2RL::Widgets::InvalidHandle;
	if (g_widgets->findPanel(g_ctx, kLoaderPanel, &panel) != D2RL::Widgets::Result::Success) {
		Log("loader: %s not found", kLoaderPanel);
		return;
	}
	const auto logRect = [](const char* name, D2RL::Widgets::WidgetHandle h) {
		D2RL::Widgets::Rect r {};
		const auto got = g_widgets->getWidgetRect(g_ctx, h, &r);
		Log("loader: %s rect %d,%d %dx%d (%u)", name, r.x, r.y, r.width, r.height, static_cast<unsigned>(got));
	};
	logRect(kLoaderPanel, panel);
	for (const char* name : {"MainTabs", "BrowseContainer", "HostContainer", "HostGameNameInput", "HostPasswordInput", "DirectJoinAddressInput",
	                         "DirectJoinPasswordInput"}) {
		D2RL::Widgets::WidgetHandle h = D2RL::Widgets::InvalidHandle;
		const auto found = g_widgets->findWidget(g_ctx, panel, name, &h);
		if (found == D2RL::Widgets::Result::Success) logRect(name, h);
		else Log("loader: %s not found (%u)", name, static_cast<unsigned>(found));
	}
}

void HandleOwn(const std::string& command, const std::string& text) {
	if (command == "Close") {
		g_tabActive = false;
		ShowPanel(false);
	} else if (command == "Tab") {
		if (g_loaderUi) EnterTab();  // in the D2R Rooms window it is the selected tab
	} else if (command == "ToHost" || command == "ToBrowse") {
		OpenLoaderWindow(command == "ToHost" ? "ShowHost" : "ShowBrowse");
	} else if (command == "Refresh") {
		SetStatus("Loading the rooms...");
		ApplyLayout();
		Refresh();
	} else if (command == "Prev" || command == "Next") {
		{
			std::lock_guard lock(g_mu);
			g_page = std::clamp(g_page + (command == "Next" ? 1 : -1), 0, PageCount(g_rooms.size()) - 1);
		}
		ApplyLayout();
	} else if (command == "Join") {
		Join(std::atoi(text.c_str()));
	} else if (command == "Create") {
		Create();
	} else if (command == "CloseRoom") {
		StopHosting(true);
	}
}

// D2RLoader's Multiplayer window is modal: a click on the tab or the list
// never reaches their buttons. So a click is matched here against where they
// are drawn. Layout units are 2160 high, from the top middle of the window.
std::string g_lastClickCommand;
std::chrono::steady_clock::time_point g_lastClickAt;

void HandleClick() {
	HWND hwnd = GameWindow();
	POINT p {};
	RECT rc {};
	if (!hwnd || !GetCursorPos(&p) || !ScreenToClient(hwnd, &p) || !GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
	const double scale = std::min(rc.right / 3840.0, rc.bottom / 2160.0);
	const int x = static_cast<int>((p.x - rc.right / 2.0) / scale);
	const int y = static_cast<int>(p.y / scale);
	const std::vector<HitArea> areas = g_tabButtonOpen ? TabHitAreas() : WindowHitAreas(g_shownView);
	for (const HitArea& a : areas) {
		if (x < a.x || x >= a.x + a.width || y < a.y || y >= a.y + a.height) continue;
		Log("click: %d,%d -> %s %s", x, y, a.command.c_str(), a.argument.c_str());
		g_lastClickCommand = a.command;
		g_lastClickAt = std::chrono::steady_clock::now();
		HandleOwn(a.command, a.argument);
		return;
	}
	Log("click: %d,%d (window %ldx%ld)", x, y, rc.right, rc.bottom);
}

D2RL::SharedEvents::UiMessageAction __cdecl OnUiMessage(const D2RL::PluginContext*, const D2RL::SharedEvents::UiMessageEvent* event, void*) noexcept {
	const std::string target = event->target ? event->target : "";
	const std::string command = event->command ? event->command : "";
	const std::string text = event->text ? event->text : "";
	// Only the messages this plugin acts on are logged: every UI message (60 a
	// second in a game, about 1 ms a line) filled 1.25 MB in minutes.
	// D2R_ROOMS_LOG_UI=1 logs them all again.
	static const bool logAll = [] {
		char v[8] {};
		return GetEnvironmentVariableA("D2R_ROOMS_LOG_UI", v, sizeof(v)) > 0 && v[0] == '1';
	}();
	if (logAll || target == kLoaderMessage || target == "FrontEndNavigation" ||
	    (target == "PanelManager" && (text.rfind("d2r-rooms/", 0) == 0 || text == kLoaderPanel || command == "UnloadAll")))
		Log("ui: %s:%s:%s", target.c_str(), command.c_str(), text.c_str());

	if (target == kMessageTarget) {
		// The same press may already have been taken by HandleClick.
		const bool clicked = command == g_lastClickCommand && std::chrono::steady_clock::now() - g_lastClickAt < std::chrono::milliseconds(500);
		try {
			if (!clicked) HandleOwn(command, text);
		} catch (...) {
		}
		return D2RL::SharedEvents::UiMessageAction::Consume;
	}
	const bool windowOpen = g_panelOpen && g_tabActive;
	if (target == "InputMessage" && command == "LeftMouseButtonUp" && (g_tabButtonOpen || windowOpen)) {
		try {
			HandleClick();
		} catch (...) {
		}
	}
	if (target == "InputMessage" && command == "KeyDown" && windowOpen && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
		Log("key: Escape closes the window");
		OnUi([] { HandleOwn("Close", ""); });
	}
	if (target == kLoaderMessage && command == "JoinDirect" && g_lockedJoin && std::chrono::steady_clock::now() >= g_resendUntil) {
		// The player typed the locked room's password into Direct Join: the
		// board gets it, the relay opens, then this press goes on.
		const std::string password = ReadInput(kLoaderPanel, {"DirectJoinPasswordInput"});
		const Room room = *g_lockedJoin;
		g_lockedJoin.reset();
		Log("join: Direct Join pressed for room %s (%s password)", room.code.c_str(), password.empty() ? "no" : "a");
		g_resendJoinDirect = true;
		try {
			JoinRoom(room, password);
		} catch (...) {
		}
		return D2RL::SharedEvents::UiMessageAction::Consume;
	}
	if (target == "PanelManager" && text == kLoaderPanel) {
		if (command == "OpenPanel" || command == "OpenExclusivePanel") {
			OnUi([] {
				g_loaderUi = true;
				LogLoaderPanel();
				ShowTabButton(true);
			});
			Refresh();  // the list is ready when the tab is pressed
		} else if (command == "ClosePanel" || command == "UnloadPanel") {
			OnUi(CloseLoaderUi);
		}
	} else if (target == kLoaderMessage && command == "Close") {
		// D2RLoader closes its window with its own message, not ClosePanel.
		OnUi(CloseLoaderUi);
	} else if (target == kLoaderMessage && command == "CreateGame") {
		// Read its fields now, before D2RLoader moves on; it hosts by itself.
		try {
			CreateFromLoader();
		} catch (...) {
		}
	}
	if (target == kLoaderMessage && command == "JoinDirect" && !g_joinAddress.empty() && std::chrono::steady_clock::now() >= g_resendUntil) {
		// A join the plugin started: it goes out only to its own address.
		const std::string box = ReadInput(kLoaderPanel, {"DirectJoinAddressInput"});
		if (box != g_joinAddress) {
			Log("join: Direct Join pressed with '%s', not %s: held", box.c_str(), g_joinAddress.c_str());
			OnUi([] { ShowNotice("D2R Rooms: type " + g_joinAddress + " into Direct Join's address box, then press Join."); });
			return D2RL::SharedEvents::UiMessageAction::Consume;
		}
		Log("join: Direct Join pressed with %s", box.c_str());
		g_joinAddress.clear();
		OnUi(HideNotice);
	} else if (target == kLoaderMessage && command == "JoinDirect" && std::chrono::steady_clock::now() < g_resendUntil) {
		g_joinAddress.clear();
	}
	if (target == "FrontEndNavigation") {
		if (command == "ToTCPIP") {
			OnUi([] { ShowPanel(true); });
			Refresh();
		} else {
			// Leaving the front-end screen (into a game too): D2R unloads the
			// panels, D2RLoader's window included, without its Close message.
			OnUi(CloseLoaderUi);
		}
	} else if (target == "PanelManager" && command == "UnloadAll") {
		OnUi(CloseLoaderUi);
	}
	return D2RL::SharedEvents::UiMessageAction::Continue;
}

void __cdecl OnGameplay(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent* event, void*) noexcept {
	switch (event->kind) {
		case D2RL::Lifecycle::GameplayEventKind::GameJoined:
			g_inGame = true;
			if (Hosting()) g_hostedGameStarted = true;
			break;
		case D2RL::Lifecycle::GameplayEventKind::GameLeft:
			g_inGame = false;
			if (Hosting() && g_hostedGameStarted) StopHosting(true);
			break;
		case D2RL::Lifecycle::GameplayEventKind::ActChanged:
			g_act = event->currentValue + 1;
			break;
		case D2RL::Lifecycle::GameplayEventKind::PlayerLevelChanged:
			g_level = event->currentValue;
			break;
		default:
			break;
	}
}

// --- console ------------------------------------------------------------

std::string Args(const D2RL::ConsoleCommandContext* command) {
	return command && command->args ? std::string(command->args, command->argsLength) : std::string();
}

// rooms-art [<image> [<width> <height>]]: shows that image in a panel of that
// size (default the TCP/IP screen's art at 1730 x 1300), centred where the D2R
// Rooms window goes, both as a GridImageWidget and an ImageWidget with that
// rect, to see whether the game stretches it. "rooms-art off" hides it.
D2RL::ConsoleCommandResult __cdecl CmdArt(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept {
	std::istringstream in(Args(command));
	std::string image = "PANEL\\TCP_IP\\TCP_IP_PanelBG";
	int width = 1730, height = 1300;
	std::string first;
	if (in >> first) {
		if (first == "off") {
			OnUi([] { ShowPanel(false); });
			return D2RL::ConsoleCommandResult::Handled;
		}
		image = first;
		in >> width >> height;
	}
	OnUi([image, width, height] {
		g_tabActive = false;
		if (g_panelOpen && g_slot >= 0) g_panels->closePanel(g_ctx, g_pool[g_slot]);
		g_panelOpen = true;
		const int gen = ++g_layoutGen;
		const int slot = g_nextSlot++ % g_poolSize;
		g_slot = slot;
		const std::string file = json::Quote(image);
		const std::string rect = "{\"x\":0,\"y\":0,\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) + "}";
		const std::string layout = "{\"type\":\"Panel\",\"name\":" + json::Quote(PanelName(slot)) + ",\"fields\":{\"anchor\":{\"x\":0.5,\"y\":0.1},\"rect\":{\"x\":" +
			std::to_string(-width / 2) + ",\"y\":0,\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) +
			"},\"priority\":2100000000},\"children\":["
			"{\"type\":\"GridImageWidget\",\"name\":\"Grid\",\"fields\":{\"rect\":" + rect + ",\"filename\":" + file + "}},"
			"{\"type\":\"TextBoxWidget\",\"name\":\"Title" + std::to_string(gen) + "\",\"fields\":{\"rect\":{\"x\":0,\"y\":120,\"width\":" +
			std::to_string(width) + ",\"height\":80},\"text\":" + json::Quote("art probe: " + image + " " + std::to_string(width) + " x " + std::to_string(height)) +
			",\"style\":{\"fontColor\":\"$FontColorLightGold\",\"pointSize\":\"$MediumFontSize\",\"alignment\":{\"h\":\"center\",\"v\":\"center\"}}}}]}";
		if (g_poolLayout[slot] != D2RL::Resources::InvalidHandle) g_resources->unregisterResource(g_ctx, g_poolLayout[slot]);
		g_poolLayout[slot] = D2RL::Resources::InvalidHandle;
		const std::string path = PanelPath(slot);
		const D2RL::Resources::ResourceRegistration resource {
			.structSize = D2RL::Resources::ResourceRegistrationSize,
			.flags      = 0,
			.path       = path.c_str(),
			.bytes      = layout.data(),
			.byteCount  = layout.size(),
		};
		g_resources->registerResource(g_ctx, &resource, &g_poolLayout[slot]);
		Log("art: %s %d x %d in panel %d", image.c_str(), width, height, slot);
		OpenLater(gen, 1);
	});
	return D2RL::ConsoleCommandResult::Handled;
}

D2RL::ConsoleCommandResult __cdecl CmdRooms(D2R::Game::Client*, const D2RL::ConsoleCommandContext*, void*) noexcept {
	OnUi([] {
		const bool show = !g_panelOpen;
		ShowPanel(show);
		if (show) Refresh();
	});
	return D2RL::ConsoleCommandResult::Handled;
}

// rooms-ui <panel> [<child> ...]: logs a widget's rectangle and input text.
D2RL::ConsoleCommandResult __cdecl CmdUi(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept {
	const std::string args = Args(command);
	OnUi([args] {
		std::vector<std::string> parts;
		size_t start = 0;
		while (start < args.size()) {
			const size_t space = args.find(' ', start);
			const std::string part = args.substr(start, space == std::string::npos ? std::string::npos : space - start);
			if (!part.empty()) parts.push_back(part);
			if (space == std::string::npos) break;
			start = space + 1;
		}
		if (parts.empty()) return;
		D2RL::Widgets::WidgetHandle h = D2RL::Widgets::InvalidHandle;
		auto r = g_widgets->findPanel(g_ctx, parts[0].c_str(), &h);
		for (size_t i = 1; i < parts.size() && r == D2RL::Widgets::Result::Success; ++i)
			r = g_widgets->findWidget(g_ctx, h, parts[i].c_str(), &h);
		if (r != D2RL::Widgets::Result::Success) {
			Log("ui-probe: %s not found (%u)", args.c_str(), static_cast<unsigned>(r));
			return;
		}
		D2RL::Widgets::Rect rect {};
		g_widgets->getWidgetRect(g_ctx, h, &rect);
		char text[256] {};
		uint32_t required = 0;
		const auto t = g_widgets->getInputText(g_ctx, h, text, sizeof(text), &required);
		Log("ui-probe: %s rect %d,%d %dx%d text(%u) '%s'", args.c_str(), rect.x, rect.y, rect.width, rect.height,
		    static_cast<unsigned>(t), text);
	});
	return D2RL::ConsoleCommandResult::Handled;
}

// rooms-msg <target> <command> [<text>]: sends a UI message, as a button would.
D2RL::ConsoleCommandResult __cdecl CmdMsg(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept {
	const std::string args = Args(command);
	OnUi([args] {
		const size_t a = args.find(' ');
		if (a == std::string::npos) return;
		const size_t b = args.find(' ', a + 1);
		const std::string target = args.substr(0, a);
		const std::string cmd = args.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
		const std::string text = b == std::string::npos ? std::string() : args.substr(b + 1);
		Dispatch(target.c_str(), cmd.c_str(), text.c_str());
	});
	return D2RL::ConsoleCommandResult::Handled;
}

// rooms-type <text>: clears the focused text box and types the text.
D2RL::ConsoleCommandResult __cdecl CmdType(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept {
	TypeText(Args(command));
	return D2RL::ConsoleCommandResult::Handled;
}

template <class Service>
bool Query(const Service*& service, uint32_t requiredSize, bool (*has)(const Service*, uint32_t)) {
	return g_ctx->QueryService(&service) == D2RL::ServiceQueryResult::Success && has(service, requiredSize);
}

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &kInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) return false;
	g_ctx = context;
	if (!Query(g_panels, D2RL::PanelServiceRequiredSize, D2RL::HasPanelServiceField)
	    || !Query(g_resources, D2RL::ResourceServiceRequiredSize, D2RL::HasResourceServiceField)
	    || !Query(g_events, D2RL::SharedEventServiceRequiredSize, D2RL::HasSharedEventServiceField)
	    || !Query(g_widgets, D2RL::WidgetServiceRequiredSize, D2RL::HasWidgetServiceField)
	    || !Query(g_threads, D2RL::ThreadServiceRequiredSize, D2RL::HasThreadServiceField)
	    || !Query(g_http, D2RL::HttpServiceRequiredSize, D2RL::HasHttpServiceField)
	    || !Query(g_life, D2RL::LifecycleServiceRequiredSize, D2RL::HasLifecycleServiceField)) {
		D2RL::LogError(context, "d2r-rooms: this D2RLoader lacks a service the plugin needs");
		return false;
	}

	char server[512] {};
	if (GetEnvironmentVariableA("D2R_ROOMS_SERVER", server, sizeof(server)) > 0 && std::strncmp(server, "https://", 8) == 0)
		g_server = server;
	g_mod = DetectMod(GetCommandLineA());
	Log("d2r-rooms %s: server %s, mod %s", kInfo.version, g_server.c_str(), ModLabel(g_mod).c_str());

	// Relays: d2r-rooms.ini beside the DLL, written with ours when missing.
	g_gameDir = ExeDir();
	net::StartNetworking();
	LoadRelayConfig();
	for (const RelayServer& r : g_relayCfg.relays) Log("relay: %s = %s%s", r.name.c_str(), r.url.c_str(), r.key.empty() ? "" : " (with a key)");
	Log("command line: %s", GetCommandLineA());  // does D2RLoader's -mod reach D2R.exe?

	// The D2R Rooms button on D2RLoader's window first: a fixed layout. (In 0.8 it
	// came after 64 pool panels and did not register: D2RLoader has a limit.)
	const std::string tabLayout = TabLayout();
	const D2RL::Resources::ResourceRegistration tabResource {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.flags      = 0,
		.path       = kTabPanelPath,
		.bytes      = tabLayout.data(),
		.byteCount  = tabLayout.size(),
	};
	const D2RL::Panels::PanelRegistration tab {
		.structSize = D2RL::Panels::PanelRegistrationSize,
		.flags      = D2RL::Panels::PanelFlags::None,
		.localId    = kTabPanelLocalId,
	};
	if (g_resources->registerResource(context, &tabResource, &g_tabLayout) != D2RL::Resources::Result::Success ||
	    g_panels->registerPanel(context, &tab, &g_tabPanel) != D2RL::Panels::Result::Success)
		D2RL::LogWarn(context, "d2r-rooms: the tab beside BROWSE GAMES did not register");

	// The pool of rooms panels, each with a layout before it is registered;
	// every later layout replaces one before that panel's first open.
	for (int slot = 0; slot < kPanelSlots; ++slot) {
		PanelView empty;
		empty.slot = slot;
		const std::string layout = PanelLayout(empty);
		const std::string path = PanelPath(slot);
		const std::string localId = PanelLocalId(slot);
		const D2RL::Resources::ResourceRegistration resource {
			.structSize = D2RL::Resources::ResourceRegistrationSize,
			.flags      = 0,
			.path       = path.c_str(),
			.bytes      = layout.data(),
			.byteCount  = layout.size(),
		};
		const D2RL::Panels::PanelRegistration panel {
			.structSize = D2RL::Panels::PanelRegistrationSize,
			.flags      = D2RL::Panels::PanelFlags::CloseOnEscape,
			.localId    = localId.c_str(),
		};
		if (g_resources->registerResource(context, &resource, &g_poolLayout[slot]) != D2RL::Resources::Result::Success ||
		    g_panels->registerPanel(context, &panel, &g_pool[slot]) != D2RL::Panels::Result::Success) {
			if (g_resources && g_poolLayout[slot] != D2RL::Resources::InvalidHandle) g_resources->unregisterResource(context, g_poolLayout[slot]);
			g_poolLayout[slot] = D2RL::Resources::InvalidHandle;
			break;
		}
		g_poolSize = slot + 1;
	}
	Log("panels: %d in the pool", g_poolSize);
	if (g_poolSize == 0) {
		D2RL::LogError(context, "d2r-rooms: panel registration failed");
		return false;
	}

	const D2RL::SharedEvents::UiMessageListener listener {
		.structSize = D2RL::SharedEvents::UiMessageListenerSize,
		.flags      = 0,
		.priority   = 100,
		.reserved   = 0,
		.callback   = OnUiMessage,
		.userData   = nullptr,
	};
	D2RL::SharedEvents::ListenerHandle listenerHandle = D2RL::SharedEvents::InvalidHandle;
	if (g_events->registerUiMessageListener(context, &listener, &listenerHandle) != D2RL::SharedEvents::Result::Success) {
		D2RL::LogError(context, "d2r-rooms: UI message listener failed");
		return false;
	}

	for (const auto kind : {D2RL::Lifecycle::GameplayEventKind::GameJoined, D2RL::Lifecycle::GameplayEventKind::GameLeft,
	                        D2RL::Lifecycle::GameplayEventKind::ActChanged, D2RL::Lifecycle::GameplayEventKind::PlayerLevelChanged}) {
		const D2RL::Lifecycle::GameplayEventListener gameplay {
			.structSize = D2RL::Lifecycle::GameplayEventListenerSize,
			.flags      = 0,
			.kind       = kind,
			.reserved   = 0,
			.callback   = OnGameplay,
			.userData   = nullptr,
		};
		D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
		g_life->registerGameplayEventListener(context, &gameplay, &handle);
	}

	const bool commands = context->RegisterConsoleCommand("rooms", CmdRooms, "Show or hide the D2R Rooms list.")
		&& context->RegisterConsoleCommand("rooms-ui", CmdUi, "rooms-ui <panel> [<child>...]: log a widget's rect and input text.")
		&& context->RegisterConsoleCommand("rooms-msg", CmdMsg, "rooms-msg <target> <command> [<text>]: send a UI message.")
		&& context->RegisterConsoleCommand("rooms-type", CmdType, "rooms-type <text>: type text into the focused box.")
		&& context->RegisterConsoleCommand("rooms-art", CmdArt, "rooms-art [<image> [<width> <height>]] | off: show an image stretched to a size.");
	if (!commands) D2RL::LogWarn(context, "d2r-rooms: a console command did not register");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	// D2RLoader cancels open requests on unload; an open room expires on the
	// board a few minutes after its heartbeat stops.
	StopHeartbeat();
	++g_hostSession;
	std::shared_ptr<net::HostRelay> relay;
	{
		std::lock_guard lock(g_mu);
		relay.swap(g_hostRelay);
	}
	if (relay) relay->Stop();
	StopJoinRelay();
}
