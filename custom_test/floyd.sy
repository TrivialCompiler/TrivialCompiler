const int N = 1024;
int dist[N][N];

int main() {
    int n = getint();
    int i, j;
    i = 0;
    while (i < n) {
        j = 0;
        while (j < n) {
            dist[i][j] = getint();
            j = j + 1;
        }
        i = i + 1;
    }

    starttime();

    int k = 0;
    while (k < n) {
        i = 0;
        while (i < n) {
            j = 0;
            while (j < n) {
                if (dist[i][k] + dist[k][j] < dist[i][j]) {
                    dist[i][j] = dist[i][k] + dist[k][j];
                }
                j = j + 1;
            }
            i = i + 1;
        }
        k = k + 1;
    }


    stoptime();
    i = 0;
    while (i < n) {
        j = 0;
        while (j < n) {
            putint(dist[i][j]);
            putch(32);
            j = j + 1;
        }
        i = i + 1;
    }
}