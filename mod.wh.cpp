// ==WindhawkMod==
// @id              taskbar-smooth-autohide
// @name            Smooth Taskbar Auto-Hide
// @description     Physically slides the auto-hidden taskbar in and out with a smooth, eased animation
// @version         1.0.0
// @author          you
// @include         explorer.exe
// @compilerOptions -luser32 -lshell32 -ladvapi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Smooth Taskbar Auto-Hide

Replaces the instant/native auto-hide behaviour of the taskbar with a smooth
slide animation. The taskbar window itself is moved (not faded), so it stays
visually identical to the native one.

**Requires Windows' "Automatically hide the taskbar" to be enabled.** When it
is disabled the mod does nothing and the normal taskbar behaviour is restored.

How it works: Explorer's own decisions about when to show/hide the taskbar are
intercepted (`SetWindowPos` on `Shell_TrayWnd` / `Shell_SecondaryTrayWnd`) and
turned into an eased animation. On top of that the mod adds its own bottom-edge
hover detection and an optional "show when a window is minimized" trigger.
Triggers are suppressed while a fullscreen app is in the foreground.

Direction note: "Automatic" slides toward the edge the taskbar is docked to
(down for a bottom taskbar). Forcing another direction makes the taskbar sweep
across the screen to that side, which is rarely what you want.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- animationDuration: 250
  $name: Animation duration (ms)
  $description: Length of a full slide, 50-2000. 200-300 feels snappy.
- slideDirection: auto
  $name: Slide direction
  $options:
  - auto: Automatic (toward the docked edge - down for a bottom taskbar)
  - down: Down
  - up: Up
  - left: Left
  - right: Right
