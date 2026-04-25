#include <stdio.h>
#include <stdlib.h>

static int flatten_target(int a, int b) {
    int x = a + b;
    int y = 0;

    if ((x & 1) == 0) {
        y = x ^ a;
    } else {
        y = x - b;
    }

    for (int i = 0; i < 3; ++i) {
        if ((y + i) % 2 == 0) {
            y += i + a;
        } else {
            y ^= (b + i);
        }
    }

    switch (y & 3) {
        case 0:
            y += 11;
            break;
        case 1:
            y -= 7;
            break;
        case 2:
            y ^= 0x55;
            break;
        default:
            y += a - b;
            break;
    }

    if (y > 100) {
        y = y / 2;
    } else if (y < 0) {
        y = -y;
    } else {
        y = y + 9;
    }

    return y;
}

int main(int argc, char **argv) {
    int a = argc > 1 ? atoi(argv[1]) : 37;
    int b = argc > 2 ? atoi(argv[2]) : 22;

    int r = flatten_target(a, b);
    printf("result=%d\n", r);
    return 0;
}