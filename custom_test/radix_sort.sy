// 你可以用这个生成数据
// import numpy as np
//
// print(2000000)
// for x in np.random.choice(2000000, 2000000):
//     print(x, end=' ')

int n;
int aux[2000000];


void radix_sort(int a[], int n) {
  const int U = 65536;
  int i;
  // 不行, segment fault
  // int cnt[65536] = {};
  // 可以
  int cnt[65536];

  i = 0;
  while (i < U) {
    cnt[i] = 0;
    i = i + 1;
  }

  i = 0;
  while (i < n) {
    cnt[a[i] % U] = cnt[a[i] % U] + 1;
    i = i + 1;
  }
  i = 1;
  while (i < U) {
    cnt[i] = cnt[i] + cnt[i - 1];
    i = i + 1;
  }
  i = n - 1;
  while (i >= 0) {
    cnt[a[i] % U] = cnt[a[i] % U] - 1;
    aux[cnt[a[i] % U]] = a[i];
    i = i - 1;
  }

  i = 0;
  while (i < U) {
    cnt[i] = 0;
    i = i + 1;
  }

  i = 0;
  while (i < n) {
    cnt[aux[i] / U % U] = cnt[aux[i] / U % U] + 1;
    i = i + 1;
  }
  i = 1;
  while (i < U) {
    cnt[i] = cnt[i] + cnt[i - 1];
    i = i + 1;
  }
  i = n - 1;
  while (i >= 0) {
    cnt[aux[i] / U % U] = cnt[aux[i] / U % U] - 1;
    a[cnt[aux[i] / U % U]] = aux[i];
    i = i - 1;
  }
}

int main(){
  starttime();
  int a[2000000];
  n = getarray(a);
  radix_sort(a, n);
  putarray(n, a);
  stoptime();
  return 0;
}