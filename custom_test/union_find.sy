const int N = 1024;
int p[N];

int parent(int n) {
    if (p[n] == n) {
        return n;
    } else {
        p[n] = parent(p[n]);
        return p[n];
    }
}

void init() {
    int i = 0;
    while (i < N) {
        p[i] = i;
        i = i + 1;
    }
}

int main() {
    int n = getint();
    int i = 0;
    int a[N], b[N];
    starttime();
    while (i < n) {
        a[i] = getint();
        b[i] = getint();
        i = i + 1;
    }

    starttime();
    init();
    i = 0;
    while (i < n) {
        p[a[i]] = parent(b[i]);
        i = i + 1;
    }

    i = 0;
    while (i < n) {
        parent(i);
        i = i + 1;
    }
    stoptime();

    i = 0;
    while (i < n) {
        putint(p[i]);
        putch(32);
        i = i + 1;
    }
    return 0;
}