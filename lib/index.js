// index.ts — dsh-keymouse 插件入口
// apply(ctx) 中:
//   1. spawn 常驻 helper-x64.exe(通过 stdin 发 JSON 行指令)
//   2. ctx.tools.register 七个键鼠工具
//   3. ctx.skills.register 常驻 keymouse 说明书(runtime skill)
// 只支持 Windows。helper 预编译进 bin/helper-x64.exe。
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import z from '@deepseek-ai/schemastery';
import { defineTool } from '@deepseek-ai/dsh-tools';
import { isSkillName } from '@deepseek-ai/dsh-skill';
import { KEYMOUSE_SKILL_NAME, KEYMOUSE_SKILL_DESCRIPTION, KEYMOUSE_SKILL_WHEN_TO_USE, KEYMOUSE_SKILL_CONTENT, } from './skill-content.js';
export const name = 'tool-keymouse';
export const inject = ['tools', 'skills'];
const __dirname = dirname(fileURLToPath(import.meta.url));
const DEFAULT_HELPER_PATH = join(__dirname, '..', 'bin', 'helper-x64.exe');
export const Config = z.object({
    helperPath: z.string(),
    timeoutMs: z.number().min(1),
});
const HELPER_TIMEOUT_MS = 5000;
/** 常驻 helper 客户端:一次只发一条指令,等一行 JSON 响应;helper 崩溃后自动重启。 */
class HelperClient {
    helperPath;
    timeoutMs;
    child = null;
    rl = null;
    started = false;
    chain = Promise.resolve();
    inFlight = null;
    constructor(helperPath, timeoutMs) {
        this.helperPath = helperPath;
        this.timeoutMs = timeoutMs;
    }
    ensureStarted() {
        if (!existsSync(this.helperPath)) {
            return Promise.reject(new Error(`helper not found: ${this.helperPath}`));
        }
        if (this.started && this.child !== null)
            return Promise.resolve();
        this.started = true;
        const child = spawn(this.helperPath, [], {
            stdio: ['pipe', 'pipe', 'pipe'],
            windowsHide: true,
        });
        this.child = child;
        this.rl = createInterface({ input: child.stdout });
        this.rl.on('line', (line) => this.consume(line));
        child.on('error', (err) => this.failInFlight(err));
        child.on('exit', () => {
            this.failInFlight(new Error('helper exited'));
            if (this.child === child) {
                this.child = null;
                this.started = false;
                this.rl?.close();
                this.rl = null;
            }
        });
        return new Promise((res, rej) => {
            child.once('spawn', () => res());
            child.once('error', (e) => rej(e));
        });
    }
    consume(line) {
        const cur = this.inFlight;
        if (cur === null)
            return;
        this.inFlight = null;
        clearTimeout(cur.timer);
        try {
            const parsed = JSON.parse(line.trim());
            const ok = parsed.ok === true;
            const error = typeof parsed.error === 'string' ? parsed.error : undefined;
            const path = typeof parsed.path === 'string' ? parsed.path : undefined;
            const width = typeof parsed.width === 'number' ? parsed.width : undefined;
            const height = typeof parsed.height === 'number' ? parsed.height : undefined;
            const hwnd = typeof parsed.hwnd === 'number' ? parsed.hwnd : undefined;
            const pid = typeof parsed.pid === 'number' ? parsed.pid : undefined;
            const title = typeof parsed.title === 'string' ? parsed.title : undefined;
            const process = typeof parsed.process === 'string' ? parsed.process : undefined;
            const visible = typeof parsed.visible === 'boolean' ? parsed.visible : undefined;
            const x = typeof parsed.x === 'number' ? parsed.x : undefined;
            const y = typeof parsed.y === 'number' ? parsed.y : undefined;
            const w = typeof parsed.w === 'number' ? parsed.w : undefined;
            const h = typeof parsed.h === 'number' ? parsed.h : undefined;
            // 成功/无错误时省略 error 键,避免含 undefined 属性被判为 lossy JSON;
            // 其余字段(截图/窗口)同样仅有值时才挂载。
            const base = error === undefined ? { ok } : { ok, error };
            if (path !== undefined)
                base.path = path;
            if (width !== undefined)
                base.width = width;
            if (height !== undefined)
                base.height = height;
            if (hwnd !== undefined)
                base.hwnd = hwnd;
            if (pid !== undefined)
                base.pid = pid;
            if (title !== undefined)
                base.title = title;
            if (process !== undefined)
                base.process = process;
            if (visible !== undefined)
                base.visible = visible;
            if (x !== undefined)
                base.x = x;
            if (y !== undefined)
                base.y = y;
            if (w !== undefined)
                base.w = w;
            if (h !== undefined)
                base.h = h;
            cur.resolve(base);
        }
        catch {
            cur.reject(new Error(`helper returned invalid response: ${line}`));
        }
    }
    failInFlight(err) {
        const cur = this.inFlight;
        if (cur === null)
            return;
        this.inFlight = null;
        clearTimeout(cur.timer);
        cur.reject(err instanceof Error ? err : new Error(String(err)));
    }
    /** 串行发送一条指令,helper 响应后解析返回。 */
    send(cmd) {
        const run = this.chain.then(async () => {
            await this.ensureStarted();
            if (this.child === null)
                throw new Error('helper is not running');
            return this.writeAndRead(cmd);
        });
        this.chain = run.catch(() => undefined);
        return run;
    }
    writeAndRead(cmd) {
        return new Promise((resolve, reject) => {
            const child = this.child;
            const timer = setTimeout(() => this.failInFlight(new Error('helper timed out')), this.timeoutMs);
            this.inFlight = { resolve, reject, timer };
            const line = JSON.stringify(cmd) + '\n';
            child.stdin.write(line, (err) => {
                if (err)
                    this.failInFlight(err);
            });
        });
    }
    /** 退出前统一 release_all,再掐断 stdin 并结束进程。 */
    async dispose() {
        const child = this.child;
        this.child = null;
        this.started = false;
        this.failInFlight(new Error('disposed'));
        if (child !== null && !child.killed) {
            try {
                this.inFlight = null;
                child.stdin.write('{"cmd":"release_all"}\n');
                child.stdin.end();
            }
            catch {
                // ignore
            }
            child.kill();
        }
    }
}
/** 统一的输出 schema:每次调用返回 ok/error;截图额外带 path/width/height。 */
const OUTPUT_SCHEMA = {
    type: 'object',
    additionalProperties: false,
    properties: {
        ok: { type: 'boolean', required: true },
        error: { type: 'string' },
        path: { type: 'string' },
        width: { type: 'number' },
        height: { type: 'number' },
        hwnd: { type: 'number' },
        title: { type: 'string' },
        pid: { type: 'number' },
        process: { type: 'string' },
        x: { type: 'number' },
        y: { type: 'number' },
        w: { type: 'number' },
        h: { type: 'number' },
        visible: { type: 'boolean' },
    },
};
/** 生成统一的 render:成功/失败各一条文本,尽量带上调用参数便于模型核对。 */
function renderOutput(kind) {
    return (args, value) => {
        const a = args;
        const detail = a?.key ??
            a?.button ??
            (a?.x !== undefined && a?.y !== undefined ? `${a.x},${a.y}` : undefined) ??
            undefined;
        if (value.ok) {
            return [{ type: 'text', text: detail !== undefined ? `${kind}: ${detail} 完成` : `${kind} 完成` }];
        }
        return [{ type: 'text', text: `${kind}失败: ${value.error ?? '未知错误'}` }];
    };
}
/** 截图结果的 render:成功时显示保存路径与尺寸,失败显示错误。 */
function renderScreenshot(args, value) {
    if (value.ok) {
        const text = value.path ? `截图已保存: ${value.path} (${value.width}x${value.height})` : '截图完成';
        return [{ type: 'text', text }];
    }
    return [{ type: 'text', text: `截图失败: ${value.error ?? '未知错误'}` }];
}
/** get_window 的 render:显示标题/进程/pid/位置。 */
function renderGetWindow(args, value) {
    if (value.ok) {
        const tex = `当前窗口: ${value.title ?? '(无标题)'} (${value.process ?? ''}, pid=${value.pid ?? '?'}, ${value.x ?? 0},${value.y ?? 0} ${value.w ?? 0}x${value.h ?? 0})`;
        return [{ type: 'text', text: tex }];
    }
    return [{ type: 'text', text: `get_window失败: ${value.error ?? '未知错误'}` }];
}
/** win_rect 的 render:显示位置与尺寸。 */
function renderWinRect(args, value) {
    if (value.ok) {
        const tex = `窗口位置: (${value.x ?? 0},${value.y ?? 0}) ${value.w ?? 0}x${value.h ?? 0}`;
        return [{ type: 'text', text: tex }];
    }
    return [{ type: 'text', text: `win_rect失败: ${value.error ?? '未知错误'}` }];
}
/** win_find 的 render:显示匹配窗口的句柄/标题/进程/可见性。 */
function renderFindWindow(args, value) {
    if (value.ok) {
        const tex = `找到窗口: hwnd=${value.hwnd} "${value.title ?? '(无标题)'}" (${value.process ?? ''}, pid=${value.pid ?? '?'}, visible=${value.visible ? '是' : '否,最小化到托盘'}, ${value.x ?? 0},${value.y ?? 0} ${value.w ?? 0}x${value.h ?? 0})`;
        return [{ type: 'text', text: tex }];
    }
    return [{ type: 'text', text: `win_find失败: ${value.error ?? '未知错误'}` }];
}
function apply(ctx, config = {}) {
    const helperPath = config.helperPath ?? DEFAULT_HELPER_PATH;
    const timeoutMs = config.timeoutMs ?? HELPER_TIMEOUT_MS;
    const client = new HelperClient(helperPath, timeoutMs);
    // 插件卸载时清理常驻进程。
    ctx.effect(() => async () => {
        await client.dispose();
    });
    const register = (tool) => ctx.tools.register(tool);
    register(defineTool({
        name: 'key_press',
        description: '按下某个键盘键(不抬起)。用于组合键:先 key_press 其他键,最后必须调用 release_all 抬起,否则键会一直按住。',
        parameters: {
            key: { type: 'string', required: true, description: '小写英文键名:字母 a-z、数字 0-9、space/enter/tab/esc/backspace/delete/home/end/pageup/pagedown、方向键 up/down/left/right、f1-f12、ctrl/alt/shift/win/capslock。' },
        },
        output: { schema: OUTPUT_SCHEMA, render: renderOutput('key_press') },
        async execute(args, exec) {
            if (exec.signal.aborted)
                throw new Error('aborted');
            return client.send({ cmd: 'press', key: args.key });
        },
    }));
    register(defineTool({
        name: 'key_up',
        description: '抬起某个键盘键。与 key_press 配对;也可单独使用。',
        parameters: {
            key: { type: 'string', required: true, description: '小写英文键名,同 key_press。' },
        },
        output: { schema: OUTPUT_SCHEMA, render: renderOutput('key_up') },
        async execute(args, exec) {
            if (exec.signal.aborted)
                throw new Error('aborted');
            return client.send({ cmd: 'up', key: args.key });
        },
    }));
    register(defineTool({
        name: 'key_tap',
        description: '按下并立刻抬起单个键(单个动作,不卡键)。用于点按/输入单字符或单个快捷键按键。',
        parameters: {
            key: { type: 'string', required: true, description: '小写英文键名,同 key_press。' },
        },
        output: { schema: OUTPUT_SCHEMA, render: renderOutput('key_tap') },
        async execute(args, exec) {
            if (exec.signal.aborted)
                throw new Error('aborted');
            return client.send({ cmd: 'tap', key: args.key });
        },
    }));
    register(defineTool({
        name: 'mouse_press',
        description: '只按下鼠标按钮(不抬起)。用于按住左键拖动:mouse_press("left") → mouse_move(...) → release_all。',
        parameters: {
            button: { type: 'string', required: true, description: '鼠标按钮:left/right/middle/x1/x2。' },
        },
        output: { schema: OUTPUT_SCHEMA, render: renderOutput('mouse_press') },
        async execute(args, exec) {
            if (exec.signal.aborted)
                throw new Error('aborted');
            return client.send({ cmd: 'mpress', button: args.button });
        },
    }));
    register(defineTool({
        name: 'mouse_move',
        description: '把鼠标绝对移动到屏幕坐标 (x,y)。以主屏左上为 (0,0),单屏。',
        parameters: {
            x: { type: 'number', required: true, description: '目标屏幕 X 坐标(像素)。' },
            y: { type: 'number', required: true, description: '目标屏幕 Y 坐标(像素)。' },
        },
        output: { schema: OUTPUT_SCHEMA, render: renderOutput('mouse_move') },
        async execute(args, exec) {
            if (exec.signal.aborted)
                throw new Error('aborted');
            return client.send({ cmd: 'mmove', x: Math.round(args.x), y: Math.round(args.y) });
        },
    }));
    register(defineTool({
        name: 'mouse_roll',
        description: '滚动鼠标滚轮。up 为 true 滚上、false 滚下;可指定滚动格数 amount(默认1)。',
        parameters: {
            up: { type: 'boolean', required: true, description: 'true 滚上,false 滚下。' },
            amount: { type: 'number', description: '滚动格数,默认 1。' },
        },
        output: { schema: OUTPUT_SCHEMA, render: renderOutput('mouse_roll') },
        async execute(args, exec) {
            if (exec.signal.aborted)
                throw new Error('aborted');
            return client.send({ cmd: 'mroll', up: args.up, amount: args.amount ?? 1 });
        },
    }));
    register(defineTool({
        name: 'release_all',
        description: '抬起所有仍被按住的键与鼠标按钮。每次键鼠操作结束后必须调用一次,避免卡键。',
        parameters: {},
        output: { schema: OUTPUT_SCHEMA, render: renderOutput('release_all') },
        async execute(_args, exec) {
            if (exec.signal.aborted)
                throw new Error('aborted');
            return client.send({ cmd: 'release_all' });
        },
    }));
    register(defineTool({
        name: 'screenshot',
        description: '截取当前主屏(全屏)保存为 PNG,返回图片路径与尺寸。之后用 read_image 读取该路径即可查看屏幕内容,再配合键鼠操作。',
        parameters: {},
        output: { schema: OUTPUT_SCHEMA, render: renderScreenshot },
        async execute(_args, exec) {
            if (exec.signal.aborted)
                throw new Error('aborted');
            return client.send({ cmd: 'capture' });
        },
    }));
    register(defineTool({
        name: 'get_window',
        description: '获取当前前台窗口的信息:窗口句柄 hwnd、标题 title、进程名 process、pid、位置(x,y,w,h)。用于确认目标窗口、拿到 hwnd 再给 win_rect/win_top。',
        parameters: {},
        output: { schema: OUTPUT_SCHEMA, render: renderGetWindow },
        async execute(_args, exec) {
            if (exec.signal.aborted)
                throw new Error('aborted');
            return client.send({ cmd: 'get_window' });
        },
    }));
    register(defineTool({
        name: 'win_rect',
        description: '获取窗口位置与尺寸:传入 hwnd(来自 get_window),不传默认当前前台窗口。返回 x/y/w/h。',
        parameters: {
            hwnd: { type: 'number', description: '窗口句柄(来自 get_window 的 hwnd)。可选。' },
        },
        output: { schema: OUTPUT_SCHEMA, render: renderWinRect },
        async execute(args, exec) {
            if (exec.signal.aborted)
                throw new Error('aborted');
            return client.send(args.hwnd !== undefined ? { cmd: 'win_rect', hwnd: args.hwnd } : { cmd: 'win_rect' });
        },
    }));
    register(defineTool({
        name: 'win_top',
        description: '置顶(true)/取消置顶(false)指定窗口(置顶后窗口常驻最前)。任务结束统一调 release_top 取消所有置顶,避免干扰。',
        parameters: {
            hwnd: { type: 'number', required: true, description: '窗口句柄(来自 get_window 的 hwnd)。' },
            on: { type: 'boolean', required: true, description: 'true 置顶,false 取消置顶。' },
        },
        output: { schema: OUTPUT_SCHEMA, render: renderOutput('win_top') },
        async execute(args, exec) {
            if (exec.signal.aborted)
                throw new Error('aborted');
            return client.send({ cmd: 'win_top', hwnd: Math.round(args.hwnd), on: args.on });
        },
    }));
    register(defineTool({
        name: 'release_top',
        description: '取消所有本插件置顶过的窗口,恢复默认层级。任务结束必须调用一次。',
        parameters: {},
        output: { schema: OUTPUT_SCHEMA, render: renderOutput('release_top') },
        async execute(_args, exec) {
            if (exec.signal.aborted)
                throw new Error('aborted');
            return client.send({ cmd: 'release_top' });
        },
    }));
    const winState = (action, hint) => defineTool({
        name: `win_${action}`,
        description: hint,
        parameters: {
            hwnd: { type: 'number', description: '窗口句柄(来自 get_window 的 hwnd)。可选,不传默认当前前台窗口。' },
        },
        output: { schema: OUTPUT_SCHEMA, render: renderOutput(`win_${action}`) },
        async execute(args, exec) {
            if (exec.signal.aborted)
                throw new Error('aborted');
            return client.send(args.hwnd !== undefined ? { cmd: 'win_state', action, hwnd: args.hwnd } : { cmd: 'win_state', action });
        },
    });
    register(winState('minimize', '最小化指定窗口(不传默认当前前台窗口)。'));
    register(winState('restore', '还原指定窗口(不传默认当前前台窗口):从最小化或最大化还原。'));
    register(winState('maximize', '最大化指定窗口(不传默认当前前台窗口)。'));
    register(defineTool({
        name: 'win_find',
        description: '按进程名查找窗口:返回第一个匹配的窗口句柄 hwnd、标题、进程、pid、visible(是否可见,false 表示最小化到托盘)。用于替代鼠标点任务栏图标,定位并唤起某程序窗口。',
        parameters: {
            process: { type: 'string', required: true, description: '进程名,如 "QQ"、"QQ.exe"、"firefox"(忽略大小写与 .exe 后缀)。' },
        },
        output: { schema: OUTPUT_SCHEMA, render: renderFindWindow },
        async execute(args, exec) {
            if (exec.signal.aborted)
                throw new Error('aborted');
            return client.send({ cmd: 'win_find', process: args.process });
        },
    }));
    register(defineTool({
        name: 'win_activate',
        description: '激活/前置指定窗口:自动处理最小化与"最小化到托盘(隐藏)"两种情况,再置前台。配合 win_find 使用,任务结束无需额外清理。',
        parameters: {
            hwnd: { type: 'number', required: true, description: '窗口句柄(来自 win_find 或 get_window 的 hwnd)。' },
        },
        output: { schema: OUTPUT_SCHEMA, render: renderOutput('win_activate') },
        async execute(args, exec) {
            if (exec.signal.aborted)
                throw new Error('aborted');
            return client.send({ cmd: 'win_activate', hwnd: Math.round(args.hwnd) });
        },
    }));
    // 注册常驻 skill 说明书(runtime skill,随插件分发)。
    if (isSkillName(KEYMOUSE_SKILL_NAME)) {
        ctx.skills.register({
            name: KEYMOUSE_SKILL_NAME,
            description: KEYMOUSE_SKILL_DESCRIPTION,
            whenToUse: KEYMOUSE_SKILL_WHEN_TO_USE,
            invocation: { modelInvocable: true, userInvocable: false },
            provider: 'tool-keymouse',
            source: 'runtime',
            content: KEYMOUSE_SKILL_CONTENT,
        });
    }
}
export { apply };
