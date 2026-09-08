# dsh-tool-keymouse

Windows 键鼠 + 截图 + 窗口自动化工具插件（DeepSeek Harness）。通过常驻 helper（`bin/helper-x64.exe`）走管道 + JSON 行协议调用 Win32 `SendInput` 模拟真实键盘/鼠标输入，用于桌面自动化：打开/唤起应用、登录、点击、输入、拖拽、滚动、截图观察、窗口定位/置顶。

helper 已 **DPI-aware**：`GetSystemMetrics`、截图、`mouse_move` 全部统一到物理像素（如 2560x1600）。模型给坐标 = 截图里看到的坐标 = 物理像素，无需缩放换算。

## 特性

- 键盘：`key_press` / `key_up` / `key_tap`，字母数字、空格回车、F1-F12、方向键、Ctrl/Alt/Shift、Tab、Esc 等。
- 鼠标：`mouse_press`（按住拖动）、`mouse_move`（绝对物理坐标）、`mouse_roll`（滚轮）。
- 截图：`screenshot` 抓主屏保存 PNG，返回物理像素尺寸；多模态模型直接看图。
- 窗口：`get_window` / `win_rect`（查位置）、`win_find`（按进程名找窗口）、`win_activate`（唤起，含最小化/托盘）、`win_minimize` / `win_restore` / `win_maximize`、`win_top` / `release_top`（置顶）。
- 清理：`release_all` 一键抬起所有按住项防卡键；`release_top` 取消所有置顶。
- 自带常驻 `keymouse` 说明书（runtime skill），模型通过 skill 工具读取使用规则。

## 工具清单（17）

| 工具 | 参数 | 说明 |
| --- | --- | --- |
| `screenshot()` | 无 | 截主屏 PNG，返回 path/width/height（物理像素） |
| `get_window()` | 无 | 当前前台窗口 hwnd/title/process/pid/x/y/w/h |
| `win_rect(hwnd?)` | hwnd? | 窗口位置与尺寸（缺省前台） |
| `win_find(process)` | process | 按进程名找窗口（返回 hwnd/visible，false=托盘） |
| `win_activate(hwnd)` | hwnd | 唤起/前置窗口，处理最小化+托盘 |
| `win_minimize(hwnd?)` | hwnd? | 最小化 |
| `win_restore(hwnd?)` | hwnd? | 还原 |
| `win_maximize(hwnd?)` | hwnd? | 最大化 |
| `win_top(hwnd, on)` | hwnd, on | 置顶/取消置顶 |
| `release_top()` | 无 | 取消所有本插件置顶 |
| `key_tap(key)` | key | 按下+立刻抬起 |
| `key_press(key)` | key | 只按下（组合键用） |
| `key_up(key)` | key | 抬起 |
| `mouse_press(button)` | button | 只按下鼠标按钮 |
| `mouse_move(x,y)` | x, y | 绝对移动（物理像素） |
| `mouse_roll(up, amount?)` | up, amount? | 滚轮滚动 |
| `release_all()` | 无 | 统一抬起全部按住项 |

典型组合：`win_find('QQ')` → `win_activate(hwnd)` 唤起窗口（不点图标）；`key_press('ctrl')` → `key_tap('s')` → `release_all`；`mouse_press('left')` → `mouse_move(...)` → `release_all`；`screenshot()` → `read_image(path)` 看图再操作。

## 安装

本地目录方式（开发/内网）：

```
dsh plugin --profile <name> add D:\AI_production\dsh-keymouse
```

npm 方式（发布后）：

```
dsh plugin --profile <name> add @keymouse/dsh-tool-keymouse
```

## 目录结构

```
dsh-keymouse/
  package.json          # npm 包契约(peerDeps/files/exports)
  tsconfig.json
  src/
    index.ts            # 插件入口: apply 里 spawn helper + 注册工具 + 注册skill
    helper.cpp          # helper 源码(SendInput + JSON行协议 + 键位表 + 窗口/截图)
    skill-content.ts    # keymouse 说明书内容(注册为 runtime skill)
  skills/
    keymouse.md         # 说明书可读副本(与 skill-content 一致)
  bin/
    helper-x64.exe      # 预编译 helper(Windows x64)
  lib/                  # tsc 编译产物(index.js / skill-content.js / *.d.ts)
```

## helper 协议

helper 从 stdin 逐行读 JSON，每行一条指令，每条输出一行 `{"ok":true|false,"error":...}` 到 stdout。stdin 关闭即退出（退出前自动 `release_all`）。

指令（`get_window`/`win_*` 返回窗口字段，`capture` 返回 path/width/height）：

```
{"cmd":"press","key":"space"}
{"cmd":"up","key":"space"}
{"cmd":"tap","key":"enter"}
{"cmd":"mpress","button":"left"}
{"cmd":"mmove","x":800,"y":450}
{"cmd":"mroll","up":true,"amount":1}
{"cmd":"release_all"}
{"cmd":"ping"}
{"cmd":"capture"}
{"cmd":"get_window"}
{"cmd":"win_rect","hwnd":123}
{"cmd":"win_top","hwnd":123,"on":true}
{"cmd":"release_top"}
{"cmd":"win_state","hwnd":123,"action":"minimize|restore|maximize"}
{"cmd":"win_find","process":"QQ"}
{"cmd":"win_activate","hwnd":123}
```

## 重新编译 helper

```
g++ -O2 -std=c++17 src\helper.cpp -o bin\helper-x64.exe -luser32 -lwinmm -lgdiplus -lgdi32
```

## 重新编译插件(TypeScript -> lib)

首次需把本机 DSH 的 `@deepseek-ai` 依赖接(junction)到本项目 `node_modules`（`@deepseek-ai/dsh-tools` 等类型来自 DSH 安装目录）：

```
cmd /c mklink /J "node_modules\@deepseek-ai" "C:\Users\<你的用户名>\AppData\Roaming\npm\node_modules\@deepseek-ai\dsh\node_modules\@deepseek-ai"
```

然后：

```
npm install
npx tsc
```

说明：该 junction 仅用于本机编译，不在发布白名单(`files`)内，不影响打包分发。`node_modules` 已 gitignore，`bin/helper-x64.exe`（预编译、按需分发）随包提交。

## 平台

仅支持 Windows（helper 使用 Win32 `SendInput`/`GetSystemMetrics`/`EnumWindows`）。DPI-aware，主屏物理像素坐标基准，单屏。

## License

MIT
