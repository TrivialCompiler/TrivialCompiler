import os

os.system('parser_gen parser.toml --lang cpp -o parser.cpp')
parser = open('./parser.cpp').read()
os.remove('./parser.cpp')

# 目前一个适合.hpp和.cpp分割的特征是连续三个换行符，如果修改了include，比如在最后加了一个换行，可能就变了
sep = parser.find('\n\n\n')
open('src/generated/parser.hpp', 'w').write('#pragma once\n\n' + parser[:sep])
open('src/generated/parser.cpp', 'w').write('#include "parser.hpp"\n\n' + parser[sep + 3:])
