#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <dbt.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cwctype>

namespace {

constexpr wchar_t APP_NAME[] = L"BatteryDock";
constexpr wchar_t WINDOW_CLASS[] = L"BatteryDockMainWindow";
constexpr wchar_t WIDGET_CLASS[] = L"BatteryDockTaskbarWidget";
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_DATA = WM_APP + 2;
constexpr UINT_PTR TIMER_UI = 1;
constexpr UINT ID_REFRESH = 100;
constexpr UINT ID_WIDGET_MODE = 101;
constexpr UINT ID_TRAY_MODE = 102;
constexpr UINT ID_STARTUP = 103;
constexpr UINT ID_EXIT = 104;
constexpr UINT ID_DEVICE_BASE = 1000;
constexpr DWORD LOW_BATTERY_LIMIT = 15;
constexpr wchar_t RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t PREF_KEY[] = L"Software\\BatteryDock";

struct Device {
    std::wstring key;
    std::wstring name;
    std::wstring source;
    int percent = -1;
    bool connected = true;
    bool charging = false;
    bool coarse = false;
};

HWND g_main = nullptr;
HWND g_widget = nullptr;
NOTIFYICONDATAW g_tray{};
HICON g_icon = nullptr;
HANDLE g_stopEvent = nullptr;
std::thread g_monitor;
std::mutex g_devicesLock;
std::unordered_map<std::wstring, Device> g_devices;
std::vector<Device> g_widgetDevices;
std::unordered_set<std::wstring> g_hidden;
std::unordered_set<std::wstring> g_startedHid;
std::mutex g_startedLock;
std::atomic<bool> g_forceRefresh{ false };
std::atomic<bool> g_widgetMode{ true };
std::atomic<bool> g_lowAlertShown{ false };
std::wstring g_widgetText = L"BatteryDock: procurando aparelhos...";
bool g_lightTheme = false;

using XGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XGetBatteryFn = DWORD(WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
HMODULE g_xinput = nullptr;
XGetStateFn g_xgetState = nullptr;
XGetBatteryFn g_xgetBattery = nullptr;

std::wstring Hex4(USHORT x) {
    wchar_t b[8]{};
    swprintf_s(b, L"%04X", x);
    return b;
}

std::wstring ShortLabel(const Device& d) {
    if (d.name.find(L"PULSE") != std::wstring::npos) return L"P3D";
    if (d.name.find(L"Xbox") != std::wstring::npos || d.name.find(L"controller") != std::wstring::npos) return L"CTRL";
    if (d.name.find(L"Mouse") != std::wstring::npos) return L"MOUSE";
    if (d.name.find(L"Keyboard") != std::wstring::npos) return L"KEY";
    return L"BAT";
}

void PutDevice(Device d) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_devicesLock);
        auto it = g_devices.find(d.key);
        changed = it == g_devices.end() || it->second.name != d.name || it->second.percent != d.percent ||
            it->second.connected != d.connected || it->second.charging != d.charging || it->second.source != d.source;
        if (changed) g_devices[d.key] = std::move(d);
    }
    if (changed && g_main) PostMessageW(g_main, WM_DATA, 0, 0);
}

void RemoveDevice(const std::wstring& key) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(g_devicesLock);
        removed = g_devices.erase(key) != 0;
    }
    if (removed && g_main) PostMessageW(g_main, WM_DATA, 0, 0);
}

std::wstring PathKey(const std::wstring& path) {
    unsigned long long h = 1469598103934665603ull;
    for (wchar_t c : path) { h ^= (unsigned short)towlower(c); h *= 1099511628211ull; }
    wchar_t b[40]{};
    swprintf_s(b, L"hid:%016llX", h);
    return b;
}

bool IsStopped() {
    return g_stopEvent && WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0;
}

