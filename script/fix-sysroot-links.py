#!/usr/bin/env python3
"""用途：整理从 RK3588 下载的 sysroot，防止链接误指向 WSL 主机文件。

使用方法：
    python3 script/fix-sysroot-links.py /绝对路径/sysroot
    python3 script/fix-sysroot-links.py --help

前提：Python 3.9+；sysroot 已同步 usr/include、usr/lib 等视频开发依赖。
行为：只修改本地 sysroot 的软链接；创建 lib -> usr/lib；转换绝对链接；
      检查关键文件，并在其父目录写入 <sysroot目录名>-link-report.json。
注意：不跟随目录软链接遍历，避免 HDF5 自引用目录导致无限递归。
      非关键悬空链接仅记录；关键文件缺失或链接越界时返回非零退出码。
      重复运行会重新生成报告，因此转换数量可能为 0。
"""

import argparse
import json
import os
from pathlib import Path


def fix_links(root: Path) -> None:
    # 防止误把主机根目录作为需要修改的板端文件副本。
    if root == Path('/') or not (root / 'usr/include/stdio.h').is_file():
        raise SystemExit('错误：请指定包含 usr/include/stdio.h 的本地 sysroot 副本')

    # 板端使用合并的 /usr 布局，/lib 本身不需要再下载一份。
    lib = root / 'lib'
    if not os.path.lexists(lib):
        lib.symlink_to('usr/lib')
    elif not lib.is_symlink() or os.readlink(lib) != 'usr/lib':
        raise SystemExit('错误：已有 lib 不是预期的 lib -> usr/lib，未覆盖')

    converted = []
    for directory, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            path = Path(directory) / name
            if not path.is_symlink():
                continue
            target = os.readlink(path)
            if target.startswith('/'):
                # 例如 /usr/lib/foo.so 改成指向副本内部的相对路径。
                # 使用词法路径计算，不解析源端可能缺少目标的链接。
                relative = os.path.relpath(root / target.lstrip('/'), path.parent)
                path.unlink()
                path.symlink_to(relative)
                converted.append({
                    'path': str(path.relative_to(root)),
                    'original': target,
                    'relative': relative,
                })

    missing, escaping = [], []
    for directory, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            path = Path(directory) / name
            if not path.is_symlink():
                continue
            try:
                # 解析完整链接链，既检查绝对链接，也检查相对路径越界。
                if not path.resolve().is_relative_to(root):
                    escaping.append(str(path.relative_to(root)))
                if not path.exists():
                    missing.append(str(path.relative_to(root)))
            except (RuntimeError, OSError):
                missing.append(str(path.relative_to(root)))

    report = {
        'absolute_links_converted': converted,
        'dangling_or_cyclic_links': missing,
        'escaping_links': escaping,
    }
    report_path = root.parent / f'{root.name}-link-report.json'
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(f'转换绝对链接 {len(converted)} 个；悬空或循环链接 {len(missing)} 个；越界链接 {len(escaping)} 个')
    print(f'报告：{report_path}')

    # 部分 rootfs 快照允许缺少无关程序，但视频交叉编译所需文件必须存在。
    required = [
        'usr/include/stdio.h',
        'usr/include/gstreamer-1.0/gst/gst.h',
        'usr/include/gstreamer-1.0/gst/app/gstappsink.h',
        'usr/lib/aarch64-linux-gnu/glib-2.0/include/glibconfig.h',
        'usr/lib/aarch64-linux-gnu/crt1.o',
        'usr/lib/aarch64-linux-gnu/libc.so',
        'usr/lib/aarch64-linux-gnu/libgstreamer-1.0.so',
        'usr/lib/aarch64-linux-gnu/libgstapp-1.0.so',
        'lib/ld-linux-aarch64.so.1',
    ]
    for name in required:
        if not (root / name).is_file():
            raise SystemExit(f'错误：缺少关键文件 {name}')
    if escaping:
        raise SystemExit('错误：检测到指向 sysroot 外部的链接，请查看报告')
    print('PASS：关键头文件、库、加载器齐全，软链接未越界')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('sysroot', type=Path, help='本地 sysroot 副本目录')
    args = parser.parse_args()
    fix_links(args.sysroot.resolve())


if __name__ == '__main__':
    main()
