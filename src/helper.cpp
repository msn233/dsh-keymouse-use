// helper.cpp — 常驻键鼠控制 helper(Windows x64)
// 通过 stdin 逐行接收 JSON 指令,即时调用 SendInput 模拟键盘/鼠标。
// 不装全局钩子(非监听型),而是"指令型":来一条执行一条,stdin 关闭即退出。
// 响应:每个指令输出一行 JSON {"ok":true} 或 {"ok":false,"error":"..."} 到 stdout。
//
// 协议(每行一个 json,字段名见下):
//   {"cmd":"press","key":"space"}                      按下某键(不抬起)
//   {"cmd":"up","key":"space"}                         抬起某键
//   {"cmd":"tap","key":"enter"}                        按下+抬起(单键,不卡)
//   {"cmd":"mpress","button":"left"}                   按下鼠标按钮(不抬起)
//   {"cmd":"mmove","x":800,"y":450}                    绝对移动鼠标(主屏基准)
//   {"cmd":"mroll","up":true,"amount":2}               滚动(amount 默认1)
//   {"cmd":"release_all"}                              抬起全部已按住的键/鼠标按钮
//   {"cmd":"ping"}                                    存活检测,返回 ok
//
// 编译:g++ -O2 -std=c++17 src/helper.cpp -o bin/helper-x64.exe -luser32 -lwinmm

#define NOMINMAX
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <mmsystem.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cwchar>
#include <cctype>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdiplus.lib")

// ============================ 迷你 JSON 解析(仅对象/字符串/数字/布尔) ============================
struct JsonVal {
    enum Type { Null, Bool, Num, Str, Obj } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::map<std::string, JsonVal> obj;

    bool isNum() const { return type == Num; }
    bool isStr() const { return type == Str; }
    bool isBool() const { return type == Bool; }
    const JsonVal* get(const std::string& k) const {
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s), i_(0) {}

    bool parse(JsonVal& out) {
        skipWs();
        if (!parseValue(out)) return false;
        skipWs();
        return i_ >= s_.size();
    }

private:
    const std::string& s_;
    size_t i_;

    void skipWs() { while (i_ < s_.size() && isspace((unsigned char)s_[i_])) i_++; }

    bool parseValue(JsonVal& out) {
        skipWs();
        if (i_ >= s_.size()) return false;
        char c = s_[i_];
        if (c == '{') return parseObj(out);
        if (c == '"') { out.type = JsonVal::Str; return parseStr(out.str); }
        if (c == 't' || c == 'f') return parseBool(out);
        if (c == 'n') { out.type = JsonVal::Null; i_ += 4; return true; } // null
        if (c == '-' || isdigit((unsigned char)c)) return parseNum(out);
        return false;
    }

    bool parseStr(std::string& out) {
        if (s_[i_] != '"') return false;
        i_++;
        out.clear();
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_++];
            if (c == '\\' && i_ < s_.size()) {
                char e = s_[i_++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        if (i_ >= s_.size()) return false;
        i_++; // closing quote
        return true;
    }

    bool parseBool(JsonVal& out) {
        if (s_.compare(i_, 4, "true") == 0) { out.type = JsonVal::Bool; out.b = true; i_ += 4; return true; }
        if (s_.compare(i_, 5, "false") == 0) { out.type = JsonVal::Bool; out.b = false; i_ += 5; return true; }
        return false;
    }

    bool parseNum(JsonVal& out) {
        size_t start = i_;
        if (s_[i_] == '-') i_++;
        while (i_ < s_.size() && isdigit((unsigned char)s_[i_])) i_++;
        if (i_ < s_.size() && s_[i_] == '.') {
            i_++;
            while (i_ < s_.size() && isdigit((unsigned char)s_[i_])) i_++;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            i_++;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) i_++;
            while (i_ < s_.size() && isdigit((unsigned char)s_[i_])) i_++;
        }
        std::string tok = s_.substr(start, i_ - start);
        out.type = JsonVal::Num;
        out.num = atof(tok.c_str());
        return true;
    }