void LoadXInput() {
    const wchar_t* candidates[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (auto name : candidates) {
        g_xinput = LoadLibraryW(name);
        if (g_xinput) break;
    }
    if (!g_xinput) return;
    g_xgetState = reinterpret_cast<XGetStateFn>(GetProcAddress(g_xinput, "XInputGetState"));
    g_xgetBattery = reinterpret_cast<XGetBatteryFn>(GetProcAddress(g_xinput, "XInputGetBatteryInformation"));
}

void PollXInput() {
    if (!g_xgetState || !g_xgetBattery) return;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE state{};
        std::wstring key = L"xinput:" + std::to_wstring(i);
        if (g_xgetState(i, &state) != ERROR_SUCCESS) { RemoveDevice(key); continue; }
        XINPUT_BATTERY_INFORMATION info{};
        if (g_xgetBattery(i, BATTERY_DEVTYPE_GAMEPAD, &info) != ERROR_SUCCESS ||
            info.BatteryType == BATTERY_TYPE_WIRED || info.BatteryType == BATTERY_TYPE_DISCONNECTED) {
            RemoveDevice(key);
            continue;
        }
        int pct = info.BatteryLevel == BATTERY_LEVEL_EMPTY ? 0
                : info.BatteryLevel == BATTERY_LEVEL_LOW ? 25
                : info.BatteryLevel == BATTERY_LEVEL_MEDIUM ? 60
                : info.BatteryLevel == BATTERY_LEVEL_FULL ? 100 : -1;
        Device d;
        d.key = key;
        d.name = L"Xbox controller " + std::to_wstring(i + 1);
        d.source = L"XInput";
        d.percent = pct;
        d.coarse = true;
        PutDevice(std::move(d));
    }
}

struct HidInfo {
    std::wstring path;
    USHORT vid = 0, pid = 0;
    HIDP_CAPS caps{};
    HIDP_VALUE_CAPS battery{};
};

std::vector<HidInfo> EnumerateHid() {
    std::vector<HidInfo> out;
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA iface{ sizeof(iface) };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &iface); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &needed, nullptr);
        if (!needed) continue;
        std::vector<BYTE> detailBuf(needed);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, needed, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{ sizeof(attr) };
        PHIDP_PREPARSED_DATA pp = nullptr;
        HIDP_CAPS caps{};
        if (!HidD_GetAttributes(h, &attr) || !HidD_GetPreparsedData(h, &pp) || HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS) {
            if (pp) HidD_FreePreparsedData(pp);
            CloseHandle(h);
            continue;
        }

        HidInfo info;
        info.path = detail->DevicePath;
        info.vid = attr.VendorID;
        info.pid = attr.ProductID;
        info.caps = caps;
        bool genericBattery = false;
        if (caps.NumberInputValueCaps) {
            USHORT count = caps.NumberInputValueCaps;
            std::vector<HIDP_VALUE_CAPS> values(count);
            if (HidP_GetValueCaps(HidP_Input, values.data(), &count, pp) == HIDP_STATUS_SUCCESS) {
                for (USHORT n = 0; n < count; ++n) {
                    const auto& c = values[n];
                    const bool hasBatteryUsage = c.IsRange
                        ? c.Range.UsageMin <= 0x20 && c.Range.UsageMax >= 0x20
                        : c.NotRange.Usage == 0x20;
                    if (c.UsagePage == 0x06 && hasBatteryUsage) {
                        info.battery = c;
                        genericBattery = true;
                        break;
                    }
                }
            }
        }
        HidD_FreePreparsedData(pp);
        CloseHandle(h);
        if ((info.vid == 0x054c && info.pid == 0x0d5e && caps.UsagePage == 0xff01 && caps.Usage == 0x20) || genericBattery)
            out.push_back(std::move(info));
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

bool ReadInputReport(HANDLE h, std::vector<BYTE>& report, DWORD timeoutMs, DWORD& bytesRead) {
    bytesRead = 0;
    if (report.empty()) return false;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    DWORD read = 0;
    BOOL ok = ReadFile(h, report.data(), (DWORD)report.size(), &read, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        DWORD wait = WaitForSingleObject(ov.hEvent, timeoutMs);
        if (wait == WAIT_OBJECT_0) ok = GetOverlappedResult(h, &ov, &read, FALSE);
        else { CancelIoEx(h, &ov); GetOverlappedResult(h, &ov, &read, TRUE); ok = FALSE; }
    }
    CloseHandle(ov.hEvent);
    bytesRead = read;
    return ok && read > 0;
}

