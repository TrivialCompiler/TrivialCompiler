#!/usr/bin/env python3

import sys
import re
from tabulate import tabulate

if __name__ == '__main__':
    with open(sys.argv[1]) as f:
        content = f.read()

    pattern = re.compile(r'Test: check_run_(llvm|tc|gcc|clang)_(.*)[\s\S]*?TOTAL: (\d+)H-(\d+)M-(\d+)S-(\d+)us')

    results = {}

    # split by test cases
    for case in content.split('----------------------------------------------------------\n\n'):
        try:
            for compiler, name, h, m, s, us in pattern.findall(case):
                h, m, s, us = int(h), int(m), int(s), int(us)
                if name not in results:
                    results[name] = {}
                results[name][compiler] = ((h * 60 + m) * 60 + s) * 1e6 + us
        except:
            continue

    data = []

    for case, time in results.items():
        nan = float('nan')
        time_llvm = time['llvm'] if 'llvm' in time else nan
        time_gcc = time['gcc'] if 'gcc' in time else nan
        time_clang = time['clang'] if 'clang' in time else nan
        time_tc = time['tc'] if 'tc' in time else nan
        ratio_llvm = f'{time_tc * 100 / time_llvm:0.2f}%' if time_llvm != 0 else 'N/A'
        ratio_gcc = f'{time_tc * 100 / time_gcc:0.2f}%' if time_gcc != 0 else 'N/A'
        ratio_clang = f'{time_tc * 100 / time_clang:0.2f}%' if time_clang != 0 else 'N/A'
        data.append([case, time_llvm / 1e6, time_gcc / 1e6, time_clang / 1e6, time_tc / 1e6, ratio_llvm, ratio_gcc, ratio_clang])

    print(tabulate(data, headers=["Case", "LLVM", "GCC", "Clang", "TC", "Ratio(LLVM)", "Ratio(GCC)", "Ratio(Clang)"]))