    bool parseObj(JsonVal& out) {
        if (s_[i_] != '{') return false;
        i_++;
        out.type = JsonVal::Obj;
        skipWs();
        if (i_ < s_.size() && s_[i_] == '}') { i_++; return true; }
        while (true) {
            skipWs();
            if (i_ >= s_.size() || s_[i_] != '"') return false;
            std::string key;
            if (!parseStr(key)) return false;
            skipWs();
            if (i_ >= s_.size() || s_[i_] != ':') return false;
            i_++;
            JsonVal val;
            if (!parseValue(val)) return false;
            out.obj[key] = val;
            skipWs();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') { i_++; continue; }
            if (s_[i_] == '}') { i_++; return true; }
            return false;
        }
    }
};

// ============================ 键位表 ============================
static WORD keyToVk(const std::string& k) {
    if (k.empty()) return 0;
    // 单个字母/数字字符 -> 对应 VK
    if (k.size() == 1) {
        char c = k[0];
        if (isalpha((unsigned char)c)) return (WORD)toupper((unsigned char)c); // A-Z => 0x41..0x5A
        if (isdigit((unsigned char)c)) return (WORD)c;                        // 0-9 => 0x30..0x39
        // 常见标点直接映射
        switch (c) {
            case ' ': return VK_SPACE;
            case '.': return VK_OEM_PERIOD;
            case ',': return VK_OEM_COMMA;
            case ';': return VK_OEM_1;
            case '/': return VK_OEM_2;
            case '[': return VK_OEM_4;
            case ']': return VK_OEM_6;
            case '-': return VK_OEM_MINUS;
            case '=': return VK_OEM_PLUS;
            default: return 0;
        }
    }
    // 具名键
    if (k == "space") return VK_SPACE;
    if (k == "enter" || k == "return") return VK_RETURN;
    if (k == "tab") return VK_TAB;
    if (k == "esc" || k == "escape") return VK_ESCAPE;
    if (k == "backspace") return VK_BACK;
    if (k == "delete" || k == "del") return VK_DELETE;
    if (k == "insert") return VK_INSERT;
    if (k == "home") return VK_HOME;
    if (k == "end") return VK_END;
    if (k == "pageup" || k == "pgup") return VK_PRIOR;
    if (k == "pagedown" || k == "pgdn") return VK_NEXT;
    if (k == "up") return VK_UP;
    if (k == "down") return VK_DOWN;
    if (k == "left") return VK_LEFT;
    if (k == "right") return VK_RIGHT;
    if (k == "ctrl" || k == "control") return VK_CONTROL;
    if (k == "lctrl") return VK_LCONTROL;
    if (k == "rctrl") return VK_RCONTROL;
    if (k == "alt" || k == "menu") return VK_MENU;
    if (k == "lalt" || k == "lmenu") return VK_LMENU;
    if (k == "ralt" || k == "rmenu") return VK_RMENU;
    if (k == "shift") return VK_SHIFT;
    if (k == "lshift") return VK_LSHIFT;
    if (k == "rshift") return VK_RSHIFT;
    if (k == "win" || k == "lwin") return VK_LWIN;
    if (k == "rwin") return VK_RWIN;
    if (k == "caps" || k == "capslock") return VK_CAPITAL;
    // F1..F24(常用 F1-F12)
    if (k.size() >= 2 && (k[0] == 'f' || k[0] == 'F') && isdigit((unsigned char)k[1])) {
        int n = atoi(k.c_str() + 1);
        if (n >= 1 && n <= 24) return (WORD)(VK_F1 + (n - 1));
    }
    return 0;
}

