// has side effect
void f1() {
  putint(1); putch(10);
}

int glob;

// no side effect, load glob
int f2(int x[]) {
  return x[0] + glob;
}

int f3(int x[]) {
  return x[0] + x[1];
}

int main() {
  f1();
  int i = 0;
  int x[2] = {getint(), getint()};
  f2(x);
  while (i < f3(x)) {
    i = i + f2(x);
  }
  putint(i); putch(10);
  return 0;
}