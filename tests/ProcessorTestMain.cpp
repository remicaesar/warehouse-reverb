/*
    Runner for the WarehouseProcessorTests target. CTO-owned; see tests/TestSuites.h for the contract.
*/

#include "TestSuites.h"

#include <cstdio>

int main (int, char**)
{
    int total = 0;

    total += runProcessorTests();
    total += runStateCompatTests();

    std::printf ("\n==================================================\n");
    std::printf ("%s - %d failure%s across all processor suites\n",
                 total == 0 ? "ALL SUITES PASSED" : "SUITE FAILURES",
                 total, total == 1 ? "" : "s");
    std::printf ("==================================================\n");

    return total == 0 ? 0 : 1;
}