// ============================ SendInput 辅助 ============================
static bool sendKey(WORD vk, bool up) {
    INPUT in = {0};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = 0;
    in.ki.dwExtraInfo = 0;
    if (up) in.ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

// 鼠标按钮 -> down/up flags + xbutton
struct MouseBtn { DWORD down; DWORD up; DWORD xbtn = 0; };
static bool parseMouseBtn(const std::string& b, MouseBtn& out) {
    if (b == "left")  { out.down = MOUSEEVENTF_LEFTDOWN;  out.up = MOUSEEVENTF_LEFTUP; return true; }
    if (b == "right") { out.down = MOUSEEVENTF_RIGHTDOWN; out.up = MOUSEEVENTF_RIGHTUP; return true; }
    if (b == "middle" || b == "mid") { out.down = MOUSEEVENTF_MIDDLEDOWN; out.up = MOUSEEVENTF_MIDDLEUP; return true; }
    if (b == "x1" || b == "xbutton1") { out.down = MOUSEEVENTF_XDOWN; out.up = MOUSEEVENTF_XUP; out.xbtn = XBUTTON1; return true; }
    if (b == "x2" || b == "xbutton2") { out.down = MOUSEEVENTF_XDOWN; out.up = MOUSEEVENTF_XUP; out.xbtn = XBUTTON2; return true; }
    return false;
}

static bool sendMouseClick(DWORD flag, DWORD xbtn) {
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flag;
    in.mi.mouseData = xbtn;
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

// 绝对移动:以主屏为基准,像素 -> 0..65535 归一化
static bool sendMouseMove(int x, int y) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0) sw = 1;
    if (sh <= 0) sh = 1;
    // 夹取到主屏范围内
    if (x < 0) x = 0;
    if (x > sw) x = sw;
    if (y < 0) y = 0;
    if (y > sh) y = sh;
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    in.mi.dx = (LONG)((double)x * 65535.0 / (sw - 1));
    in.mi.dy = (LONG)((double)y * 65535.0 / (sh - 1));
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

static bool sendMouseWheel(int amount, bool up) {
    int delta = amount * WHEEL_DELTA;
    if (!up) delta = -delta;
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = (DWORD)((SHORT)delta);
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

// ============================ 内部状态(记录被按住的键/鼠标,供 release_all) ============================
static bool g_keyDown[256] = {false};
static bool g_mouseDown[5] = {false}; // [0]left [1]right [2]middle [3]x1 [4]x2

static void releaseAll() {
    for (int i = 0; i < 256; i++) {
        if (g_keyDown[i]) {
            sendKey((WORD)i, true);
            g_keyDown[i] = false;
        }
    }
    static const MouseBtn btns[5] = {
        { MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP },
        { MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP },
        { MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP },
        { MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON1 },
        { MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON2 },
    };
    for (int i = 0; i < 5; i++) {
        if (g_mouseDown[i]) {
            sendMouseClick(btns[i].up, btns[i].xbtn);
            g_mouseDown[i] = false;
        }
    }
}

// ============================ 屏幕截图(GDI+ -> PNG) ============================
static ULONG_PTR g_gdiplusToken = 0;
static void gdiplusStart() {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, NULL);
}
static void gdiplusStop() {
    if (g_gdiplusToken) { Gdiplus::GdiplusShutdown(g_gdiplusToken); g_gdiplusToken = 0; }
}

// 按 MIME 查找图像编码器 CLSID(image/png)。
static int findEncoderClsid(const WCHAR* mime, CLSID* out) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    Gdiplus::ImageCodecInfo* info = (Gdiplus::ImageCodecInfo*)malloc(size);
    Gdiplus::GetImageEncoders(num, size, info);
    int found = -1;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(info[i].MimeType, mime) == 0) { *out = info[i].Clsid; found = (int)i; break; }
    }
    free(info);
    return found;
}

