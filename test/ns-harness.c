#include "ns-harness.h"

#include <math.h>
#include <stdio.h>

int g_run = 0;
int g_failed = 0;

bool feq(float a, float b)   { return fabsf(a - b) < 1e-5f; }
bool deq(double a, double b) { return fabs(a - b) < 1e-9; }

int test_summary(void) {
    if (g_failed == 0) {
        printf("OK: %d checks\n", g_run);
        return 0;
    }

    printf("FAILED: %d/%d checks\n", g_failed, g_run);
    return 1;
}
