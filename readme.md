生成的parser已经在`src`中，所以直接编译即可。当前的`src/main.cpp`只是简单地读入文件并parse，如果成功则只是提示成功，如果失败则输出失败位置的token。

# 生成parser

安装`parser_gen`：

```
cargo install --git https://github.com/MashPlant/lalr1 --features="clap toml"
```

现在`parser_gen`功能还有一点限制：不能生成.hpp和.cpp两个文件，所以需要手动分割一下。现在我是用`gen.py`来分割生成的文件，从而得到`src/parser.[hpp|cpp]`。`gen.py`里会调用`parser_gen`，所以每次修改了`parset.toml`后直接执行`python gen.py`就可以了。

现在lexer部分还有一点小问题：因为我输出的表依赖于散列表遍历的顺序，所以每次输出都可能不一样，虽然不会影响结果，但是在版本管理上不是很好看，好像每次都修改了这个文件一样。所以如果重新生成了`src/parser.cpp`但没有修改lexer的话，最好在commit前把`src/parser.cpp`中的`Lexer::next`中的那几个表改回原样。