// 抓取主屏并保存为 PNG。成功返回空串,失败返回错误文本。
static std::string captureScreen(std::string& outPath, int& outW, int& outH) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0 || sh <= 0) return "screen size invalid";
    HDC hdc = GetDC(NULL);
    HDC memdc = CreateCompatibleDC(hdc);
    HBITMAP hbmp = CreateCompatibleBitmap(hdc, sw, sh);
    if (!hbmp) { ReleaseDC(NULL, hdc); return "create bitmap failed"; }
    HGDIOBJ old = SelectObject(memdc, hbmp);
    BitBlt(memdc, 0, 0, sw, sh, hdc, 0, 0, SRCCOPY);
    SelectObject(memdc, old);

    const char* tmp = getenv("TEMP");
    if (!tmp || !*tmp) tmp = "C:\\Windows\\Temp";
    char buf[MAX_PATH];
    long long ts = (long long)GetTickCount64();
    snprintf(buf, sizeof(buf), "%s\\keymouse_shot_%lld.png", tmp, ts);
    outPath = buf;
    outW = sw; outH = sh;

    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, wpath, MAX_PATH);

    std::string err;
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromHBITMAP(hbmp, NULL);
    if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
        CLSID clsid;
        if (findEncoderClsid(L"image/png", &clsid) != -1) {
            if (bmp->Save(wpath, &clsid, NULL) != Gdiplus::Ok) err = "save png failed";
        } else {
            err = "png encoder not found";
        }
    } else {
        err = "bitmap creation failed";
    }
    delete bmp;
    DeleteObject(hbmp);
    DeleteDC(memdc);
    ReleaseDC(NULL, hdc);
    return err;
}

// ============================ 窗口工具(前台窗口/位置/置顶) ============================
static std::vector<HWND> g_topHwnds; // 本 helper 置顶过的窗口,供 release_top 统一取消

static std::string wstrToUtf8(const wchar_t* ws) {
    if (!ws) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";
    std::string s((size_t)len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, &s[0], len, nullptr, nullptr);
    return s;
}

static std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\' || c == '"') o += '\\';
        if (c == '\n') { o += "\\n"; continue; }
        if (c == '\r') { o += "\\r"; continue; }
        if (c == '\t') { o += "\\t"; continue; }
        o += c;
    }
    return o;
}

// 当前前台窗口信息。成功返回空串并把 json 字段写入 out;失败返回错误文本。
static std::string foregroundWindowInfo(std::string& outJson) {
    HWND fg = GetForegroundWindow();
    if (!fg) return "no foreground window";
    wchar_t title[512] = {0};
    GetWindowTextW(fg, title, 512);
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    wchar_t proc[1024] = {0};
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hp) {
        DWORD plen = 1024;
        QueryFullProcessImageNameW(hp, 0, proc, &plen);
        CloseHandle(hp);
    }
    wchar_t* base = wcsrchr(proc, L'\\');
    base = base ? base + 1 : proc;
    RECT rc = {0, 0, 0, 0};
    GetWindowRect(fg, &rc);
    char buf[32];
    std::string j;
    snprintf(buf, sizeof(buf), "%.0f", (double)(UINT_PTR)fg); j += "\"hwnd\":"; j += buf;
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)pid);                j += ",\"pid\":"; j += buf;
    j += ",\"title\":\"";                                                  j += jsonEsc(wstrToUtf8(title));
    j += "\",\"process\":\"";                                              j += jsonEsc(wstrToUtf8(base));
    snprintf(buf, sizeof(buf), "\",\"x\":%ld", (long)rc.left);             j += buf;
    snprintf(buf, sizeof(buf), ",\"y\":%ld", (long)rc.top);                j += buf;
    snprintf(buf, sizeof(buf), ",\"w\":%ld", (long)(rc.right - rc.left));  j += buf;
    snprintf(buf, sizeof(buf), ",\"h\":%ld", (long)(rc.bottom - rc.top));  j += buf;
    outJson = j;
    return "";
}