std::wstring HidProductName(HANDLE h, const HidInfo& info) {
    wchar_t name[128]{};
    if (HidD_GetProductString(h, name, sizeof(name) - sizeof(wchar_t)) && name[0]) return name;
    return L"HID device " + Hex4(info.vid) + L":" + Hex4(info.pid);
}

void ListenHid(HidInfo info) {
    const bool pulse = info.vid == 0x054c && info.pid == 0x0d5e && info.caps.UsagePage == 0xff01 && info.caps.Usage == 0x20;
    const std::wstring key = pulse ? L"pulse3d:054C:0D5E" : PathKey(info.path);
    HANDLE h = CreateFileW(info.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) { RemoveDevice(key); return; }
    PHIDP_PREPARSED_DATA pp = nullptr;
    if (!HidD_GetPreparsedData(h, &pp)) { CloseHandle(h); RemoveDevice(key); return; }

    Device device;
    device.key = key;
    device.name = pulse ? L"Sony PULSE 3D" : HidProductName(h, info);
    device.source = pulse ? L"Sony USB dongle" : L"Standard HID battery";
    device.percent = -1;
    if (pulse) {
        device.connected = true;
        device.source = L"waiting for first headset report";
        PutDevice(device);
    }

    std::vector<BYTE> report(std::max<USHORT>(info.caps.InputReportByteLength, 8), 0);
    while (!IsStopped()) {
        DWORD bytesRead = 0;
        if (!ReadInputReport(h, report, 1000, bytesRead)) continue;
        if (pulse) {
            if (report.size() < 8 || report[0] != 0xb0) continue;
            device.connected = (report[4] & 0x08) != 0;
            if (report[3] == 0x80) device.charging = true;
            else if (report[3] <= 100) { device.charging = false; device.percent = report[3]; }
            device.source = device.percent < 0 ? L"waiting for first battery level" : L"Sony USB dongle";
        } else {
            ULONG value = 0;
            NTSTATUS status = HidP_GetUsageValue(HidP_Input, info.battery.UsagePage,
                info.battery.LinkCollection, 0x20, &value, pp, (PCHAR)report.data(), bytesRead);
            if (status != HIDP_STATUS_SUCCESS) continue;
            LONG lo = info.battery.LogicalMin;
            LONG hi = info.battery.LogicalMax;
            if (hi <= lo) continue;
            device.percent = (int)std::clamp<LONG>((((LONG)value - lo) * 100) / (hi - lo), 0, 100);
        }
        PutDevice(device);
    }
    HidD_FreePreparsedData(pp);
    CloseHandle(h);
    RemoveDevice(key);
    std::lock_guard<std::mutex> startedLock(g_startedLock);
    g_startedHid.erase(info.path);
}

std::wstring FindHeadsetControl() {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring local(self);
    local.resize(local.find_last_of(L"\\/") + 1);
    local += L"headsetcontrol.exe";
    if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) return local;
    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, L"headsetcontrol.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return L"";
}

std::string ReadHeadsetControlJson(const std::wstring& exe) {
    if (exe.empty()) return {};
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return {};
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"" + exe + L"\" -b -o json --timeout 1500";
    BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!created) { CloseHandle(readPipe); return {}; }

    std::string output;
    ULONGLONG deadline = GetTickCount64() + 12000;
    char buffer[4096];
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available) {
            DWORD got = 0;
            if (ReadFile(readPipe, buffer, sizeof(buffer), &got, nullptr) && got) output.append(buffer, got);
        }
        DWORD state = WaitForSingleObject(pi.hProcess, 20);
        if (state == WAIT_OBJECT_0) {
            DWORD availableAfterExit = 0;
            if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &availableAfterExit, nullptr) && availableAfterExit) continue;
            break;
        }
        if (GetTickCount64() >= deadline) { TerminateProcess(pi.hProcess, 1); break; }
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(readPipe);
    return output;
}

