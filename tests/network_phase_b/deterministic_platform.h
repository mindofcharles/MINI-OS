#ifndef MINI_OS_PHASE_B_DETERMINISTIC_PLATFORM_H
#define MINI_OS_PHASE_B_DETERMINISTIC_PLATFORM_H

void phase_b_test_clock_set(unsigned int milliseconds);
void phase_b_test_clock_advance(unsigned int milliseconds);
const char *phase_b_test_platform_marker(void);

#endif
