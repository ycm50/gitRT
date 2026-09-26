# ---------------------------------------------------------------------------
# 文档体检（零依赖，只用标准库）
#
#   ① 站内链接：相对路径的目标文件是否存在（最容易坏的地方：文件改名/移动后忘了改引用）
#   ② 表格：同一个表格块里每行的竖线数是否一致
#      - 比表头**多**竖线 → 单元格里有**未转义的 `|`**（GFM 会当成分隔符，整行错位）
#      - 比表头**少**竖线 → 整列缺失（内容问题，需要人来补，不是渲染错误）
#
# 用法: python tools/check-docs.py   （退出码 0 = 通过）
#
# 为什么需要它：本仓库的文档量比代码还大，而"改了文件名/粘错一行"在 Markdown 里
# 不会报错、只会静默渲染歪。这套检查抓到过：复合动作改名后残留的 2 条死链、
# 5 处代码块里未转义的 `|`、2 处两张表被逐行粘在一起。
# ---------------------------------------------------------------------------
import os
import re
import sys

# 已知的内容缺口（**不是**渲染错误）：整列缺失，GFM 会渲染成空单元格。
# 这些行由产品设计负责人补齐，不要在这里瞎填值。
# 键 = (文件, 该行第一个单元格)。文件或行号变了就重新核对。
ALLOW = {
    ('docs/产品设计.md', '33e'),   # 标签（Tag）：优先级/危险度/参数/选择类型 四列待补
    ('docs/产品设计.md', '33f'),   # 发布（Release）：同上
}

SKIP_DIRS = {'.git', 'build'}


def md_files(root):
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if fn.endswith('.md'):
                p = os.path.normpath(os.path.join(dirpath, fn)).replace('\\', '/')
                out.append(p)
    return sorted(out)


def check_tables(path, lines):
    """返回 [(行号, 问题描述)]"""
    bad = []
    in_fence = False
    i = 0
    while i < len(lines):
        if lines[i].lstrip().startswith('```'):
            in_fence = not in_fence
            i += 1
            continue
        if in_fence or not lines[i].lstrip().startswith('|'):
            i += 1
            continue
        block, start = [], i
        while i < len(lines) and lines[i].lstrip().startswith('|'):
            block.append(lines[i])
            i += 1
        # 先去掉转义竖线 \| 再统计
        cells = [len(re.sub(r'\\\|', '', b).split('|')) - 2 for b in block]
        want = max(set(cells), key=cells.count)
        for off, got in enumerate(cells):
            if got == want:
                continue
            row = re.sub(r'\\\|', '', block[off])
            first = row.split('|')[1].strip() if row.count('|') > 1 else ''
            if (path, first) in ALLOW:
                continue
            what = '多了' if got > want else '少了'
            bad.append((start + off + 1,
                        '表格列数%s（本行 %d 列，同表多数行 %d 列）：%s' % (what, got, want, first[:30])))
    return bad


def check_links(path, text):
    bad = []
    base = os.path.dirname(path)
    for m in re.finditer(r'\[[^\]]*\]\(([^)]+)\)', text):
        url = m.group(1).strip()
        if url.startswith(('http://', 'https://', '#', 'mailto:')):
            continue
        target = url.split('#')[0]
        if not target:
            continue
        if not os.path.exists(os.path.normpath(os.path.join(base, target))):
            bad.append((0, '链接目标不存在：%s' % url))
    return bad


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    files = md_files('.')
    problems = 0
    for path in files:
        with open(path, encoding='utf-8', errors='replace') as f:
            text = f.read()
        issues = check_links(path, text) + check_tables(path, text.splitlines())
        if issues:
            print('%s' % path)
            for no, desc in sorted(issues):
                print('  %s%s' % ('行 %d: ' % no if no else '', desc))
            problems += len(issues)
    print('检查 %d 个 .md 文件：%s' % (len(files), '通过' if not problems else '发现 %d 个问题' % problems))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