std::string JsonStringField(const std::string& object, const char* field) {
    std::string marker = std::string("\"") + field + "\"";
    size_t p = object.find(marker);
    if (p == std::string::npos || (p = object.find(':', p + marker.size())) == std::string::npos) return {};
    p = object.find('"', p + 1);
    if (p == std::string::npos) return {};
    size_t end = object.find('"', p + 1);
    if (end == std::string::npos) return {};
    return object.substr(p + 1, end - p - 1);
}

int JsonIntField(const std::string& object, const char* field) {
    std::string marker = std::string("\"") + field + "\"";
    size_t p = object.find(marker);
    if (p == std::string::npos || (p = object.find(':', p + marker.size())) == std::string::npos) return -1;
    ++p;
    while (p < object.size() && (object[p] == ' ' || object[p] == '\t')) ++p;
    try { return std::stoi(object.substr(p)); } catch (...) { return -1; }
}

std::string JsonObjectField(const std::string& object, const char* field) {
    std::string marker = std::string("\"") + field + "\"";
    size_t p = object.find(marker);
    if (p == std::string::npos || (p = object.find('{', p + marker.size())) == std::string::npos) return {};
    int depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = p; i < object.size(); ++i) {
        char c = object[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') quoted = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return object.substr(p, i - p + 1);
    }
    return {};
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), nullptr, 0);
    std::wstring out(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), out.data(), len);
    return out;
}

void PollHeadsetControl() {
    const std::wstring exe = FindHeadsetControl();
    if (exe.empty()) return;
    const std::string json = ReadHeadsetControlJson(exe);
    if (json.empty()) return;

    std::unordered_set<std::wstring> seen;
    size_t list = json.find("\"devices\"");
    size_t p = list == std::string::npos ? std::string::npos : json.find('[', list);
    if (p != std::string::npos) {
        for (++p; p < json.size();) {
            size_t begin = json.find('{', p);
            if (begin == std::string::npos) break;
            int depth = 0; bool quoted = false, escaped = false; size_t end = begin;
            for (; end < json.size(); ++end) {
                char c = json[end];
                if (quoted) { if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') quoted = false; continue; }
                if (c == '"') quoted = true;
                else if (c == '{') ++depth;
                else if (c == '}' && --depth == 0) break;
            }
            if (end >= json.size()) break;
            const std::string object = json.substr(begin, end - begin + 1);
            const std::string vid = JsonStringField(object, "id_vendor");
            const std::string pid = JsonStringField(object, "id_product");
            const std::string name = JsonStringField(object, "device");
            const std::string battery = JsonObjectField(object, "battery");
            const std::string status = JsonStringField(battery, "status");
            const int level = JsonIntField(battery, "level");
            if (!vid.empty() && !pid.empty() && !name.empty() && !status.empty()) {
                auto norm = [](std::string part) {
                    std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c) { return (char)tolower(c); });
                    if (part.rfind("0x", 0) == 0) part.erase(0, 2);
                    if (part.size() < 4) part.insert(0, 4 - part.size(), '0');
                    return part;
                };
                std::string id = norm(vid) + ":" + norm(pid);
                if (id != "054c:0d5e") {
                    std::wstring key = L"hsc:" + Utf8ToWide(id);
                    Device d;
                    d.key = key; d.name = Utf8ToWide(name); d.source = L"HeadsetControl";
                    d.connected = status == "BATTERY_AVAILABLE" || status == "BATTERY_CHARGING";
                    d.charging = status == "BATTERY_CHARGING";
                    d.percent = level >= 0 && level <= 100 ? level : -1;
                    seen.insert(key);
                    PutDevice(std::move(d));
                }
            }
            p = end + 1;
            if (p < json.size() && json[p] == ']') break;
        }
    }
    std::vector<std::wstring> stale;
    {
        std::lock_guard<std::mutex> lock(g_devicesLock);
        for (const auto& [key, d] : g_devices)
            if (key.rfind(L"hsc:", 0) == 0 && !seen.count(key)) stale.push_back(key);
    }
    for (const auto& key : stale) RemoveDevice(key);
}

