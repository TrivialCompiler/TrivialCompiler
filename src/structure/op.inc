// op.hpp, ast.hpp, ir.hpp都include了这个文件用来定义op(并且都是在enum的最前面)，这样保证了定义的一致性
// Add和Or被用于定义classof，如果修改了上界/下界，也要修改classof的定义
Add, Sub, Rsb, Mul, Div, Mod, Lt, Le, Ge, Gt, Eq, Ne, And, Or,