static void windowSetTop(HWND h, bool on) {
    if (!h) return;
    if (on) {
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (std::find(g_topHwnds.begin(), g_topHwnds.end(), h) == g_topHwnds.end()) g_topHwnds.push_back(h);
    } else {
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        g_topHwnds.erase(std::remove(g_topHwnds.begin(), g_topHwnds.end(), h), g_topHwnds.end());
    }
}

// 取窗口所属进程的可执行文件基名(如 "QQ.exe")
static std::string windowProcessName(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    wchar_t proc[1024] = {0};
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hp) {
        DWORD plen = 1024;
        QueryFullProcessImageNameW(hp, 0, proc, &plen);
        CloseHandle(hp);
    }
    wchar_t* base = wcsrchr(proc, L'\\');
    base = base ? base + 1 : proc;
    return wstrToUtf8(base);
}

// 进程名匹配:忽略大小写与 .exe 后缀
static bool processMatches(const std::string& procName, const std::string& query) {
    std::string a = procName, b = query;
    for (auto& c : a) c = (char)tolower((unsigned char)c);
    for (auto& c : b) c = (char)tolower((unsigned char)c);
    auto strip = [](std::string& s) { if (s.size() > 4 && s.compare(s.size() - 4, 4, ".exe") == 0) s = s.substr(0, s.size() - 4); };
    strip(a); strip(b);
    return a == b;
}

struct FindCtx { std::string query; HWND first; HWND best; HWND titled; };
static BOOL CALLBACK findProc(HWND h, LPARAM lp) {
    FindCtx* c = (FindCtx*)lp;
    if (!h) return TRUE;
    if (!processMatches(windowProcessName(h), c->query)) return TRUE;
    if (!c->first) c->first = h;
    wchar_t t[64] = {0}; GetWindowTextW(h, t, 64);
    bool hasTitle = t[0] != 0;
    if (!IsIconic(h) && IsWindowVisible(h)) {
        RECT rc; GetWindowRect(h, &rc);
        if ((rc.right - rc.left) > 0 && (rc.bottom - rc.top) > 0 && hasTitle) {
            if (!c->best) c->best = h;   // 可见主窗口
        }
    } else if (hasTitle && !c->titled) {
        c->titled = h;                     // 最小化/托盘隐藏但带标题的主窗口兜底
    }
    return TRUE;
}

// 按进程名枚举顶层窗口,返回第一个匹配的窗口信息(含托盘隐藏的窗口,visible 标识)
static std::string windowFindInfo(const std::string& process, std::string& outJson) {
    FindCtx ctx; ctx.query = process; ctx.first = 0; ctx.best = 0; ctx.titled = 0;
    EnumWindows(findProc, (LPARAM)&ctx);
    HWND h = ctx.best ? ctx.best : (ctx.titled ? ctx.titled : ctx.first);
    if (!h) return "no window for process: " + process;
    wchar_t title[512] = {0};
    GetWindowTextW(h, title, 512);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    RECT rc = {0, 0, 0, 0};
    GetWindowRect(h, &rc);
    std::string pn = windowProcessName(h);
    char buf[32];
    std::string j;
    snprintf(buf, sizeof(buf), "%.0f", (double)(UINT_PTR)h);  j += "\"hwnd\":"; j += buf;
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)pid);     j += ",\"pid\":"; j += buf;
    j += ",\"title\":\""; j += jsonEsc(wstrToUtf8(title));
    j += "\",\"process\":\""; j += jsonEsc(pn);
    j += "\",\"visible\":"; j += (IsWindowVisible(h) ? "true" : "false");
    snprintf(buf, sizeof(buf), ",\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld",
             (long)rc.left, (long)rc.top, (long)(rc.right - rc.left), (long)(rc.bottom - rc.top));
    j += buf;
    outJson = j;
    return "";
}