void MonitorLoop() {
    LoadXInput();
    ULONGLONG lastScan = 0;
    ULONGLONG lastXInput = 0;
    ULONGLONG lastHeadsetControl = 0;
    while (!IsStopped()) {
        ULONGLONG now = GetTickCount64();
        if (g_forceRefresh.exchange(false)) { lastScan = 0; lastXInput = 0; lastHeadsetControl = 0; }
        if (now - lastXInput >= 2000) { PollXInput(); lastXInput = now; }
        if (now - lastScan >= 3000) {
            for (auto& info : EnumerateHid()) {
                bool start = false;
                {
                    std::lock_guard<std::mutex> lock(g_startedLock);
                    start = g_startedHid.insert(info.path).second;
                }
                if (start) std::thread(ListenHid, std::move(info)).detach();
            }
            lastScan = now;
        }
        if (now - lastHeadsetControl >= 60000) { PollHeadsetControl(); lastHeadsetControl = GetTickCount64(); }
        WaitForSingleObject(g_stopEvent, 100);
    }
    if (g_xinput) FreeLibrary(g_xinput);
}

std::wstring CurrentTaskbarText() {
    g_widgetDevices.clear();
    {
        std::lock_guard<std::mutex> lock(g_devicesLock);
        for (const auto& [key, d] : g_devices)
            if (d.connected && !g_hidden.count(key)) g_widgetDevices.push_back(d);
    }
    std::sort(g_widgetDevices.begin(), g_widgetDevices.end(), [](const Device& a, const Device& b) { return a.name < b.name; });
    std::wstring text;
    bool low = false;
    for (const auto& d : g_widgetDevices) {
        if (d.percent >= 0 && d.percent <= (int)LOW_BATTERY_LIMIT) low = true;
        if (!text.empty()) text += L"   ";
        text += ShortLabel(d) + L" ";
        text += d.percent < 0 ? L"?" : (d.coarse ? L"~" : L"") + std::to_wstring(d.percent) + L"%";
    }
    if (text.empty()) text = L"BatteryDock: conecte um aparelho compat\x00EDvel";
    if (low && !g_lowAlertShown.exchange(true)) {
        g_tray.uFlags = NIF_INFO;
        wcscpy_s(g_tray.szInfoTitle, L"Bateria baixa");
        wcscpy_s(g_tray.szInfo, L"Um aparelho conectado est\x00E1 com 15% ou menos.");
        g_tray.dwInfoFlags = NIIF_WARNING;
        Shell_NotifyIconW(NIM_MODIFY, &g_tray);
    } else if (!low) g_lowAlertShown.store(false);
    return text;
}

enum class DeviceIcon { Headset, Mouse, Keyboard, Controller, Battery };

DeviceIcon IconForDevice(const Device& d) {
    std::wstring name = d.name;
    std::transform(name.begin(), name.end(), name.begin(), towupper);
    if (name.find(L"PULSE") != std::wstring::npos || name.find(L"HEADSET") != std::wstring::npos || name.find(L"HEADPHONE") != std::wstring::npos)
        return DeviceIcon::Headset;
    if (name.find(L"MOUSE") != std::wstring::npos) return DeviceIcon::Mouse;
    if (name.find(L"KEYBOARD") != std::wstring::npos) return DeviceIcon::Keyboard;
    if (name.find(L"XBOX") != std::wstring::npos || name.find(L"CONTROLLER") != std::wstring::npos || name.find(L"GAMEPAD") != std::wstring::npos)
        return DeviceIcon::Controller;
    return DeviceIcon::Battery;
}

wchar_t DeviceGlyph(const Device& d) {
    switch (IconForDevice(d)) {
        case DeviceIcon::Headset: return 0xE7F6;  // Headphone, same glyph as Wireless Battery
        case DeviceIcon::Mouse: return 0xE962;    // Mouse
        case DeviceIcon::Keyboard: return 0xE765; // KeyboardClassic
        case DeviceIcon::Controller: return 0xE7FC; // Game
        default: return d.source.find(L"Bluetooth") != std::wstring::npos ? 0xE702 : 0xE772;
    }
}

