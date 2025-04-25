#include <cstdio>

int main(int argc, char **argv) {
	if (argc < 2) {
		printf("no arguments\n");
		return 0;
	}
	for (int i=1; i<argc; i++) {
		printf("[%d] %s\n", i, argv[i]);
	}
	printf("total: %d\n", argc-1);
	return 0;
}