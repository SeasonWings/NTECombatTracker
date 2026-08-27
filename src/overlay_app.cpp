#include "overlay_model.hpp"
#include "overlay_protocol.hpp"

#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using Microsoft::WRL::ComPtr;
using nte::combat_stats::EncounterState;
using nte::combat_stats::NteOverlaySnapshot;
using nte::combat_stats::PlayerRow;
using nte::combat_stats::overlay::CalculateLayout;
using nte::combat_stats::overlay::CriticalRate;
using nte::combat_stats::overlay::HeadRate;
using nte::combat_stats::overlay::HitRegion;
using nte::combat_stats::overlay::HitTest;
using nte::combat_stats::overlay::SortColumn;
using nte::combat_stats::overlay::SortPlayers;
using nte::combat_stats::overlay::WeakRate;
using namespace std::chrono_literals;

constexpr wchar_t kWindowClass[] = L"NteCombatOverlay.Window.v1";
constexpr wchar_t kWindowTitle[] = L"NTE Combat Overlay";
constexpr wchar_t kMutexName[] = L"Local\\NteCombatOverlay.local.nte.combat-stats";
constexpr UINT kSnapshotMessage = WM_APP + 1;
constexpr UINT kTrayMessage = WM_APP + 2;
constexpr UINT kShowMessage = WM_APP + 3;
constexpr UINT kHotkeyId = 1;
constexpr UINT_PTR kStaleTimerId = 1;
constexpr UINT kTrayId = 1;
constexpr std::size_t kMaximumMessage = nte::combat_stats::kMaximumOverlayMessageBytes;

enum MenuId : UINT {
    MenuToggleLock = 100,
    MenuToggleCollapsed,
    MenuToggleTopmost,
    MenuToggleHideIdle,
    MenuToggleTheme,
    MenuToggleHead,
    MenuToggleWeak,
    MenuToggleActiveDps,
    MenuToggleRollingDps,
    MenuOpacity60,
    MenuOpacity75,
    MenuOpacity90,
    MenuScale100,
    MenuScale125,
    MenuScale150,
    MenuShow,
    MenuHide,
    MenuExit,
};

struct Settings {
    int x{100};
    int y{100};
    int width{520};
    int height{220};
    bool locked{};
    bool collapsed{};
    bool topmost{true};
    bool hide_idle{};
    bool light_theme{};
    bool show_head{};
    bool show_weak{};
    bool show_active_dps{};
    bool show_rolling_dps{};
    float opacity{0.90F};
    float ui_scale{1.0F};
    SortColumn sort_column{SortColumn::Damage};
    bool descending{true};
};

struct OverlayColumn {
    std::wstring_view label;
    SortColumn sort;
    float weight;
};

std::vector<OverlayColumn> VisibleColumns(const Settings& settings) {
    std::vector<OverlayColumn> result{
        {L"角色", SortColumn::Character, 2.2F},
        {L"伤害", SortColumn::Damage, 1.2F},
        {L"占比", SortColumn::Share, 0.9F},
        {L"DPS", SortColumn::EncounterDps, 1.0F},
        {L"暴击", SortColumn::CriticalRate, 0.9F},
        {L"最大一击", SortColumn::MaximumHit, 1.2F},
    };
    if (settings.show_head) result.push_back({L"爆头", SortColumn::HeadRate, 0.9F});
    if (settings.show_weak) result.push_back({L"弱点", SortColumn::WeakRate, 0.9F});
    if (settings.show_active_dps)
        result.push_back({L"活跃 DPS", SortColumn::ActiveDps, 1.1F});
    if (settings.show_rolling_dps)
        result.push_back({L"10s DPS", SortColumn::RollingDps, 1.1F});
    return result;
}

struct RenderResources {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> write;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteTextFormat> regular;
    ComPtr<IDWriteTextFormat> small_format;
    ComPtr<IDWriteTextFormat> bold;
};

struct App {
    HINSTANCE instance{};
    HWND window{};
    UINT taskbar_created{};
    std::uint16_t port{14514};
    Settings settings;
    std::filesystem::path settings_path;
    RenderResources render;
    std::jthread receiver;
    std::mutex snapshot_mutex;
    std::optional<NteOverlaySnapshot> snapshot;
    std::string connection_status{"正在连接 Runtime…"};
    std::chrono::steady_clock::time_point last_snapshot{};
    std::chrono::steady_clock::time_point stopping_since{};
    bool connected{};
    bool dragging{};
    POINT drag_origin{};
    RECT window_origin{};
};