HFONT MakeDeviceIconFont(int pixels) {
    const wchar_t* faces[] = { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" };
    for (const wchar_t* face : faces) {
        HFONT font = CreateFontW(-pixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ old = SelectObject(dc, font);
        wchar_t installed[LF_FACESIZE]{};
        GetTextFaceW(dc, LF_FACESIZE, installed);
        SelectObject(dc, old);
        DeleteDC(dc);
        if (!_wcsicmp(installed, face) || face == faces[1]) return font;
        DeleteObject(font);
    }
    return nullptr;
}

std::wstring DevicePercentText(const Device& d) {
    if (d.percent < 0) return L"?";
    return (d.coarse ? L"~" : L"") + std::to_wstring(d.percent) + L"%";
}

bool IsShellSurfaceWindow(HWND window) {
    if (!window || window == GetShellWindow()) return true;
    wchar_t className[64]{};
    if (!GetClassNameW(window, className, (int)std::size(className))) return false;
    return !_wcsicmp(className, L"Progman") || !_wcsicmp(className, L"WorkerW") ||
        !_wcsicmp(className, L"Shell_TrayWnd") || !_wcsicmp(className, L"Shell_SecondaryTrayWnd");
}

bool IsFullscreenForegroundWindow() {
    QUERY_USER_NOTIFICATION_STATE shellState = QUNS_NOT_PRESENT;
    if (SUCCEEDED(SHQueryUserNotificationState(&shellState)) &&
        (shellState == QUNS_RUNNING_D3D_FULL_SCREEN || shellState == QUNS_PRESENTATION_MODE))
        return true;

    HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == g_main || foreground == g_widget || IsShellSurfaceWindow(foreground) ||
        !IsWindowVisible(foreground)) return false;

    RECT windowRect{};
    if (!GetWindowRect(foreground, &windowRect)) return false;

    HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{ sizeof(monitorInfo) };
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) return false;

    const RECT& screen = monitorInfo.rcMonitor;
    constexpr LONG tolerance = 2;
    return windowRect.left <= screen.left + tolerance &&
        windowRect.top <= screen.top + tolerance &&
        windowRect.right >= screen.right - tolerance &&
        windowRect.bottom >= screen.bottom - tolerance;
}

void PlaceWidget() {
    if (!g_widget || !g_widgetMode.load()) { if (g_widget) ShowWindow(g_widget, SW_HIDE); return; }
    if (IsFullscreenForegroundWindow()) { ShowWindow(g_widget, SW_HIDE); return; }
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND trayArea = taskbar ? FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    RECT taskRect{}, trayRect{};
    if (!taskbar || !trayArea || !GetWindowRect(taskbar, &taskRect) || !GetWindowRect(trayArea, &trayRect)) {
        ShowWindow(g_widget, SW_HIDE);
        return;
    }
    HDC dc = GetDC(g_widget);
    HFONT font = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT iconFont = MakeDeviceIconFont(16);
    HGDIOBJ old = SelectObject(dc, font);
    int width = 0;
    int height = 24;
    if (g_widgetDevices.empty()) {
        SIZE size{};
        GetTextExtentPoint32W(dc, g_widgetText.c_str(), (int)g_widgetText.size(), &size);
        width = (int)size.cx + 12;
        height = (int)std::max<LONG>(height, size.cy + 4);
    } else {
        width = 6;
        for (size_t i = 0; i < g_widgetDevices.size(); ++i) {
            SIZE size{};
            std::wstring pct = DevicePercentText(g_widgetDevices[i]);
            GetTextExtentPoint32W(dc, pct.c_str(), (int)pct.size(), &size);
            HGDIOBJ oldIconFont = SelectObject(dc, iconFont);
            wchar_t glyph = DeviceGlyph(g_widgetDevices[i]);
            SIZE iconSize{};
            GetTextExtentPoint32W(dc, &glyph, 1, &iconSize);
            SelectObject(dc, oldIconFont);
            width += (int)iconSize.cx + 4 + (int)size.cx;
            if (i + 1 < g_widgetDevices.size()) width += 12;
        }
        width += 6;
    }
    SelectObject(dc, old); DeleteObject(iconFont); DeleteObject(font); ReleaseDC(g_widget, dc);
    width = std::max(40, width);
    int x = (int)std::max<LONG>(taskRect.left + 8, trayRect.left - width - 3);
    int y = taskRect.top + ((taskRect.bottom - taskRect.top) - height) / 2;
    HWND foreground = GetForegroundWindow();
    if (foreground && foreground != g_main && foreground != g_widget && !IsShellSurfaceWindow(foreground)) {
        RECT foregroundRect{}, widgetRect{ x, y, x + width, y + height }, overlap{};
        if (GetWindowRect(foreground, &foregroundRect) && IntersectRect(&overlap, &foregroundRect, &widgetRect)) {
            ShowWindow(g_widget, SW_HIDE);
            return;
        }
    }
    SetWindowPos(g_widget, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_widget, nullptr, TRUE);
}