- hideDelay: 300
  $name: Delay before hiding (ms)
  $description: How long the taskbar waits after the mouse leaves before sliding away (added on top of Windows' own delay).
- showOnMinimize: true
  $name: Show taskbar when a window is minimized
- minimizeVisibleTime: 1500
  $name: Visible time after minimize (ms)
  $description: How long the taskbar stays visible after a minimize, unless the mouse is on it.
- followExplorerState: false
  $name: Also follow Explorer's own show/hide requests
  $description: Leave off unless the taskbar fails to appear for some actions (e.g. flashing buttons).
- easing: easeOutCubic
  $name: Animation easing
  $options:
  - linear: Linear
  - easeOutCubic: Ease out (cubic) - smooth, modern
  - easeInOutCubic: Ease in-out (cubic)
  - easeInOutQuad: Ease in-out (quad) - gentle
  - easeOutQuint: Ease out (quint) - fast start, long settle
  - easeOutExpo: Ease out (expo) - very fast start
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <map>
#include <vector>

// ---------------------------------------------------------------- types ----

struct Settings {
    int duration = 250;
    int direction = 0;  // Dir
    int hideDelay = 300;
    bool showOnMinimize = true;
    int minimizeVisible = 1500;
    int easing = 1;
    bool followExplorer = false;
};

enum Dir { DirAuto, DirDown, DirUp, DirLeft, DirRight };
enum Edge { EdgeBottom, EdgeTop, EdgeLeft, EdgeRight };

struct TaskbarState {
    RECT shown{};        // geometry when fully visible (also gives size)
    RECT mon{};          // full monitor rect the taskbar lives on
    bool nativeHidden = false;  // what Explorer currently wants
    double lastF = -1;   // last visible fraction requested by Explorer
    double p = 0;        // 0 = shown, 1 = hidden
    double startP = 0, targetP = 0;
    double t0 = 0, dur = 0;
    bool animating = false;
    double hideAt = 0;   // 0 = not scheduled
    bool wasForced = false;
    bool dirty = false;
};

struct Job {
    HWND h;
    RECT r;
};

// -------------------------------------------------------------- globals ----

typedef BOOL(WINAPI* SetWindowPos_t)(HWND, HWND, int, int, int, int, UINT);
SetWindowPos_t SetWindowPos_Original;

CRITICAL_SECTION g_cs;  // guards g_set, g_states, g_force*
Settings g_set;
std::map<HWND, TaskbarState> g_states;
double g_forceUntil = 0, g_forceStart = 0;

std::atomic<bool> g_active{false};
std::atomic<bool> g_autoHide{false};
std::atomic<bool> g_fullscreen{false};
std::atomic<bool> g_interacting{false};
std::atomic<bool> g_busy{false};

HANDLE g_stop = nullptr, g_wake = nullptr, g_thread = nullptr;
HWINEVENTHOOK g_winEventHook = nullptr;
double g_nextPoll = 0;  // controller thread only

constexpr int kStrip = 2;  // pixels left visible when hidden (like native)

// -------------------------------------------------------------- helpers ----

double NowMs() {
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

template <class T>
T Clamp(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

double Ease(double t, int mode) {
    t = Clamp(t, 0.0, 1.0);
    switch (mode) {
        case 1: return 1 - std::pow(1 - t, 3);
        case 2: return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
        case 3: return t < 0.5 ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2) / 2;
        case 4: return 1 - std::pow(1 - t, 5);
        case 5: return t >= 1 ? 1 : 1 - std::pow(2.0, -10 * t);
        default: return t;
    }
}

bool IsTaskbarWnd(HWND h) {
    wchar_t cls[32];
    if (!GetClassNameW(h, cls, 32)) return false;
    return !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd");
}

double VisibleFraction(const RECT& r, const RECT& m) {
    RECT i;
    if (!IntersectRect(&i, &r, &m)) return 0;
    double a = (double)(i.right - i.left) * (i.bottom - i.top);
    double t = (double)(r.right - r.left) * (r.bottom - r.top);
    return t > 0 ? a / t : 1;
}

RECT ClampInto(RECT r, const RECT& m) {
    int w = r.right - r.left, h = r.bottom - r.top;
    r.left = (LONG)Clamp<int>(r.left, m.left, m.right - w < m.left ? m.left : m.right - w);
    r.top = (LONG)Clamp<int>(r.top, m.top, m.bottom - h < m.top ? m.top : m.bottom - h);
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

Edge DockEdge(const RECT& rc, const RECT& mon) {
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w >= h) return (mon.bottom - rc.bottom <= rc.top - mon.top) ? EdgeBottom : EdgeTop;
    return (mon.right - rc.right <= rc.left - mon.left) ? EdgeRight : EdgeLeft;
}

RECT HiddenRect(const RECT& shown, const RECT& mon, int dir, int strip) {
    int w = shown.right - shown.left, h = shown.bottom - shown.top;
    if (dir == DirAuto) {
        switch (DockEdge(shown, mon)) {
            case EdgeBottom: dir = DirDown; break;
            case EdgeTop: dir = DirUp; break;
            case EdgeLeft: dir = DirLeft; break;
            default: dir = DirRight; break;
        }
    }
    RECT r = shown;
    switch (dir) {
        case DirDown: r.top = mon.bottom - strip; r.bottom = r.top + h; break;
        case DirUp: r.bottom = mon.top + strip; r.top = r.bottom - h; break;
        case DirLeft: r.right = mon.left + strip; r.left = r.right - w; break;
        default: r.left = mon.right - strip; r.right = r.left + w; break;
    }
    return r;
}

RECT CurRect(const TaskbarState& s, int dir) {
    RECT hid = HiddenRect(s.shown, s.mon, dir, kStrip);
    int w = s.shown.right - s.shown.left, h = s.shown.bottom - s.shown.top;
    RECT r;
    r.left = (LONG)std::lround(s.shown.left + (hid.left - s.shown.left) * s.p);
    r.top = (LONG)std::lround(s.shown.top + (hid.top - s.shown.top) * s.p);
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

TaskbarState MakeState(HWND h) {
    TaskbarState s;
    RECT cur{};
    GetWindowRect(h, &cur);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&cur, MONITOR_DEFAULTTONEAREST), &mi);
    s.mon = mi.rcMonitor;
    s.lastF = VisibleFraction(cur, s.mon);
    s.shown = ClampInto(cur, s.mon);
    if (s.lastF <= 0.5) {
        s.nativeHidden = true;
        s.p = s.targetP = 1;
    }
    return s;
}

void EnsureStateLocked(HWND h) {
    if (h && g_states.find(h) == g_states.end()) g_states[h] = MakeState(h);
}

void EnsureStatesLocked() {
    EnsureStateLocked(FindWindowW(L"Shell_TrayWnd", nullptr));
    HWND s = nullptr;
    while ((s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr)
        EnsureStateLocked(s);
}

bool QueryAutoHide() {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    if (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) return true;
    // Fallback: the same flag as stored by Explorer (StuckRects3, byte 8, bit 0).
    BYTE buf[128];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StuckRects3",
                     L"Settings", RRF_RT_REG_BINARY, nullptr, buf, &sz) == ERROR_SUCCESS &&
        sz > 8)
        return (buf[8] & 1) != 0;
    return false;
}

