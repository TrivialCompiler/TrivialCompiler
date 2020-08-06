void f1(int x[]) {
  int a[3] = {};
  int i = x[0];
  while (i < x[1]) {
    a[i] = a[i] + 2 * i; i = i + 1;
  }
  i = x[0];
  while (i < x[1]) {
    a[i] = a[i] - i; i = i + 1;
  }
  putarray(3, a);
}

void f2(int x[]) {
  int a[3] = {};
  int i = x[0];
  while (i <= x[1] - 1) {
    a[i] = a[i] + 2 * i; i = i + 1;
  }
  i = x[0];
  while (i <= x[1] - 1) {
    a[i] = a[i] - i; i = i + 1;
  }
  putarray(3, a);
}

void f3(int x[]) {
  int a[3] = {};
  int i = x[0];
  while (x[1] > i) {
    a[i] = a[i] + 2 * i; i = i + 1;
  }
  i = x[0];
  while (x[1] > i) {
    a[i] = a[i] - i; i = i + 1;
  }
  putarray(3, a);
}

void f4(int x[]) {
  int a[3] = {};
  int i = x[0];
  while (x[1] - 1 >= i) {
    a[i] = a[i] + 2 * i; i = i + 1;
  }
  i = x[0];
  while (x[1] - 1 >= i) {
    a[i] = a[i] - i; i = i + 1;
  }
  putarray(3, a);
}

void f5(int x[]) {
  int a[3] = {};
  int i = x[1] - 1;
  while (i >= x[0]) {
    a[i] = a[i] + 2 * i; i = i - 1;
  }
  i = x[1] - 1;
  while (i >= x[0]) {
    a[i] = a[i] - i; i = i - 1;
  }
  putarray(3, a);
}

void f6(int x[]) {
  int a[3] = {};
  int i = x[1] - 1;
  while (i > x[0] - 1) {
    a[i] = a[i] + 2 * i; i = i - 1;
  }
  i = x[1] - 1;
  while (i > x[0] - 1) {
    a[i] = a[i] - i; i = i - 1;
  }
  putarray(3, a);
}

void f7(int x[]) {
  int a[3] = {};
  int i = x[1] - 1;
  while (x[0] <= i) {
    a[i] = a[i] + 2 * i; i = i - 1;
  }
  i = x[1] - 1;
  while (x[0] <= i) {
    a[i] = a[i] - i; i = i - 1;
  }
  putarray(3, a);
}

void f8(int x[]) {
  int a[3] = {};
  int i = x[1] - 1;
  while (x[0] - 1 < i) {
    a[i] = a[i] + 2 * i; i = i - 1;
  }
  i = x[1] - 1;
  while (x[0] - 1 < i) {
    a[i] = a[i] - i; i = i - 1;
  }
  putarray(3, a);
}

int main() {
  int x[2];
  x[0] = 0;
  while (x[0] <= 3) {
    x[1] = x[0];
    while (x[1] <= 3) {
      f1(x);
      f2(x);
      f3(x);
      f4(x);
      f5(x);
      f6(x);
      f7(x);
      f8(x);
      x[1] = x[1] + 1;
    }
    x[0] = x[0] + 1;
  }
  return 0;
}