void UpdateUI() {
    g_widgetText = CurrentTaskbarText();
    PlaceWidget();
    if (g_tray.hWnd) {
        std::wstring tip = g_widgetText;
        if (tip.size() > 120) tip.resize(120);
        wcsncpy_s(g_tray.szTip, tip.c_str(), _TRUNCATE);
        g_tray.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &g_tray);
    }
}

bool StartupEnabled() {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    bool enabled = RegQueryValueExW(key, APP_NAME, nullptr, &type, nullptr, &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    return enabled;
}

void SetStartup(bool enabled) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    if (enabled) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(key, APP_NAME, 0, REG_SZ, (const BYTE*)command.c_str(), (DWORD)((command.size() + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(key, APP_NAME);
    RegCloseKey(key);
}

void ShowMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Click a device to show or hide it");
    std::vector<Device> list;
    {
        std::lock_guard<std::mutex> lock(g_devicesLock);
        for (const auto& [key, d] : g_devices) list.push_back(d);
    }
    std::sort(list.begin(), list.end(), [](const Device& a, const Device& b) { return a.name < b.name; });
    std::vector<std::wstring> keys;
    UINT id = ID_DEVICE_BASE;
    if (list.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No battery devices detected");
    for (const auto& d : list) {
        std::wstring label = d.name + L": ";
        if (!d.connected) label += L"off / not connected";
        else if (d.percent >= 0) label += (d.coarse ? L"~" : L"") + std::to_wstring(d.percent) + L"%" + (d.charging ? L" (charging)" : L"");
        else label += L"waiting for battery";
        if (!d.source.empty()) label += L" [" + d.source + L"]";
        const UINT flags = MF_STRING | (g_hidden.count(d.key) ? 0 : MF_CHECKED);
        AppendMenuW(menu, flags, id++, label.c_str());
        keys.push_back(d.key);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MFT_RADIOCHECK | (g_widgetMode.load() ? MF_CHECKED : 0), ID_WIDGET_MODE,
        L"Show on the taskbar (icon + percentage)");
    AppendMenuW(menu, MF_STRING | MFT_RADIOCHECK | (!g_widgetMode.load() ? MF_CHECKED : 0), ID_TRAY_MODE,
        L"Show as tray icons");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_REFRESH, L"Refresh now");
    AppendMenuW(menu, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), ID_STARTUP, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");
    POINT p{}; GetCursorPos(&p);
    SetForegroundWindow(g_main);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, g_main, nullptr);
    DestroyMenu(menu);
    if (cmd >= ID_DEVICE_BASE && cmd < ID_DEVICE_BASE + keys.size()) {
        const auto& key = keys[cmd - ID_DEVICE_BASE];
        if (!g_hidden.erase(key)) g_hidden.insert(key);
        UpdateUI();
    } else if (cmd == ID_WIDGET_MODE) { g_widgetMode.store(true); UpdateUI(); }
    else if (cmd == ID_TRAY_MODE) { g_widgetMode.store(false); UpdateUI(); }
    else if (cmd == ID_REFRESH) { g_forceRefresh.store(true); PostMessageW(g_main, WM_DATA, 0, 0); }
    else if (cmd == ID_STARTUP) SetStartup(!StartupEnabled());
    else if (cmd == ID_EXIT) PostMessageW(g_main, WM_CLOSE, 0, 0);
}

LRESULT CALLBACK WidgetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT r{}; GetClientRect(hwnd, &r);
        HBRUSH key = CreateSolidBrush(RGB(1, 2, 3));
        FillRect((HDC)wp, &r, key);
        DeleteObject(key);
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
        SetBkMode(dc, TRANSPARENT);
        COLORREF color = g_lightTheme ? RGB(32,32,32) : RGB(245,245,245);
        SetTextColor(dc, color);
        HFONT font = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT iconFont = MakeDeviceIconFont(16);
        HGDIOBJ old = SelectObject(dc, font);
        RECT r{}; GetClientRect(hwnd, &r);
        if (g_widgetDevices.empty()) {
            DrawTextW(dc, g_widgetText.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        } else {
            int x = 6;
            int y = (r.bottom - 18) / 2;
            for (const auto& device : g_widgetDevices) {
                wchar_t glyph = DeviceGlyph(device);
                HGDIOBJ oldIconFont = SelectObject(dc, iconFont);
                SIZE iconSize{};
                GetTextExtentPoint32W(dc, &glyph, 1, &iconSize);
                TextOutW(dc, x, y + (18 - (int)iconSize.cy) / 2, &glyph, 1);
                SelectObject(dc, oldIconFont);
                x += (int)iconSize.cx + 4;
                std::wstring pct = DevicePercentText(device);
                RECT textRect{ x, 0, r.right, r.bottom };
                DrawTextW(dc, pct.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
                SIZE size{};
                GetTextExtentPoint32W(dc, pct.c_str(), (int)pct.size(), &size);
                x += (int)size.cx + 12;
            }
        }
        SelectObject(dc, old); DeleteObject(iconFont); DeleteObject(font); EndPaint(hwnd, &ps); return 0;
    }
    if (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP) { ShowMenu(); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TRAY) {
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU || LOWORD(lp) == WM_LBUTTONUP) ShowMenu();
        return 0;
    }
    if (msg == WM_DATA) { UpdateUI(); return 0; }
    if (msg == WM_DEVICECHANGE) { UpdateUI(); return TRUE; }
    if (msg == WM_TIMER && wp == TIMER_UI) { PlaceWidget(); return 0; }
    if (msg == WM_DESTROY) {
        if (g_stopEvent) SetEvent(g_stopEvent);
        if (g_monitor.joinable()) g_monitor.join();
        Shell_NotifyIconW(NIM_DELETE, &g_tray);
        if (g_widget) DestroyWindow(g_widget);
        if (g_xinput) FreeLibrary(g_xinput);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_lightTheme = false;
    HKEY theme{}; DWORD v = 0, size = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_QUERY_VALUE, &theme) == ERROR_SUCCESS) {
        RegQueryValueExW(theme, L"SystemUsesLightTheme", nullptr, nullptr, (BYTE*)&v, &size);
        RegCloseKey(theme); g_lightTheme = v != 0;
    }
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = instance; wc.lpfnWndProc = MainProc; wc.lpszClassName = WINDOW_CLASS;
    RegisterClassExW(&wc);
    WNDCLASSEXW widget{ sizeof(widget) };
    widget.hInstance = instance; widget.lpfnWndProc = WidgetProc; widget.lpszClassName = WIDGET_CLASS;
    widget.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&widget);
    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, WINDOW_CLASS, APP_NAME, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!g_main) return 1;
    g_widget = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, WIDGET_CLASS, L"",
        WS_POPUP, 0, 0, 100, 24, nullptr, nullptr, instance, nullptr);
    SetLayeredWindowAttributes(g_widget, RGB(1, 2, 3), 0, LWA_COLORKEY);
    g_icon = LoadIconW(nullptr, IDI_APPLICATION);
    g_tray = NOTIFYICONDATAW{ sizeof(g_tray) };
    g_tray.hWnd = g_main; g_tray.uID = 1; g_tray.uCallbackMessage = WM_TRAY;
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; g_tray.hIcon = g_icon;
    wcscpy_s(g_tray.szTip, L"BatteryDock");
    Shell_NotifyIconW(NIM_ADD, &g_tray);
    g_tray.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &g_tray);
    SetTimer(g_main, TIMER_UI, 2000, nullptr);
    g_monitor = std::thread(MonitorLoop);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_stopEvent) CloseHandle(g_stopEvent);
    return 0;
}