bool IsFullscreenActive() {
    QUERY_USER_NOTIFICATION_STATE q;
    if (SUCCEEDED(SHQueryUserNotificationState(&q)) &&
        (q == QUNS_BUSY || q == QUNS_RUNNING_D3D_FULL_SCREEN))
        return true;

    // Borderless windows covering the whole monitor (maximized normal windows have a caption).
    HWND fg = GetForegroundWindow();
    if (!fg || IsIconic(fg) || IsTaskbarWnd(fg)) return false;
    wchar_t cls[32];
    if (GetClassNameW(fg, cls, 32) && (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")))
        return false;
    LONG_PTR style = GetWindowLongPtrW(fg, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) return false;
    RECT r;
    MONITORINFO mi{sizeof(mi)};
    if (!GetWindowRect(fg, &r) ||
        !GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi))
        return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

// True while the user is interacting with taskbar menus / has the taskbar focused.
bool IsInteracting() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) {
        DWORD tid = GetWindowThreadProcessId(tray, nullptr);
        GUITHREADINFO gti{sizeof(gti)};
        if (GetGUIThreadInfo(tid, &gti) && (gti.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE)))
            return true;
    }
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (IsTaskbarWnd(fg)) return true;
    wchar_t cls[64];
    // Start menu / Search / notification center / quick settings are CoreWindow-based.
    return GetClassNameW(fg, cls, 64) && !wcscmp(cls, L"Windows.UI.Core.CoreWindow");
}

bool MouseWants(const TaskbarState& s, POINT pt) {
    if (!PtInRect(&s.mon, pt)) return false;
    if (s.p < 0.5 && PtInRect(&s.shown, pt)) return true;  // on the visible taskbar
    switch (DockEdge(s.shown, s.mon)) {  // hot edge
        case EdgeBottom:
            return pt.y >= s.mon.bottom - 2 && pt.x >= s.shown.left && pt.x < s.shown.right;
        case EdgeTop:
            return pt.y <= s.mon.top + 1 && pt.x >= s.shown.left && pt.x < s.shown.right;
        case EdgeLeft:
            return pt.x <= s.mon.left + 1 && pt.y >= s.shown.top && pt.y < s.shown.bottom;
        default:
            return pt.x >= s.mon.right - 2 && pt.y >= s.shown.top && pt.y < s.shown.bottom;
    }
}

void ApplyJobs(const std::vector<Job>& jobs) {
    for (const Job& j : jobs) {
        if (!IsWindow(j.h) || !IsWindowVisible(j.h)) continue;
        RECT c;
        if (GetWindowRect(j.h, &c) && EqualRect(&c, &j.r)) continue;
        if (!SetWindowPos_Original(j.h, nullptr, j.r.left, j.r.top, j.r.right - j.r.left,
                                   j.r.bottom - j.r.top,
                                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                                       SWP_NOSENDCHANGING))
            Wh_Log(L"SetWindowPos failed, error %lu", GetLastError());
    }
}

// --------------------------------------------------------------- settings --

