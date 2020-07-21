#!/usr/bin/env python3

import sys
import re
from tabulate import tabulate

if __name__ == '__main__':
    with open(sys.argv[1]) as f:
        content = f.read()

    pattern = re.compile(r'Test: check_run_(llvm|tc)_(.*)[\s\S]*?TOTAL: (\d+)H-(\d+)M-(\d+)S-(\d+)us')

    results = {}

    try:
        for compiler, name, h, m, s, us in pattern.findall(content):
            h, m, s, us = int(h), int(m), int(s), int(us)
            if name not in results:
                results[name] = {}
            results[name][compiler] = ((h * 60 + m) * 60 + s) * 1e6 + us
    except:
        pass

    data = []

    for case, time in results.items():
        time_llvm = time['llvm'] if 'llvm' in time else float('nan')
        time_tc = time['tc'] if 'tc' in time else float('nan')
        ratio = f'+{time_tc * 100 / time_llvm:0.2f}%' if time_llvm != 0 else 'nan'
        data.append([case, time_llvm, time_tc, ratio])

    print(tabulate(data, headers=["Case", "LLVM", "TC", "Ratio"]))
