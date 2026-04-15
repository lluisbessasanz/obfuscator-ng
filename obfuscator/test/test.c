#include <stdio.h>
#include <stdlib.h>

int f(int a, int b) {
    int x = a + b;
    int y = x ^ a;
    int z = y - b;
    return z | 7;
}

int main(int argc, char **argv) {
    int a = argc > 1 ? atoi(argv[1]) : 37;
    int b = argc > 2 ? atoi(argv[2]) : 22;
    printf("%d\n", f(a, b));
    return 0;
}