void LoadSettings() {
    Settings s;
    s.duration = Clamp(Wh_GetIntSetting(L"animationDuration"), 50, 2000);
    s.hideDelay = Clamp(Wh_GetIntSetting(L"hideDelay"), 0, 10000);
    s.showOnMinimize = Wh_GetIntSetting(L"showOnMinimize") != 0;
    s.followExplorer = Wh_GetIntSetting(L"followExplorerState") != 0;
    s.minimizeVisible = Clamp(Wh_GetIntSetting(L"minimizeVisibleTime"), 300, 15000);

    PCWSTR d = Wh_GetStringSetting(L"slideDirection");
    s.direction = !wcscmp(d, L"down") ? DirDown
                  : !wcscmp(d, L"up") ? DirUp
                  : !wcscmp(d, L"left") ? DirLeft
                  : !wcscmp(d, L"right") ? DirRight
                                         : DirAuto;
    Wh_FreeStringSetting(d);

    PCWSTR e = Wh_GetStringSetting(L"easing");
    s.easing = !wcscmp(e, L"linear") ? 0
               : !wcscmp(e, L"easeInOutCubic") ? 2
               : !wcscmp(e, L"easeInOutQuad") ? 3
               : !wcscmp(e, L"easeOutQuint") ? 4
               : !wcscmp(e, L"easeOutExpo") ? 5
                                              : 1;
    Wh_FreeStringSetting(e);

    EnterCriticalSection(&g_cs);
    g_set = s;
    for (auto& kv : g_states) kv.second.dirty = true;
    LeaveCriticalSection(&g_cs);
}

// ------------------------------------------------------------------ hook ---

BOOL WINAPI SetWindowPos_Hook(HWND hWnd, HWND hAfter, int X, int Y, int cx, int cy, UINT flags) {
    if (!g_active.load() || !g_autoHide.load() || (flags & SWP_NOMOVE) || !IsTaskbarWnd(hWnd))
        return SetWindowPos_Original(hWnd, hAfter, X, Y, cx, cy, flags);

    RECT cur;
    if (!GetWindowRect(hWnd, &cur))
        return SetWindowPos_Original(hWnd, hAfter, X, Y, cx, cy, flags);

    int w = (flags & SWP_NOSIZE) ? cur.right - cur.left : cx;
    int h = (flags & SWP_NOSIZE) ? cur.bottom - cur.top : cy;
    RECT req{X, Y, X + w, Y + h};

    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromRect(&req, MONITOR_DEFAULTTONEAREST), &mi))
        return SetWindowPos_Original(hWnd, hAfter, X, Y, cx, cy, flags);

    double f = VisibleFraction(req, mi.rcMonitor);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 40)
        Wh_Log(L"Explorer SetWindowPos taskbar: rect=(%ld,%ld,%ld,%ld) visible=%d%%", req.left,
               req.top, req.right, req.bottom, (int)(f * 100));

    EnterCriticalSection(&g_cs);
    EnsureStateLocked(hWnd);
    TaskbarState& s = g_states[hWnd];
    s.mon = mi.rcMonitor;
    if (f >= 0.98) {  // Explorer wants it fully visible
        s.shown = req;
        s.nativeHidden = false;
    } else if (f <= 0.25) {  // Explorer wants it hidden
        s.shown = ClampInto(req, s.mon);
        s.nativeHidden = true;
    } else if (f != s.lastF) {  // intermediate step of a native slide: infer direction only
        s.nativeHidden = (s.lastF < 0) ? (f < 0.5) : (f < s.lastF);
    }
    s.lastF = f;
    s.dirty = true;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_wake);

    // Position/size are driven by the animation; keep every other effect of the call.
    return SetWindowPos_Original(hWnd, hAfter, 0, 0, 0, 0, flags | SWP_NOMOVE | SWP_NOSIZE);
}

// ------------------------------------------------------- controller thread -

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD,
                           DWORD) {
    if (event != EVENT_SYSTEM_MINIMIZESTART || idObject != OBJID_WINDOW || !hwnd) return;
    if (!g_autoHide.load()) return;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return;

    double now = NowMs();
    EnterCriticalSection(&g_cs);
    if (g_set.showOnMinimize) {
        g_forceStart = now;
        g_forceUntil = now + g_set.minimizeVisible;
    }
    LeaveCriticalSection(&g_cs);
    g_nextPoll = 0;  // refresh fullscreen state right away (same thread as Tick)
}