App g_app;

std::wstring Utf16(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return L"?";
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::filesystem::path SettingsPath() {
    PWSTR local{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local))) {
        return {};
    }
    std::filesystem::path result(local);
    CoTaskMemFree(local);
    return result / L"NTECombatStats" / L"overlay-settings.json";
}

void LoadSettings() {
    g_app.settings_path = SettingsPath();
    std::ifstream stream(g_app.settings_path, std::ios::binary);
    if (!stream) return;
    try {
        const nlohmann::json json = nlohmann::json::parse(stream);
        auto& value = g_app.settings;
        value.x = json.value("x", value.x);
        value.y = json.value("y", value.y);
        value.width = std::clamp(json.value("width", value.width), 360, 1400);
        value.height = std::clamp(json.value("height", value.height), 80, 1000);
        value.locked = json.value("locked", value.locked);
        value.collapsed = json.value("collapsed", value.collapsed);
        value.topmost = json.value("topmost", value.topmost);
        value.hide_idle = json.value("hide_idle", value.hide_idle);
        value.light_theme = json.value("light_theme", value.light_theme);
        value.show_head = json.value("show_head", value.show_head);
        value.show_weak = json.value("show_weak", value.show_weak);
        value.show_active_dps = json.value("show_active_dps", value.show_active_dps);
        value.show_rolling_dps = json.value("show_rolling_dps", value.show_rolling_dps);
        value.opacity = std::clamp(json.value("opacity", value.opacity), 0.4F, 1.0F);
        value.ui_scale = std::clamp(json.value("ui_scale", value.ui_scale), 0.75F, 2.0F);
        value.sort_column = static_cast<SortColumn>(std::clamp(
            json.value("sort_column", static_cast<int>(value.sort_column)), 0, 9));
        value.descending = json.value("descending", value.descending);
    } catch (...) {
        // A malformed settings file is ignored; defaults remain usable.
    }
}

void SaveSettings() {
    if (g_app.settings_path.empty()) return;
    RECT rect{};
    if (g_app.window != nullptr && GetWindowRect(g_app.window, &rect)) {
        g_app.settings.x = rect.left;
        g_app.settings.y = rect.top;
        if (!g_app.settings.collapsed) {
            g_app.settings.width = rect.right - rect.left;
            g_app.settings.height = rect.bottom - rect.top;
        }
    }
    std::error_code error;
    std::filesystem::create_directories(g_app.settings_path.parent_path(), error);
    if (error) return;
    const auto temporary = g_app.settings_path.parent_path() / L"overlay-settings.json.tmp";
    const auto& value = g_app.settings;
    const nlohmann::json json{
        {"schema_version", 1}, {"x", value.x}, {"y", value.y},
        {"width", value.width}, {"height", value.height}, {"locked", value.locked},
        {"collapsed", value.collapsed}, {"topmost", value.topmost},
        {"hide_idle", value.hide_idle}, {"show_head", value.show_head},
        {"light_theme", value.light_theme},
        {"show_weak", value.show_weak}, {"show_active_dps", value.show_active_dps},
        {"show_rolling_dps", value.show_rolling_dps}, {"opacity", value.opacity},
        {"ui_scale", value.ui_scale},
        {"sort_column", static_cast<int>(value.sort_column)},
        {"descending", value.descending}};
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return;
        stream << json.dump(2) << '\n';
        stream.flush();
        if (!stream) return;
    }
    MoveFileExW(temporary.c_str(), g_app.settings_path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void AddTrayIcon() {
    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = g_app.window;
    icon.uID = kTrayId;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = kTrayMessage;
    icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(icon.szTip, kWindowTitle);
    Shell_NotifyIconW(NIM_ADD, &icon);
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = g_app.window;
    icon.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &icon);
}

