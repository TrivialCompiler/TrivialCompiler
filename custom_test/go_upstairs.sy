int n;
int result[1024] = {};

int main() {
	n = getint();
	result[1] = 1;
	result[2] = 1;
	int i = 3;
	while (i <= n) {
		result[i] = result[i - 1] + result[i - 2];
		i = i + 1;
	}
	putint(result[n]);
	putch(10);
	return 0;
}

