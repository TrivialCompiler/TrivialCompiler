const int N = 1024;
void conv1d(int n, int A[], int B[], int C[]) {
    int i = 0;

    i = 0;
    while (i < n) {
        int j = 0;
        C[i] = 0;
        while (j <= i) {
            C[i] = C[i] + A[j] * B[i-j];
            j = j + 1;
        }
        i = i + 1;
    }
}

int A[N];
int B[N];
int C[N];

int main() {
    int n = getint();
    int i, j;

    i = 0;
    while (i < n){
        A[i] = getint();
        i = i + 1;
    }

    i = 0;
    while (i < n){
        B[i] = getint();
        i = i + 1;
    }

    starttime();
    i = 0;
    while (i < 1000) {
        conv1d(n, A, B, C);
        conv1d(n, B, C, A);
        i = i + 1;
    }
    int ans = 0;
    i = 0;
    while (i < n){
        ans = ans + A[i];
        i = i + 1;
    }
    stoptime();
    putint(ans);
    putch(10);
    return 0;
}