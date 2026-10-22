#!/usr/bin/env python3
"""
check_regs.py —— 把手写的 stm32f407ve_regs.h 与 STM32CubeF4 的 CMSIS 设备头逐项对拍。

用法：
    python3 tools/check_regs.py /path/to/stm32f407xx.h

两个文件里同名的数值宏会被解析成整数后比较；名字只在手写头里出现的不算错
（例如 GPIO_AF7_USART1 在 HAL 里而不在 CMSIS 设备头里）。找不到 CMSIS 头时
打印提示并以 0 退出，这样在没有厂商 SDK 的机器上也不会挡住构建。
"""

import re
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
MINE = os.path.join(HERE, "..", "include", "stm32f407ve_regs.h")

IDENT = re.compile(r"\b[A-Za-z_]\w*\b")


def parse_defines(path):
    """返回 {名字: 原始表达式}，跳过函数式宏和带参数的宏。"""
    out = {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            # 函数式宏的名字后面紧跟 "("（中间不能有空格），借此与
            # "#define FOO (0x1UL << 3)" 这种"值以括号开头"的对象式宏区分开
            m = re.match(r"#define\s+(\w+)(\([^)]*\))?\s+(.+)$", line)
            if not m:
                continue
            name, params, expr = m.group(1), m.group(2), m.group(3)
            if params is not None:          # 函数式宏
                continue
            expr = expr.split("/*")[0].strip()
            out[name] = expr
    return out


def strip_suffix(expr):
    return re.sub(r"\b(0[xX][0-9A-Fa-f]+|\d+)([uU][lL]{0,2}|[lL]{1,2})?\b",
                  r"\1", expr)


class Resolver:
    def __init__(self, defines):
        self.defines = defines
        self.cache = {}

    def resolve(self, name, depth=0):
        if name in self.cache:
            return self.cache[name]
        if depth > 12 or name not in self.defines:
            raise KeyError(name)
        expr = strip_suffix(self.defines[name])
        # 把标识符逐个替换成已解析的值
        def sub(m):
            ident = m.group(0)
            if ident in ("UL", "U", "L"):
                return ident
            return str(self.resolve(ident, depth + 1))
        expr = IDENT.sub(sub, expr)
        value = eval(expr, {"__builtins__": {}}, {})   # 只含整数与运算符
        self.cache[name] = value
        return value


def main(argv):
    cmsis = argv[1] if len(argv) > 1 else ""
    if not cmsis or not os.path.exists(cmsis):
        print("[skip] 未找到 CMSIS 设备头，跳过常量对拍。")
        print("       用法: python3 tools/check_regs.py <stm32f407xx.h>")
        return 0

    mine = parse_defines(MINE)
    ref = parse_defines(cmsis)
    mine_res, ref_res = Resolver(mine), Resolver(ref)

    common, checked, mismatch, unresolved = [], 0, [], []
    for name in sorted(mine):
        if name not in ref:
            continue
        common.append(name)
        try:
            a = mine_res.resolve(name)
            b = ref_res.resolve(name)
        except Exception:
            unresolved.append(name)
            continue
        checked += 1
        if a != b:
            mismatch.append((name, a, b))

    only_mine = sorted(n for n in mine if n not in ref and not n.startswith("MB_"))

    print("对拍：%d 个同名宏，%d 个成功解析，%d 个不一致"
          % (len(common), checked, len(mismatch)))
    if only_mine:
        print("只在本文件出现的宏（%d 个）：%s" % (len(only_mine), ", ".join(only_mine)))
    if unresolved:
        print("无法解析：%s" % ", ".join(unresolved))

    for name, a, b in mismatch:
        print("  不一致 %-32s 本文件=0x%X CMSIS=0x%X" % (name, a, b))

    if mismatch:
        return 1
    print("结论：寄存器常量与 CMSIS 设备头一致。")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
