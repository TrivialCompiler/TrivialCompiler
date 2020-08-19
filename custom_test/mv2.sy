const int N = 1024;
void mv(int n, int A[][N], int B[], int C[]) {
    int i = 0;
    while (i < n) {
        C[i] = 0;
        i = i + 1;
    }

    i = 0;
    while (i < n) {
        int j = 0;
        while (j < n) {
            C[i] = C[i] + A[i][j] * B[j];
            j = j + 1;
        }
        i = i + 1;
    }
}

int A[N][N];
int B[N];
int C[N];

int main() {
    int n = getint();
    int i, j;

    i = 0;
    j = 0;
    while (i < n){
        j = 0;
        while (j < n){
            A[i][j] = getint();
            j = j + 1;
        }
        i = i + 1;
    }

    i = 0;
    while (i < n){
        B[i] = getint();
        i = i + 1;
    }

    starttime();
    i = 0;
    while (i < 50) {
        mv(n, A, B, C);
        mv(n, A, C, B);
        i = i + 1;
    }
    int ans = 0;
    i = 0;
    while (i < n){
        ans = ans + B[i];
        i = i + 1;
    }
    stoptime();
    putint(ans);
    putch(10);
    return 0;
}