void ApplyWindowMode() {
    LONG_PTR extended = GetWindowLongPtrW(g_app.window, GWL_EXSTYLE);
    extended |= WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    if (g_app.settings.locked) extended |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    else extended &= ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    SetWindowLongPtrW(g_app.window, GWL_EXSTYLE, extended);
    SetWindowPos(g_app.window, g_app.settings.topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

void DrawTextLine(ID2D1RenderTarget* target, IDWriteTextFormat* format,
                  ID2D1Brush* brush, const std::wstring& text,
                  const D2D1_RECT_F rect) {
    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
        rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

std::wstring CompactNumber(const double value) {
    const double absolute = std::abs(value);
    if (absolute >= 1'000'000'000.0) return std::format(L"{:.2f}B", value / 1'000'000'000.0);
    if (absolute >= 1'000'000.0) return std::format(L"{:.2f}M", value / 1'000'000.0);
    if (absolute >= 1'000.0) return std::format(L"{:.1f}K", value / 1'000.0);
    return std::format(L"{:.0f}", value);
}

std::wstring StateText(const NteOverlaySnapshot& snapshot) {
    if (!snapshot.capture_available) return L"采集不可用";
    switch (snapshot.combat.state) {
    case EncounterState::Idle: return L"等待战斗";
    case EncounterState::Active: return L"Active";
    case EncounterState::Completed: return L"Completed";
    }
    return L"等待战斗";
}

std::wstring CellText(const PlayerRow& row, const SortColumn column) {
    switch (column) {
    case SortColumn::Character: return Utf16(row.label);
    case SortColumn::Damage: return CompactNumber(static_cast<double>(row.damage));
    case SortColumn::Share: return std::format(L"{:.1f}%", row.damage_share);
    case SortColumn::EncounterDps: return CompactNumber(row.encounter_dps);
    case SortColumn::CriticalRate: return std::format(L"{:.1f}%", CriticalRate(row));
    case SortColumn::MaximumHit: return CompactNumber(static_cast<double>(row.maximum_hit));
    case SortColumn::HeadRate: return std::format(L"{:.1f}%", HeadRate(row));
    case SortColumn::WeakRate: return std::format(L"{:.1f}%", WeakRate(row));
    case SortColumn::ActiveDps: return CompactNumber(row.active_dps);
    case SortColumn::RollingDps: return CompactNumber(row.rolling_dps);
    }
    return {};
}

void RenderOverlay() {
    RECT client{};
    GetClientRect(g_app.window, &client);
    const UINT width = static_cast<UINT>(std::max(1L, client.right));
    const UINT height = static_cast<UINT>(std::max(1L, client.bottom));
    if (!g_app.render.wic || !g_app.render.d2d) return;

    ComPtr<IWICBitmap> bitmap;
    if (FAILED(g_app.render.wic->CreateBitmap(width, height,
            GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) return;
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1RenderTarget> target;
    if (FAILED(g_app.render.d2d->CreateWicBitmapRenderTarget(
            bitmap.Get(), properties, &target))) return;
    ComPtr<ID2D1SolidColorBrush> text;
    ComPtr<ID2D1SolidColorBrush> muted;
    ComPtr<ID2D1SolidColorBrush> accent;
    ComPtr<ID2D1SolidColorBrush> warning;
    const bool light_theme = g_app.settings.light_theme;
    target->CreateSolidColorBrush(D2D1::ColorF(
        light_theme ? 0x15202B : 0xF3F6FA, 1.0F), &text);
    target->CreateSolidColorBrush(D2D1::ColorF(
        light_theme ? 0x516170 : 0xAAB4C0, 1.0F), &muted);
    target->CreateSolidColorBrush(D2D1::ColorF(0x55B8FF, 0.32F), &accent);
    target->CreateSolidColorBrush(D2D1::ColorF(0xFFB454, 1.0F), &warning);

    std::optional<NteOverlaySnapshot> snapshot;
    std::string status;
    bool connected{};
    {
        std::scoped_lock lock(g_app.snapshot_mutex);
        snapshot = g_app.snapshot;
        status = g_app.connection_status;
        connected = g_app.connected;
    }
    const float dpi = static_cast<float>(GetDpiForWindow(g_app.window)) / 96.0F;
    const float scale = dpi * g_app.settings.ui_scale;
    const std::size_t player_count = snapshot ? snapshot->combat.players.size() : 0;
    const auto layout = CalculateLayout(
        static_cast<float>(width), static_cast<float>(height), scale,
        g_app.settings.collapsed, player_count);
    ComPtr<IDWriteTextFormat> scaled_regular;
    ComPtr<IDWriteTextFormat> scaled_small;
    ComPtr<IDWriteTextFormat> scaled_bold;
    auto scaled_format = [&](const float size, const DWRITE_FONT_WEIGHT weight,
                             ComPtr<IDWriteTextFormat>& output) {
        if (FAILED(g_app.render.write->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
                weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                size * scale, L"zh-CN", &output))) return false;
        output->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        output->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    };
    if (!scaled_format(14.0F, DWRITE_FONT_WEIGHT_NORMAL, scaled_regular) ||
        !scaled_format(12.0F, DWRITE_FONT_WEIGHT_NORMAL, scaled_small) ||
        !scaled_format(14.0F, DWRITE_FONT_WEIGHT_SEMI_BOLD, scaled_bold)) return;

    target->BeginDraw();
    target->Clear(D2D1::ColorF(
        light_theme ? 0xEDF2F7 : 0x10151D, g_app.settings.opacity));
    target->FillRectangle(D2D1::RectF(0, 0, layout.width, layout.title_height),
        accent.Get());
    std::wstring title = L"NTE Combat";
    std::wstring state = Utf16(status);
    if (snapshot) {
        state = connected ? StateText(*snapshot) : L"连接断开";
        const int seconds = static_cast<int>(snapshot->combat.duration_seconds);
        title += std::format(L"  {:02}:{:02}  {}", seconds / 60, seconds % 60, state);
        if (!snapshot->primary_target.empty()) title += L"  ·  " + Utf16(snapshot->primary_target);
        if (snapshot->combat.quality.partial || snapshot->combat.quality.overflow ||
            snapshot->combat.quality.cursor_gap ||
            snapshot->combat.quality.aggregate_truncated) title += L"  ⚠ DATA";
    }
    title += g_app.settings.collapsed ? L"  ▾" : L"  ▴";
    title += g_app.settings.locked ? L"  🔒" : L"  🔓";
    DrawTextLine(target.Get(), scaled_bold.Get(), text.Get(), title,
        D2D1::RectF(12 * scale, 0, layout.width - 12 * scale, layout.title_height));

    if (!g_app.settings.collapsed) {
        const float top = layout.title_height;
        const auto columns = VisibleColumns(g_app.settings);
        float total_weight{};
        for (const auto& column : columns) total_weight += column.weight;
        float x{};
        for (const auto& column : columns) {
            const float width_share = column.weight / total_weight;
            DrawTextLine(target.Get(), scaled_small.Get(), muted.Get(),
                std::wstring(column.label),
                D2D1::RectF(x + 8 * scale, top, x + layout.width * width_share,
                            top + layout.header_height));
            x += layout.width * width_share;
        }
        if (!snapshot || snapshot->combat.players.empty()) {
            const std::wstring message = snapshot ? StateText(*snapshot) : Utf16(status);
            DrawTextLine(target.Get(), scaled_regular.Get(), muted.Get(), message,
                D2D1::RectF(12 * scale, top + layout.header_height + 12 * scale,
                            layout.width - 12 * scale, layout.height));
        } else {
            auto players = SortPlayers(snapshot->combat.players,
                g_app.settings.sort_column, g_app.settings.descending);
            const auto maximum_row = std::ranges::max_element(
                players, {}, &PlayerRow::damage);
            const double maximum = maximum_row == players.end()
                ? 1.0 : std::max(1.0, static_cast<double>(maximum_row->damage));
            for (std::size_t index = 0; index < layout.visible_rows; ++index) {
                const PlayerRow& row = players[index];
                const float row_top = top + layout.header_height +
                    static_cast<float>(index) * layout.row_height;
                const float bar_width = layout.width * static_cast<float>(
                    std::clamp(static_cast<double>(row.damage) / maximum, 0.0, 1.0));
                target->FillRectangle(D2D1::RectF(0, row_top, bar_width,
                    row_top + layout.row_height - 1), accent.Get());
                float column_x{};
                for (std::size_t column = 0; column < columns.size(); ++column) {
                    const float width_share = columns[column].weight / total_weight;
                    DrawTextLine(target.Get(), scaled_regular.Get(), text.Get(),
                        CellText(row, columns[column].sort),
                        D2D1::RectF(column_x + 8 * scale, row_top,
                        column_x + layout.width * width_share,
                        row_top + layout.row_height));
                    column_x += layout.width * width_share;
                }
            }
        }
    }
    if (FAILED(target->EndDraw())) return;

    WICRect lock_rect{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
    ComPtr<IWICBitmapLock> bitmap_lock;
    if (FAILED(bitmap->Lock(&lock_rect, WICBitmapLockRead, &bitmap_lock))) return;
    UINT stride{};
    UINT bytes{};
    BYTE* pixels{};
    bitmap_lock->GetStride(&stride);
    bitmap_lock->GetDataPointer(&bytes, &pixels);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* dib_pixels{};
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &dib_pixels, nullptr, 0);
    if (dib && dib_pixels) {
        for (UINT row = 0; row < height; ++row) {
            memcpy(static_cast<BYTE*>(dib_pixels) + row * width * 4,
                pixels + row * stride, width * 4);
        }
        const HGDIOBJ old = SelectObject(memory, dib);
        RECT window_rect{};
        GetWindowRect(g_app.window, &window_rect);
        POINT destination{window_rect.left, window_rect.top};
        POINT source{};
        SIZE size{static_cast<LONG>(width), static_cast<LONG>(height)};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(g_app.window, screen, &destination, &size,
            memory, &source, 0, &blend, ULW_ALPHA);
        SelectObject(memory, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

void SetConnectionStatus(std::string status, const bool connected) {
    {
        std::scoped_lock lock(g_app.snapshot_mutex);
        g_app.connection_status = std::move(status);
        g_app.connected = connected;
    }
    if (g_app.window) PostMessageW(g_app.window, kSnapshotMessage, 0, 0);
}

struct HttpHandle {
    HINTERNET value{};
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    HttpHandle() = default;
    explicit HttpHandle(HINTERNET handle) : value(handle) {}
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
};

void ReceiveSnapshots(const std::stop_token stop) {
    while (!stop.stop_requested()) {
        SetConnectionStatus("正在连接 Runtime…", false);
        HttpHandle session(WinHttpOpen(L"NteCombatOverlay/0.3.0",
            WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0));
        if (session.value) WinHttpSetTimeouts(session.value, 1000, 1000, 1000, 1000);
        HttpHandle connection(session.value ? WinHttpConnect(
            session.value, L"127.0.0.1", g_app.port, 0) : nullptr);
        HttpHandle request(connection.value ? WinHttpOpenRequest(connection.value, L"GET", L"/",
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr);
        if (!request.value || !WinHttpSetOption(request.value,
                WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
            !WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.value, nullptr)) {
            SetConnectionStatus("Runtime WebSocket 未连接", false);
            for (int i = 0; i < 20 && !stop.stop_requested(); ++i) Sleep(100);
            continue;
        }
        HttpHandle socket(WinHttpWebSocketCompleteUpgrade(request.value, 0));
        if (!socket.value) {
            SetConnectionStatus("WebSocket 握手失败", false);
            Sleep(1000);
            continue;
        }
        SetConnectionStatus("已连接，等待 NTE 数据", true);
        std::vector<char> message;
        message.reserve(8192);
        bool reconnect{};
        while (!stop.stop_requested() && !reconnect) {
            std::array<char, 8192> chunk{};
            DWORD read{};
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            const DWORD result = WinHttpWebSocketReceive(socket.value, chunk.data(),
                static_cast<DWORD>(chunk.size()), &read, &type);
            if (result != ERROR_SUCCESS || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
            if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE &&
                type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                message.clear();
                continue;
            }
            if (message.size() + read > kMaximumMessage) {
                message.clear();
                reconnect = true;
                continue;
            }
            message.insert(message.end(), chunk.data(), chunk.data() + read);
            if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;
            std::string error;
            auto parsed = nte::combat_stats::ParseOverlaySnapshot(
                std::string_view(message.data(), message.size()), error);
            message.clear();
            if (!parsed) continue;
            bool redraw = true;
            {
                std::scoped_lock lock(g_app.snapshot_mutex);
                if (g_app.snapshot && parsed->producer_instance_id ==
                        g_app.snapshot->producer_instance_id &&
                    parsed->snapshot_sequence <= g_app.snapshot->snapshot_sequence) continue;
                if (parsed->producer_stopping) g_app.stopping_since = std::chrono::steady_clock::now();
                else g_app.stopping_since = {};
                if (g_app.snapshot && parsed->combat.state == EncounterState::Idle &&
                    g_app.snapshot->combat.state == EncounterState::Idle &&
                    parsed->capture_available == g_app.snapshot->capture_available &&
                    parsed->capture_status == g_app.snapshot->capture_status &&
                    parsed->latest_damage_sequence ==
                        g_app.snapshot->latest_damage_sequence) {
                    redraw = false;
                }
                g_app.snapshot = std::move(parsed);
                g_app.last_snapshot = std::chrono::steady_clock::now();
                g_app.connection_status = "已连接";
            }
            if (redraw) PostMessageW(g_app.window, kSnapshotMessage, 0, 0);
        }
        WinHttpWebSocketClose(socket.value, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
            nullptr, 0);
        SetConnectionStatus("连接断开，正在重连…", false);
        for (int i = 0; i < 10 && !stop.stop_requested(); ++i) Sleep(100);
    }
}

void ToggleLock() {
    g_app.settings.locked = !g_app.settings.locked;
    ApplyWindowMode();
    SaveSettings();
    RenderOverlay();
}

void ToggleCollapsed() {
    RECT rect{};
    GetWindowRect(g_app.window, &rect);
    g_app.settings.collapsed = !g_app.settings.collapsed;
    const float dpi = static_cast<float>(GetDpiForWindow(g_app.window)) / 96.0F;
    const int height = g_app.settings.collapsed
        ? static_cast<int>(std::round(34.0F * dpi * g_app.settings.ui_scale))
        : g_app.settings.height;
    SetWindowPos(g_app.window, nullptr, 0, 0, rect.right - rect.left, height,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SaveSettings();
    RenderOverlay();
}

void ShowContextMenu(const POINT screen) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_app.settings.locked ? MF_CHECKED : 0),
        MenuToggleLock, L"锁定 / 点击穿透\tCtrl+Shift+F12");
    AppendMenuW(menu, MF_STRING | (g_app.settings.collapsed ? MF_CHECKED : 0),
        MenuToggleCollapsed, L"折叠");
    AppendMenuW(menu, MF_STRING | (g_app.settings.topmost ? MF_CHECKED : 0),
        MenuToggleTopmost, L"置顶");
    AppendMenuW(menu, MF_STRING | (g_app.settings.hide_idle ? MF_CHECKED : 0),
        MenuToggleHideIdle, L"离战隐藏");
    AppendMenuW(menu, MF_STRING | (g_app.settings.light_theme ? MF_CHECKED : 0),
        MenuToggleTheme, L"浅色主题");
    HMENU columns = CreatePopupMenu();
    AppendMenuW(columns, MF_STRING | (g_app.settings.show_head ? MF_CHECKED : 0),
        MenuToggleHead, L"爆头率");
    AppendMenuW(columns, MF_STRING | (g_app.settings.show_weak ? MF_CHECKED : 0),
        MenuToggleWeak, L"弱点率");
    AppendMenuW(columns, MF_STRING | (g_app.settings.show_active_dps ? MF_CHECKED : 0),
        MenuToggleActiveDps, L"活跃 DPS");
    AppendMenuW(columns, MF_STRING | (g_app.settings.show_rolling_dps ? MF_CHECKED : 0),
        MenuToggleRollingDps, L"10 秒 DPS");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(columns), L"可选列");
    HMENU opacity = CreatePopupMenu();
    AppendMenuW(opacity, MF_STRING, MenuOpacity60, L"60%");
    AppendMenuW(opacity, MF_STRING, MenuOpacity75, L"75%");
    AppendMenuW(opacity, MF_STRING, MenuOpacity90, L"90%");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opacity), L"透明度");
    HMENU scale = CreatePopupMenu();
    AppendMenuW(scale, MF_STRING, MenuScale100, L"100%");
    AppendMenuW(scale, MF_STRING, MenuScale125, L"125%");
    AppendMenuW(scale, MF_STRING, MenuScale150, L"150%");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(scale), L"界面缩放");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MenuHide, L"隐藏");
    AppendMenuW(menu, MF_STRING, MenuExit, L"退出");
    SetForegroundWindow(g_app.window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screen.x, screen.y, 0, g_app.window, nullptr);
    DestroyMenu(menu);
}

void HandleMenu(const UINT id) {
    switch (id) {
    case MenuToggleLock: ToggleLock(); return;
    case MenuToggleCollapsed: ToggleCollapsed(); return;
    case MenuToggleTopmost: g_app.settings.topmost = !g_app.settings.topmost; ApplyWindowMode(); break;
    case MenuToggleHideIdle: g_app.settings.hide_idle = !g_app.settings.hide_idle; break;
    case MenuToggleTheme: g_app.settings.light_theme = !g_app.settings.light_theme; break;
    case MenuToggleHead: g_app.settings.show_head = !g_app.settings.show_head; break;
    case MenuToggleWeak: g_app.settings.show_weak = !g_app.settings.show_weak; break;
    case MenuToggleActiveDps: g_app.settings.show_active_dps = !g_app.settings.show_active_dps; break;
    case MenuToggleRollingDps: g_app.settings.show_rolling_dps = !g_app.settings.show_rolling_dps; break;
    case MenuOpacity60: g_app.settings.opacity = 0.60F; break;
    case MenuOpacity75: g_app.settings.opacity = 0.75F; break;
    case MenuOpacity90: g_app.settings.opacity = 0.90F; break;
    case MenuScale100: g_app.settings.ui_scale = 1.0F; break;
    case MenuScale125: g_app.settings.ui_scale = 1.25F; break;
    case MenuScale150: g_app.settings.ui_scale = 1.5F; break;
    case MenuShow: ShowWindow(g_app.window, SW_SHOWNOACTIVATE); return;
    case MenuHide: ShowWindow(g_app.window, SW_HIDE); return;
    case MenuExit: DestroyWindow(g_app.window); return;
    default: return;
    }
    SaveSettings();
    RenderOverlay();
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == g_app.taskbar_created) { AddTrayIcon(); return 0; }
    switch (message) {
    case WM_NCCALCSIZE:
        if (wparam != 0) return 0;
        break;
    case WM_CREATE:
        AddTrayIcon();
        SetTimer(window, kStaleTimerId, 1000, nullptr);
        return 0;
    case kSnapshotMessage: {
        bool hide{};
        {
            std::scoped_lock lock(g_app.snapshot_mutex);
            hide = g_app.settings.hide_idle && g_app.snapshot &&
                g_app.snapshot->combat.state == EncounterState::Idle;
        }
        ShowWindow(window, hide ? SW_HIDE : SW_SHOWNOACTIVATE);
        RenderOverlay();
        return 0;
    }
    case kShowMessage:
        ShowWindow(window, SW_SHOWNOACTIVATE);
        return 0;
    case WM_TIMER: {
        const auto now = std::chrono::steady_clock::now();
        bool exit{};
        {
            std::scoped_lock lock(g_app.snapshot_mutex);
            if (g_app.stopping_since != std::chrono::steady_clock::time_point{} &&
                now - g_app.stopping_since >= 10s) exit = true;
            if (g_app.last_snapshot != std::chrono::steady_clock::time_point{} &&
                now - g_app.last_snapshot >= 30s) exit = true;
        }
        if (exit) DestroyWindow(window);
        return 0;
    }
    case WM_HOTKEY:
        if (wparam == kHotkeyId) ToggleLock();
        return 0;
    case WM_NCHITTEST: {
        if (g_app.settings.locked) return HTTRANSPARENT;
        RECT rect{};
        GetWindowRect(window, &rect);
        const float x = static_cast<float>(GET_X_LPARAM(lparam) - rect.left);
        const float y = static_cast<float>(GET_Y_LPARAM(lparam) - rect.top);
        const float scale = static_cast<float>(GetDpiForWindow(window)) / 96.0F *
            g_app.settings.ui_scale;
        std::size_t players{};
        { std::scoped_lock lock(g_app.snapshot_mutex); if (g_app.snapshot) players = g_app.snapshot->combat.players.size(); }
        const auto layout = CalculateLayout(static_cast<float>(rect.right - rect.left),
            static_cast<float>(rect.bottom - rect.top), scale,
            g_app.settings.collapsed, players);
        switch (HitTest(layout, x, y, false)) {
        case HitRegion::Title: return HTCAPTION;
        case HitRegion::ResizeGrip: return HTBOTTOMRIGHT;
        default: return HTCLIENT;
        }
    }
    case WM_LBUTTONUP: {
        RECT rect{};
        GetClientRect(window, &rect);
        const float scale = static_cast<float>(GetDpiForWindow(window)) / 96.0F *
            g_app.settings.ui_scale;
        const float y = static_cast<float>(GET_Y_LPARAM(lparam));
        if (!g_app.settings.collapsed && y >= 34.0F * scale && y < 62.0F * scale) {
            const float fraction = static_cast<float>(GET_X_LPARAM(lparam)) /
                static_cast<float>(std::max(1L, rect.right));
            const auto columns = VisibleColumns(g_app.settings);
            float total_weight{};
            for (const auto& column : columns) total_weight += column.weight;
            std::size_t index{};
            float edge = columns[0].weight / total_weight;
            while (index + 1 < columns.size() && fraction > edge) {
                ++index;
                edge += columns[index].weight / total_weight;
            }
            if (g_app.settings.sort_column == columns[index].sort)
                g_app.settings.descending = !g_app.settings.descending;
            else { g_app.settings.sort_column = columns[index].sort; g_app.settings.descending = true; }
            SaveSettings();
            RenderOverlay();
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ClientToScreen(window, &point);
        ShowContextMenu(point);
        return 0;
    }
    case WM_COMMAND: HandleMenu(LOWORD(wparam)); return 0;
    case kTrayMessage:
        if (LOWORD(lparam) == WM_LBUTTONDBLCLK) ShowWindow(window, SW_SHOWNOACTIVATE);
        else if (LOWORD(lparam) == WM_RBUTTONUP) {
            POINT point{}; GetCursorPos(&point); ShowContextMenu(point);
        }
        return 0;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        RenderOverlay();
        return 0;
    }
    case WM_SIZE: RenderOverlay(); return 0;
    case WM_DESTROY:
        SaveSettings();
        RemoveTrayIcon();
        UnregisterHotKey(window, kHotkeyId);
        KillTimer(window, kStaleTimerId);
        if (g_app.receiver.joinable()) { g_app.receiver.request_stop(); g_app.receiver.join(); }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool InitializeRendering() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            g_app.render.d2d.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(
                g_app.render.write.ReleaseAndGetAddressOf())))) return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&g_app.render.wic)))) return false;
    auto make_format = [](IDWriteFactory* factory, float size,
                          DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** result) {
        return factory->CreateTextFormat(L"Microsoft YaHei UI", nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", result);
    };
    if (FAILED(make_format(g_app.render.write.Get(), 14.0F,
            DWRITE_FONT_WEIGHT_NORMAL, &g_app.render.regular))) return false;
    if (FAILED(make_format(g_app.render.write.Get(), 12.0F,
            DWRITE_FONT_WEIGHT_NORMAL, &g_app.render.small_format))) return false;
    if (FAILED(make_format(g_app.render.write.Get(), 14.0F,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, &g_app.render.bold))) return false;
    for (auto* format : {g_app.render.regular.Get(), g_app.render.small_format.Get(),
                         g_app.render.bold.Get()}) {
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return true;
}

std::uint16_t ParsePort() {
    int count{};
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    std::uint16_t port = 14514;
    if (arguments) {
        for (int i = 1; i + 1 < count; ++i) {
            if (wcscmp(arguments[i], L"--port") == 0) {
                wchar_t* end{};
                const unsigned long value = wcstoul(arguments[i + 1], &end, 10);
                if (end && *end == L'\0' && value >= 1 && value <= 65535)
                    port = static_cast<std::uint16_t>(value);
            }
        }
        LocalFree(arguments);
    }
    return port;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (const HWND existing = FindWindowW(kWindowClass, kWindowTitle)) {
            PostMessageW(existing, kShowMessage, 0, 0);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) { CloseHandle(mutex); return 1; }
    g_app.instance = instance;
    g_app.port = ParsePort();
    g_app.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    LoadSettings();
    if (!InitializeRendering()) { CoUninitialize(); CloseHandle(mutex); return 2; }

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class)) { CoUninitialize(); CloseHandle(mutex); return 3; }
    const int height = g_app.settings.collapsed ? 34 : g_app.settings.height;
    g_app.window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        kWindowClass, kWindowTitle, WS_POPUP | WS_THICKFRAME,
        g_app.settings.x, g_app.settings.y, g_app.settings.width, height,
        nullptr, nullptr, instance, nullptr);
    if (!g_app.window) { CoUninitialize(); CloseHandle(mutex); return 4; }
    ApplyWindowMode();
    RegisterHotKey(g_app.window, kHotkeyId, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F12);
    ShowWindow(g_app.window, SW_SHOWNOACTIVATE);
    RenderOverlay();
    g_app.last_snapshot = std::chrono::steady_clock::now();
    g_app.receiver = std::jthread(ReceiveSnapshots);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_app.render = {};
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return static_cast<int>(message.wParam);
}