// If something moved a taskbar without going through our hook, adopt its position
// instead of fighting it.
void CheckExternalMoveLocked(HWND h, TaskbarState& s) {
    if (s.animating || s.dirty || !IsWindowVisible(h)) return;
    RECT act;
    if (!GetWindowRect(h, &act)) return;
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromRect(&act, MONITOR_DEFAULTTONEAREST), &mi)) s.mon = mi.rcMonitor;

    RECT exp = CurRect(s, g_set.direction);
    double fa = VisibleFraction(act, s.mon), fe = VisibleFraction(exp, s.mon);
    if (fa <= 0.25 && fe <= 0.25) return;
    if (std::abs((int)(act.left - exp.left)) <= 2 && std::abs((int)(act.top - exp.top)) <= 2) return;

    Wh_Log(L"External move detected, adopting rect=(%ld,%ld) visible=%d%%", act.left, act.top,
           (int)(fa * 100));
    if (fa >= 0.98) {
        s.shown = act;
        s.nativeHidden = false;
        s.p = s.targetP = 0;
        s.hideAt = 0;
    } else if (fa <= 0.25) {
        s.shown = ClampInto(act, s.mon);
        s.nativeHidden = true;
        s.p = s.targetP = 1;
    }
}

void Poll() {
    bool ah = QueryAutoHide();
    bool was = g_autoHide.exchange(ah);
    if (ah != was) Wh_Log(L"Taskbar auto-hide is now %s", ah ? L"ON" : L"OFF");
    g_fullscreen = ah && IsFullscreenActive();
    g_interacting = ah && IsInteracting();
    static bool lastInter = false;
    if (g_interacting.load() != lastInter) {
        lastInter = g_interacting.load();
        Wh_Log(L"interacting=%d", (int)lastInter);
    }

    std::vector<Job> jobs;
    EnterCriticalSection(&g_cs);
    if (ah) {
        EnsureStatesLocked();
        static size_t lastCount = (size_t)-1;
        if (g_states.size() != lastCount) {
            lastCount = g_states.size();
            Wh_Log(L"Tracking %d taskbar window(s)", (int)lastCount);
        }
        for (auto it = g_states.begin(); it != g_states.end();) {
            if (!IsWindow(it->first)) {
                it = g_states.erase(it);
                continue;
            }
            CheckExternalMoveLocked(it->first, it->second);
            ++it;
        }
    } else if (was) {
        // Auto-hide just got disabled: hand control back, never leave a taskbar stranded.
        for (auto& kv : g_states) {
            RECT cur;
            if (GetWindowRect(kv.first, &cur) && VisibleFraction(cur, kv.second.mon) < 0.98)
                jobs.push_back({kv.first, kv.second.shown});
        }
        g_states.clear();
        g_forceUntil = 0;
    }
    LeaveCriticalSection(&g_cs);
    ApplyJobs(jobs);
}

