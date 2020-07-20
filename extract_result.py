#!/usr/bin/env python3

import sys
import re

if __name__ == '__main__':
    with open(sys.argv[1]) as f:
        content = f.read()

    pattern = re.compile(r'Test: check_run_llvm_(.*)[\s\S]*?TOTAL: (.*)')

    try:
        for name, time in pattern.findall(content):
            print(f'{name}: {time}')
    except:
        pass

