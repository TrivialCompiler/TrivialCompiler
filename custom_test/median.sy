int median(int arr[], int N, int i, int j) {
    if (i == N / 2 || i == (N + 1) / 2) {
        return i;
    }

    if (j == N / 2 || j == (N + 1) / 2) {
        return j;
    }

    int middle = i;
    int temp = arr[i];

    int k = i + 1;

    while (k <= j) {
        if (arr[k] < temp) {
            if (k == middle + 1) {
                arr[middle] = arr[k];
                arr[middle + 1] = temp;
                middle = middle + 1;
                k = k + 1;
                continue;
            }
            arr[middle] = arr[k];
            arr[k] = arr[middle + 1];
            arr[middle + 1] = temp;
            middle = middle + 1;
            k = k + 1;
            continue;
        }
        k = k + 1;
    }

    if (middle == (N - 1) / 2 || middle == N / 2) {
        return middle;
    }
    if (middle > (N + 1) / 2) {
        return median(arr, N, i, middle - 1);
    } else {
        return median(arr, N, middle + 1, j);
    }
}

int arr[100005];

int main() {
    int N = getarray(arr);
    starttime();
    int med = median(arr, N, 0, N - 1);
    stoptime();
    putint(med);
    putch(10);
    return 0;
}
