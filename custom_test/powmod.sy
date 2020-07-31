int naive_powmod(int a, int b, int n) {
    int i = 0;
    int res = 1;
    while (i < b) {
        res = (res * a) % n;
        i = i + 1;
    }
    return res;
}

int main() {
    starttime();
    putint(naive_powmod(2, 5, 3));
    putint(naive_powmod(2, 10000, 3));
    putint(naive_powmod(2, 1000000, 3));
    putint(naive_powmod(2, 100000000, 3));
    putint(naive_powmod(2, 1000000000, 3));
    stoptime();
    return 0;
}