const int N = 1024;
void transpose(int n, int A[][N], int B[][N]) {
    int i = 0;

    i = 0;
    while (i < n) {
        int j = 0;
        while (j < n) {
            B[i][j] = A[j][i];
            j = j + 1;
        }
        i = i + 1;
    }
}

int A[N][N];
int B[N][N];

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

    starttime();
    i = 0;
    while (i < 500) {
        transpose(n, A, B);
        transpose(n, B, A);
        i = i + 1;
    }
    int ans = 0;
    i = 0;
    while (i < n){
        j = 0;
        while (j < n){
            ans = ans + B[i][j];
            j = j + 1;
        }
        i = i + 1;
    }
    stoptime();
    putint(ans);
    putch(10);
    return 0;
}