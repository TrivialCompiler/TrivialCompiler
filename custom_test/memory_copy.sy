void my_memcpy(int dst[], int src[], int n) {
    int i = 0;
    while (i < n) {
        dst[i] = src[i];
        i = i + 1;
    }
}

const int N = 1000000;
int temp1[N];
int temp2[N];

int main() {
    int i = 0;
    while (i < N) {
        temp1[i] = i;
        i = i + 1;
    }
    my_memcpy(temp2, temp1, N);

    i = 0;
    int j = 0;
    while (i < N) {
        if (temp2[i] > j) {
            j = temp2[i];
        }
        i = i + 1;
    }
    putint(j);
    return 0;
}