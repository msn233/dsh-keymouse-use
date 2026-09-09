# 键鼠控制（KeyMouse）

通过常驻 helper 用 SendInput 模拟真实键鼠输入，对当前前台窗口生效。所有工具调用结束后必须跑一次 release_all。

## 坐标与键名

- 坐标：绝对屏幕**物理**像素（helper 已 DPI-aware，无缩放换算），主屏左上角为 (0,0)，单屏。x 向右、y 向下。截图分辨率即物理像素，从图像中读到的坐标可直接用于 mouse_move，无需换算。
- 键名（小写英文）：字母 `a`-`z`、数字 `0`-`9`、`space`、`enter`、`tab`、`esc`、`backspace`、`delete`、`insert`、`home`、`end`、`pageup`、`pagedown`、方向键 `up`/`down`/`left`/`right`、`f1`-`f12`、修饰键 `ctrl`、`alt`、`shift`、`win`、`capslock`。
- 鼠标按钮：`left`、`right`、`middle`、`x1`、`x2`。

## 工具

### screenshot()
截取当前主屏（全屏）保存为 PNG，返回绝对路径 path 与物理像素尺寸 width/height。
本插件只负责截图，不提供识图；能否看图取决于宿主环境：模型支持图像输入时可用宿主的读图能力打开该 path，否则需环境另有视觉/OCR 能力或换用支持图像输入的模型。

### get_window()
获取当前前台窗口信息：窗口句柄 hwnd、标题 title、进程名 process、pid、位置 (x,y,w,h)。
用于确认目标窗口、拿到 hwnd 再定位或置顶。

### win_rect(hwnd?)
获取窗口位置与尺寸：传入 hwnd（来自 get_window），不传默认当前前台窗口。返回 x/y/w/h。

### win_find(process)
按进程名查找窗口：返回第一个匹配的窗口句柄 hwnd、标题、进程、pid、visible（是否可见，false 表示最小化到托盘）。用于替代鼠标点任务栏图标，定位并唤起某程序窗口。
例：win_find('QQ') → 拿到 hwnd，再 win_activate(hwnd) 唤起 QQ 主窗口。

### win_activate(hwnd)
激活/前置指定窗口：自动处理最小化与"最小化到托盘（隐藏）"两种情况，再置前台。配合 win_find 使用，任务结束无需额外清理。

### win_minimize(hwnd?)
最小化指定窗口：传入 hwnd（来自 get_window），不传默认当前前台窗口。

### win_restore(hwnd?)
还原指定窗口：从最小化或最大化还原到之前大小。不传默认当前前台窗口。

### win_maximize(hwnd?)
最大化指定窗口。不传默认当前前台窗口。

### win_top(hwnd, on)
置顶（true）/取消置顶（false）指定窗口。置顶后窗口常驻最前。任务结束必须统一 release_top 取消。

### release_top()
取消所有本插件置顶过的窗口，恢复默认层级。任务结束时必须调用一次。

### key_tap(key)
按下并立刻抬起单个键（单个动作，不卡键）。用于点按、输入单字符或单个快捷键按键。
例：key_tap('enter') 确认；key_tap('space') 翻页。

### key_press(key)
只按下某键、不抬起。用于组合键：先 key_press('ctrl')，再按其他键，最后 release_all。
注意：必须配合 release_all 抬起，否则键会一直按住。

### key_up(key)
抬起某键。

### mouse_press(button)
只按下鼠标按钮、不抬起。用于按住左键拖动：mouse_press('left') → mouse_move(...) → release_all。

### mouse_move(x,y)
把鼠标绝对移动到屏幕坐标 (x,y)。

### mouse_roll(up)
滚动鼠标滚轮：up 为 true 滚上、false 滚下。

### release_all()
抬起所有仍被按住的键与鼠标按钮。每次键鼠操作结束后必须调用一次，避免卡键。

## 返回

每个工具返回 ok=true 表示该命令已交给系统；返回 error 表示参数或键名不合法。screenshot 还返回 path（PNG 绝对路径）、width/height（像素尺寸）。win_find 还返回 hwnd、title、process、pid、visible、x/y/w/h。

## 典型组合

- Ctrl+S 保存：key_press('ctrl')；key_tap('s')；release_all。
- Ctrl+O 打开：key_press('ctrl')；key_tap('o')；release_all。
- 左键拖拽：mouse_press('left')；mouse_move(x2,y2)；mouse_move(x1,y1)；release_all。
- 滚到页面底部：mouse_roll(false)；mouse_roll(false)；…（可多次）。
- 截图定位：screenshot() 得 path →（环境支持看图时）查看屏幕 → 据此 mouse_move 定位。
- 唤起程序窗口（不点图标）：win_find('QQ') → win_activate(hwnd) → 前台出现 QQ 主窗口（托盘隐藏/最小化也能唤回）。

## 规范

1. 操作前先确认目标窗口与鼠标位置（可先 mouse_move 再观察结果）。
2. 用组合键/按住后，结束统一调用 release_all。
3. 置顶窗口后，任务结束统一调用 release_top 取消。
4. 工具值是即时的：SendInput 微秒级生效，不需要等待。
