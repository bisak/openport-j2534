/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test.h"
int g_tests, g_fails;

int main(void)
{
    test_proto();
    test_frames();
    test_j2534();
    test_golden();
    printf("\n%d checks, %d failures\n", g_tests, g_fails);
    return g_fails == 0 ? 0 : 1;
}