void Tick() {
    double now = NowMs();
    if (now >= g_nextPoll) {
        Poll();
        g_nextPoll = now + 250;
    }
    if (!g_autoHide.load()) {
        g_busy = false;
        return;
    }

    POINT pt{};
    GetCursorPos(&pt);
    bool fs = g_fullscreen.load(), interacting = g_interacting.load();
    bool busy = false;
    std::vector<Job> jobs;

    EnterCriticalSection(&g_cs);
    Settings st = g_set;

    bool forceActive = false;
    if (g_forceUntil > 0) {
        if (now < g_forceUntil) {
            forceActive = true;
        } else if (interacting && now - g_forceStart < 10000) {  // keep open during menus (capped)
            g_forceUntil = now + 300;
            forceActive = true;
        } else {
            g_forceUntil = 0;
        }
    }
    if (fs) forceActive = false;

    for (auto& kv : g_states) {
        TaskbarState& s = kv.second;
        bool mouse = !fs && MouseWants(s, pt);
        // The mod decides on its own: show only while the mouse is on the taskbar/edge, after a
        // minimize, or while the taskbar/Start menu is being used. Explorer's opinion is only
        // honoured if "followExplorerState" is enabled.
        bool wantShown = forceActive || mouse || interacting ||
                         (st.followExplorer && !s.nativeHidden);
        if (forceActive) s.wasForced = true;

        double target = s.targetP;
        if (wantShown) {
            target = 0;
            s.hideAt = 0;
        } else if (s.targetP < 0.5) {
            if (s.hideAt == 0) {
                s.hideAt = now + (s.wasForced ? 0 : st.hideDelay);
                s.wasForced = false;
            }
            if (now >= s.hideAt) target = 1;
        }

        if (target != s.targetP) {
            Wh_Log(L"Taskbar %p -> %s (mouse=%d force=%d interacting=%d fullscreen=%d)", kv.first,
                   target > 0.5 ? L"HIDE" : L"SHOW", (int)mouse, (int)forceActive,
                   (int)interacting, (int)fs);
            s.startP = s.p;
            s.targetP = target;
            s.t0 = now;
            s.dur = st.duration * std::fabs(target - s.p);
            s.animating = s.dur > 0;
            if (!s.animating) s.p = target;
            s.dirty = true;
        }

        if (s.animating) {
            double t = s.dur > 0 ? (now - s.t0) / s.dur : 1;
            if (t >= 1) {
                t = 1;
                s.animating = false;
            }
            s.p = s.startP + (s.targetP - s.startP) * Ease(t, st.easing);
            if (!s.animating) s.p = s.targetP;  // always land exactly on the target
            s.dirty = true;
            busy = true;
        }

        if (s.dirty) {
            s.dirty = false;
            jobs.push_back({kv.first, CurRect(s, st.direction)});
        }
    }
    LeaveCriticalSection(&g_cs);

    g_busy = busy;
    ApplyJobs(jobs);  // never call into windows while holding the lock
}

void RestoreAll() {
    std::vector<Job> jobs;
    EnterCriticalSection(&g_cs);
    if (g_autoHide.load()) {
        for (auto& kv : g_states) {
            const TaskbarState& s = kv.second;
            RECT r = s.nativeHidden ? HiddenRect(s.shown, s.mon, DirAuto, kStrip) : s.shown;
            jobs.push_back({kv.first, r});
        }
    }
    g_states.clear();
    LeaveCriticalSection(&g_cs);
    ApplyJobs(jobs);
}

DWORD WINAPI ControllerThread(LPVOID) {
    g_winEventHook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART,
                                     nullptr, WinEventProc, 0, 0,
                                     WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HANDLE handles[2] = {g_stop, g_wake};
    for (;;) {
        DWORD r = MsgWaitForMultipleObjectsEx(2, handles, g_busy.load() ? 8 : 30, QS_ALLINPUT,
                                              MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0) break;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Tick();
    }
    if (g_winEventHook) UnhookWinEvent(g_winEventHook);
    RestoreAll();
    return 0;
}

// ------------------------------------------------------------ mod lifecycle -

BOOL Wh_ModInit() {
    InitializeCriticalSection(&g_cs);
    LoadSettings();

    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (!Wh_SetFunctionHook((void*)SetWindowPos, (void*)SetWindowPos_Hook,
                            (void**)&SetWindowPos_Original)) {
        Wh_Log(L"Failed to hook SetWindowPos");
        return FALSE;
    }

    g_active = true;
    Wh_Log(L"Smooth Taskbar Auto-Hide loaded (hook installed)");
    g_thread = CreateThread(nullptr, 0, ControllerThread, nullptr, 0, nullptr);
    return TRUE;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    if (g_wake) SetEvent(g_wake);
}

void Wh_ModUninit() {
    g_active = false;  // the hook becomes a pass-through immediately
    if (g_stop) SetEvent(g_stop);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);  // thread restores taskbar positions on exit
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stop) CloseHandle(g_stop);
    if (g_wake) CloseHandle(g_wake);
    g_stop = g_wake = nullptr;
}