// 激活窗口:处理最小化与"最小化到托盘"(隐藏)两种情况,再置前台
static void windowActivate(HWND h) {
    if (!h) return;
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);              // 最小化 → 还原
    else if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOW);    // 托盘隐藏 → 显示
    SetForegroundWindow(h);
    BringWindowToTop(h);
    SwitchToThisWindow(h, TRUE);                             // 强制前台,绕过前台锁定
}

static void releaseTopAll() {
    for (HWND h : g_topHwnds) {
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    g_topHwnds.clear();
}

// ============================ 指令处理 ============================
static void printOk() { printf("{\"ok\":true}\n"); fflush(stdout); }
static void printErr(const std::string& e) {
    printf("{\"ok\":false,\"error\":\"");
    for (char c : e) {
        if (c == '\\' || c == '"') printf("\\");
        putchar(c);
    }
    printf("\"}\n");
    fflush(stdout);
}

static void handle(const JsonVal& v) {
    const JsonVal* cmd = v.get("cmd");
    if (!cmd || !cmd->isStr()) { printErr("missing cmd"); return; }
    const std::string& c = cmd->str;

    if (c == "ping") { printOk(); return; }

    if (c == "release_all") { releaseAll(); printOk(); return; }

    if (c == "get_window") {
        std::string j, err = foregroundWindowInfo(j);
        if (!err.empty()) { printErr(err); return; }
        printf("{\"ok\":true,%s}\n", j.c_str());
        fflush(stdout);
        return;
    }

    if (c == "win_rect") {
        const JsonVal* hw = v.get("hwnd");
        HWND h = (hw && hw->isNum()) ? (HWND)(UINT_PTR)(unsigned long long)hw->num : GetForegroundWindow();
        if (!h) { printErr("no window"); return; }
        RECT rc = {0, 0, 0, 0};
        if (!GetWindowRect(h, &rc)) { printErr("get window rect failed"); return; }
        printf("{\"ok\":true,\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld}\n",
               (long)rc.left, (long)rc.top, (long)(rc.right - rc.left), (long)(rc.bottom - rc.top));
        fflush(stdout);
        return;
    }

    if (c == "win_top") {
        const JsonVal* hw = v.get("hwnd");
        const JsonVal* on = v.get("on");
        if (!hw || !hw->isNum()) { printErr("win_top needs numeric hwnd"); return; }
        bool onFlag = on && on->isBool() ? on->b : false;
        windowSetTop((HWND)(UINT_PTR)(unsigned long long)hw->num, onFlag);
        printOk();
        return;
    }

    if (c == "release_top") { releaseTopAll(); printOk(); return; }

    if (c == "win_find") {
        const JsonVal* pr = v.get("process");
        if (!pr || !pr->isStr()) { printErr("win_find needs process"); return; }
        std::string j, err = windowFindInfo(pr->str, j);
        if (!err.empty()) { printErr(err); return; }
        printf("{\"ok\":true,%s}\n", j.c_str());
        fflush(stdout);
        return;
    }

    if (c == "win_activate") {
        const JsonVal* hw = v.get("hwnd");
        if (!hw || !hw->isNum()) { printErr("win_activate needs numeric hwnd"); return; }
        windowActivate((HWND)(UINT_PTR)(unsigned long long)hw->num);
        printOk();
        return;
    }

    if (c == "win_state") {
        const JsonVal* hw = v.get("hwnd");
        const JsonVal* act = v.get("action");
        HWND h = (hw && hw->isNum()) ? (HWND)(UINT_PTR)(unsigned long long)hw->num : GetForegroundWindow();
        if (!h) { printErr("no window"); return; }
        if (!act || !act->isStr()) { printErr("win_state needs action"); return; }
        int sw = -1;
        if (act->str == "minimize") sw = SW_MINIMIZE;
        else if (act->str == "restore") sw = SW_RESTORE;
        else if (act->str == "maximize") sw = SW_MAXIMIZE;
        if (sw < 0) { printErr("bad action"); return; }
        ShowWindow(h, sw);
        printOk();
        return;
    }

    if (c == "capture") {
        std::string path; int w = 0, h = 0;
        std::string err = captureScreen(path, w, h);
        if (!err.empty()) { printErr(err); return; }
        std::string esc;
        for (char ch : path) {
            if (ch == '\\') esc += "\\\\";
            else if (ch == '"') esc += "\\\"";
            else esc += ch;
        }
        printf("{\"ok\":true,\"path\":\"%s\",\"width\":%d,\"height\":%d}\n", esc.c_str(), w, h);
        fflush(stdout);
        return;
    }

    if (c == "press" || c == "up" || c == "tap") {
        const JsonVal* key = v.get("key");
        if (!key || !key->isStr()) { printErr("missing key"); return; }
        WORD vk = keyToVk(key->str);
        if (vk == 0) { printErr("unknown key: " + key->str); return; }
        if (c == "tap") {
            sendKey(vk, false);
            sendKey(vk, true);
        } else if (c == "press") {
            sendKey(vk, false);
            g_keyDown[vk & 0xFF] = true;
        } else { // up
            sendKey(vk, true);
            g_keyDown[vk & 0xFF] = false;
        }
        printOk();
        return;
    }

    if (c == "mpress") {
        const JsonVal* btn = v.get("button");
        if (!btn || !btn->isStr()) { printErr("missing button"); return; }
        MouseBtn mb;
        if (!parseMouseBtn(btn->str, mb)) { printErr("unknown button: " + btn->str); return; }
        sendMouseClick(mb.down, mb.xbtn);
        g_mouseDown[btn->str == "x1" || btn->str == "xbutton1" ? 3
                         : btn->str == "x2" || btn->str == "xbutton2" ? 4
                         : btn->str == "right" ? 1
                         : btn->str == "middle" || btn->str == "mid" ? 2 : 0] = true;
        printOk();
        return;
    }

    if (c == "mmove") {
        const JsonVal* x = v.get("x");
        const JsonVal* y = v.get("y");
        if (!x || !x->isNum() || !y || !y->isNum()) { printErr("mmove needs numeric x,y"); return; }
        sendMouseMove((int)x->num, (int)y->num);
        printOk();
        return;
    }

    if (c == "mroll") {
        const JsonVal* up = v.get("up");
        const JsonVal* amt = v.get("amount");
        bool upFlag = up && up->isStr() ? false : (up && up->isBool() ? up->b : true);
        int amount = amt && amt->isNum() ? (int)amt->num : 1;
        if (amount < 1) amount = 1;
        if (amount > 50) amount = 50;
        sendMouseWheel(amount, upFlag);
        printOk();
        return;
    }

    printErr("unknown cmd: " + c);
}

// ============================ 主入口 ============================
int main() {
    SetProcessDPIAware(); // DPI-aware:GetSystemMetrics/截图/mouse_move 全统一物理像素,避免缩放换算
    // 只通过管道交互,不走控制台 UI 编码(避免乱码)
    timeBeginPeriod(1); // 提高定时精度
    gdiplusStart();

    char linebuf[4096];
    std::string line;
    while (true) {
        line.clear();
        // 逐字节读一行
        while (true) {
            if (!fgets(linebuf, sizeof(linebuf), stdin)) goto done;
            line += linebuf;
            if (!line.empty() && line.back() == '\n') break;
            if (line.size() > 4096) break;
        }
        // 去掉换行
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;

        JsonParser p(line);
        JsonVal v;
        if (!p.parse(v) || v.type != JsonVal::Obj) {
            printErr("invalid json");
            continue;
        }
        handle(v);
    }

done:
    releaseAll();    // 退出前统一抬起键鼠,防止卡键
    releaseTopAll(); // 退出前统一取消本 helper 置顶过的窗口
    gdiplusStop();
    timeEndPeriod(1);
    return 0;
}
