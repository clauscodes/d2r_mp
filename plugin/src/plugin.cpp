// D2R Rooms: the board's room list inside the game, next to D2R's own TCP/IP
// screen. Join types the host's address into D2R's Join Game box and presses
// OK; Create lists a room and opens D2R's Host Game path. Everything goes
// through the game's own screens and messages - nothing in a game changes.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <D2RLPlugin/api.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "board.h"
#include "layout.h"

namespace {

using namespace rooms;

constexpr D2RL::PluginInfo kInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = kPluginId,
	.name        = "D2R Rooms",
	.version     = "0.1.0",
	.author      = "D2R Rooms",
	.description = "Open TCP/IP rooms: list, join and create from the TCP/IP screen.",
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

D2RL::Panels::RegistrationHandle    g_panel = D2RL::Panels::InvalidHandle;
D2RL::Resources::RegistrationHandle g_layout = D2RL::Resources::InvalidHandle;

std::string g_server = kDefaultServer;
Mod g_mod;

// Shared between the UI thread, HTTP callbacks and the heartbeat thread.
std::mutex g_mu;
std::vector<Room> g_rooms;
std::string g_status = "Loading the rooms...";
int g_page = 0;
std::optional<Hosted> g_hosted;

// UI thread only.
bool g_panelOpen = false;
struct JoinFlow {
	int step = 0;  // 0 = idle
	int ticks = 0;
	std::string address;
} g_join;

std::atomic<int> g_act {0};
std::atomic<int> g_level {0};
std::atomic<bool> g_inGame {false};
std::atomic<bool> g_hostedGameStarted {false};

std::thread g_heartbeat;
std::mutex g_heartbeatMu;
std::condition_variable g_heartbeatCv;
bool g_heartbeatStop = false;

void Log(const char* format, auto... args) {
	D2RL::LogInfoF(g_ctx, format, args...);
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
void ApplyLayout() {
	PanelView view;
	{
		std::lock_guard lock(g_mu);
		view.rooms = g_rooms;
		view.page = g_page;
		view.status = g_status;
		view.hosting = g_hosted.has_value();
		if (g_hosted)
			view.hostingText = "You host room [" + g_hosted->code + "]. Players connect to " + g_hosted->address +
				". Pick your character and start the game; the room closes when you leave it.";
	}
	const std::string layout = PanelLayout(view);

	const bool reopen = g_panelOpen;
	if (reopen) g_panels->closePanel(g_ctx, g_panel);
	if (g_layout != D2RL::Resources::InvalidHandle) g_resources->unregisterResource(g_ctx, g_layout);
	g_layout = D2RL::Resources::InvalidHandle;
	const D2RL::Resources::ResourceRegistration resource {
		.structSize = D2RL::Resources::ResourceRegistrationSize,
		.flags      = 0,
		.path       = kPanelPath,
		.bytes      = layout.data(),
		.byteCount  = layout.size(),
	};
	const auto registered = g_resources->registerResource(g_ctx, &resource, &g_layout);
	if (registered != D2RL::Resources::Result::Success) Log("layout: register failed %u", static_cast<unsigned>(registered));
	if (reopen) {
		const auto opened = g_panels->openPanel(g_ctx, g_panel);
		if (opened != D2RL::Panels::Result::Success) Log("panel: reopen failed %u", static_cast<unsigned>(opened));
	}
}

void ShowPanel(bool show) {
	if (show == g_panelOpen) return;
	if (show) {
		g_panelOpen = false;
		ApplyLayout();
		const auto opened = g_panels->openPanel(g_ctx, g_panel);
		Log("panel: open -> %u", static_cast<unsigned>(opened));
		g_panelOpen = opened == D2RL::Panels::Result::Success;
	} else {
		const auto closed = g_panels->closePanel(g_ctx, g_panel);
		Log("panel: close -> %u", static_cast<unsigned>(closed));
		g_panelOpen = false;
	}
}

void Refresh() {
	Send(D2RL::Http::Method::Get, "/api/rooms", {}, {}, [](int status, const std::string& body) {
		auto list = status == 200 ? ParseRooms(body) : std::nullopt;
		{
			std::lock_guard lock(g_mu);
			if (list) {
				g_rooms = std::move(*list);
				g_page = std::min(g_page, PageCount(g_rooms.size()) - 1);
				g_status = g_rooms.empty() ? "No open rooms right now. Create one below."
				                           : std::to_string(g_rooms.size()) + " open room(s) - page " + std::to_string(g_page + 1) +
				                                 "/" + std::to_string(PageCount(g_rooms.size()));
			} else {
				g_status = Explain(status, body);
			}
		}
		Log("rooms: %d -> %s", status, list ? "ok" : "failed");
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

void JoinTick() {
	if (g_join.step == 0) return;
	if (++g_join.ticks < kJoinWait) {
		OnUi(JoinTick);
		return;
	}
	g_join.ticks = 0;
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

void StartJoin(const JoinAnswer& answer) {
	// D2R's TCP/IP join takes an address without a port; the board lists port 4000.
	g_join = {.step = 1, .ticks = 0, .address = answer.address};
	SetStatus("Joining " + answer.address + (answer.modMismatch ? " (the host plays another mod)" : ""));
	ShowPanel(false);
	Dispatch("FrontEndNavigation", "ToJoinGame");
	OnUi(JoinTick);
}

void Join(int row) {
	Room room;
	{
		std::lock_guard lock(g_mu);
		const size_t index = static_cast<size_t>(g_page) * kRowsPerPage + row;
		if (row < 0 || index >= g_rooms.size()) return;
		room = g_rooms[index];
	}
	const std::string password = room.locked ? ReadInput(kPanelName, {"RoomName"}) : std::string();
	Log("join: room %s", room.code.c_str());
	Send(D2RL::Http::Method::Post, "/api/join", JoinBody(room.id, password, g_mod), {}, [](int status, const std::string& body) {
		const auto answer = status == 200 ? ParseJoin(body) : std::nullopt;
		if (!answer) {
			SetStatus(Explain(status, body));
			OnUi(ApplyLayout);
			return;
		}
		if (answer->ipv6) Log("join: host address is IPv6 (%s)", answer->address.c_str());
		OnUi([a = *answer] { StartJoin(a); });
	});
}

// --- hosting ------------------------------------------------------------

void HeartbeatLoop(Hosted hosted) {
	std::unique_lock lock(g_heartbeatMu);
	while (!g_heartbeatCv.wait_for(lock, std::chrono::seconds(hosted.heartbeatEvery), [] { return g_heartbeatStop; })) {
		Send(D2RL::Http::Method::Post, "/api/rooms/" + hosted.roomId + "/heartbeat", HeartbeatBody(g_act, g_level), hosted.token,
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

void StopHosting(bool removeFromBoard) {
	StopHeartbeat();
	std::optional<Hosted> hosted;
	{
		std::lock_guard lock(g_mu);
		hosted.swap(g_hosted);
	}
	g_hostedGameStarted = false;
	if (hosted && removeFromBoard) {
		Log("host: closing room %s", hosted->code.c_str());
		Send(D2RL::Http::Method::Delete, "/api/rooms/" + hosted->roomId, {}, hosted->token, [](int, const std::string&) { Refresh(); });
	}
	OnUi(ApplyLayout);
}

void Create() {
	if (Hosting()) return;
	std::string name = ReadInput(kPanelName, {"RoomName"});
	if (name.empty()) name = "D2R game";
	Log("host: creating '%s'", name.c_str());
	Send(D2RL::Http::Method::Post, "/api/rooms", CreateBody(name, g_mod, {}, g_act), {}, [](int status, const std::string& body) {
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
		// Same message as D2R's own Host Game button on the TCP/IP screen.
		OnUi([] {
			ShowPanel(false);
			Dispatch("FrontEndNavigation", "ToCharacterSelect");
		});
	});
}

// --- D2R messages -------------------------------------------------------

void HandleOwn(const std::string& command, const std::string& text) {
	if (command == "Close") {
		ShowPanel(false);
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

D2RL::SharedEvents::UiMessageAction __cdecl OnUiMessage(const D2RL::PluginContext*, const D2RL::SharedEvents::UiMessageEvent* event, void*) noexcept {
	const std::string target = event->target ? event->target : "";
	const std::string command = event->command ? event->command : "";
	const std::string text = event->text ? event->text : "";
	// Every message is logged while the plugin is young: it is how the
	// front-end screens' own messages are learned.
	Log("ui: %s:%s:%s", target.c_str(), command.c_str(), text.c_str());

	if (target == kMessageTarget) {
		try {
			HandleOwn(command, text);
		} catch (...) {
		}
		return D2RL::SharedEvents::UiMessageAction::Consume;
	}
	if (target == "FrontEndNavigation") {
		if (command == "ToTCPIP") {
			OnUi([] { ShowPanel(true); });
			Refresh();
		} else if (g_panelOpen) {
			OnUi([] { ShowPanel(false); });
		}
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
	Log("command line: %s", GetCommandLineA());  // does D2RLoader's -mod reach D2R.exe?

	ApplyLayout();  // the panel's layout must exist before the panel
	const D2RL::Panels::PanelRegistration panel {
		.structSize = D2RL::Panels::PanelRegistrationSize,
		.flags      = D2RL::Panels::PanelFlags::CloseOnEscape,
		.localId    = kPanelLocalId,
	};
	if (g_panels->registerPanel(context, &panel, &g_panel) != D2RL::Panels::Result::Success) {
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
		&& context->RegisterConsoleCommand("rooms-type", CmdType, "rooms-type <text>: type text into the focused box.");
	if (!commands) D2RL::LogWarn(context, "d2r-rooms: a console command did not register");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	// D2RLoader cancels open requests on unload; an open room expires on the
	// board a few minutes after its heartbeat stops.
	StopHeartbeat();
}
