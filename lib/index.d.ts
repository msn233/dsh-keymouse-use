import z from '@deepseek-ai/schemastery';
import type { Context } from '@deepseek-ai/cordis';
export declare const name = "tool-keymouse";
export declare const inject: string[];
export declare const Config: z<Schemastery.ObjectS<{
    helperPath: z<string, string>;
    timeoutMs: z<number, number>;
}>, Schemastery.ObjectT<{
    helperPath: z<string, string>;
    timeoutMs: z<number, number>;
}>>;
export interface KeymouseConfig {
    helperPath?: string;
    timeoutMs?: number;
}
export interface HelperResult {
    ok: boolean;
    error?: string;
    /** capture 指令返回的 PNG 绝对路径与尺寸(仅截图时)。 */
    path?: string;
    width?: number;
    height?: number;
    /** 窗口工具返回的字段(仅窗口类指令)。 */
    hwnd?: number;
    title?: string;
    pid?: number;
    process?: string;
    x?: number;
    y?: number;
    w?: number;
    h?: number;
    /** 窗口是否可见(false = 最小化到托盘隐藏状态)。 */
    visible?: boolean;
}
declare function apply(ctx: Context, config?: KeymouseConfig): void;
export { apply };
