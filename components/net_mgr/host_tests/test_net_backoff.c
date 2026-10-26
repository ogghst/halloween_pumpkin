/* Host unit tests for the pure net_mgr backoff (Unity, native build).
 *
 * net_backoff_compute(attempt, rand_pct) contract checked against the
 * Kconfig default cap (CONFIG_PUMPKIN_NET_BACKOFF_MAX_MS = 60000 from the
 * stub sdkconfig.h):
 *
 * - attempt 0: base 1000 ms, jitter extremes exact (0 -> 850, 99 -> 1150)
 * - doubling: the envelope [85%, 115%] of the doubled base grows strictly
 *   per attempt while under the cap
 * - cap: every result is <= CONFIG_PUMPKIN_NET_BACKOFF_MAX_MS, including
 *   saturated attempts and rand_pct = 99 (+15%)
 * - determinism: identical (attempt, rand_pct) pairs give identical
 *   results; rand_pct is reduced modulo 100 by contract.
 */
#include <limits.h>
#include <string.h>

#include "unity.h"

#include "sdkconfig.h"
#include "net_mgr.h" /* public declaration of net_backoff_compute */

#define CAP_MS ((uint32_t)CONFIG_PUMPKIN_NET_BACKOFF_MAX_MS)

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_attempt_zero_envelope(void)
{
    TEST_ASSERT_EQUAL_UINT32(850u, net_backoff_compute(0, 0));
    TEST_ASSERT_EQUAL_UINT32(1000u, net_backoff_compute(0, 50));
    TEST_ASSERT_EQUAL_UINT32(1150u, net_backoff_compute(0, 99));
}

static void test_jitter_extremes_exact(void)
{
    /* attempt 1: base 2000 -> [1700, 2300] */
    TEST_ASSERT_EQUAL_UINT32(1700u, net_backoff_compute(1, 0));
    TEST_ASSERT_EQUAL_UINT32(2300u, net_backoff_compute(1, 99));
    /* attempt 5: base 32000, still under the cap */
    TEST_ASSERT_EQUAL_UINT32(27200u, net_backoff_compute(5, 0));
    TEST_ASSERT_EQUAL_UINT32(36800u, net_backoff_compute(5, 99));
}

static void test_doubling_at_mid_jitter(void)
{
    /* rand_pct = 50 maps to factor 100: the raw doubling is visible. */
    TEST_ASSERT_EQUAL_UINT32(1000u, net_backoff_compute(0, 50));
    TEST_ASSERT_EQUAL_UINT32(2000u, net_backoff_compute(1, 50));
    TEST_ASSERT_EQUAL_UINT32(4000u, net_backoff_compute(2, 50));
    TEST_ASSERT_EQUAL_UINT32(8000u, net_backoff_compute(3, 50));
    TEST_ASSERT_EQUAL_UINT32(16000u, net_backoff_compute(4, 50));
    TEST_ASSERT_EQUAL_UINT32(32000u, net_backoff_compute(5, 50));
}

static void test_envelope_grows_per_attempt(void)
{
    uint32_t prev_min = 0;
    uint32_t prev_max = 0;
    for (uint32_t attempt = 0; attempt <= 4; attempt++) {
        uint32_t lo = net_backoff_compute(attempt, 0);
        uint32_t hi = net_backoff_compute(attempt, 99);
        TEST_ASSERT_TRUE(lo < hi);
        TEST_ASSERT_TRUE(lo > prev_min);
        TEST_ASSERT_TRUE(hi > prev_max);
        prev_min = lo;
        prev_max = hi;
    }
}

static void test_cap_respected(void)
{
    const uint32_t attempts[] = { 5u, 6u, 7u, 12u, 20u, 1000u, UINT32_MAX };
    const uint32_t pcts[] = { 0u, 33u, 50u, 77u, 99u };
    for (size_t a = 0; a < sizeof(attempts) / sizeof(attempts[0]); a++) {
        for (size_t p = 0; p < sizeof(pcts) / sizeof(pcts[0]); p++) {
            TEST_ASSERT_LESS_OR_EQUAL_UINT32(
                CAP_MS, net_backoff_compute(attempts[a], pcts[p]));
        }
    }
}

static void test_cap_saturated_values(void)
{
    /* attempt 6 doubles past the cap: base becomes 60000, then jitter. */
    TEST_ASSERT_EQUAL_UINT32(51000u, net_backoff_compute(6, 0));
    TEST_ASSERT_EQUAL_UINT32(60000u, net_backoff_compute(6, 99));
    /* Saturated attempts never exceed the cap, whatever the jitter. */
    TEST_ASSERT_EQUAL_UINT32(51000u, net_backoff_compute(20, 0));
    TEST_ASSERT_EQUAL_UINT32(60000u, net_backoff_compute(20, 99));
    TEST_ASSERT_EQUAL_UINT32(60000u, net_backoff_compute(UINT32_MAX, 99));
}

static void test_deterministic(void)
{
    uint32_t first[100];
    uint32_t second[100];
    for (uint32_t i = 0; i < 100u; i++) {
        first[i] = net_backoff_compute(i % 8u, i);
    }
    for (uint32_t i = 0; i < 100u; i++) {
        second[i] = net_backoff_compute(i % 8u, i);
    }
    TEST_ASSERT_EQUAL_MEMORY(first, second, sizeof(first));
}

static void test_rand_pct_reduced_modulo_100(void)
{
    /* Contract domain is 0..99; out-of-domain inputs wrap deterministically. */
    TEST_ASSERT_EQUAL_UINT32(net_backoff_compute(2, 0),
                             net_backoff_compute(2, 100));
    TEST_ASSERT_EQUAL_UINT32(net_backoff_compute(3, 42),
                             net_backoff_compute(3, 142));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_attempt_zero_envelope);
    RUN_TEST(test_jitter_extremes_exact);
    RUN_TEST(test_doubling_at_mid_jitter);
    RUN_TEST(test_envelope_grows_per_attempt);
    RUN_TEST(test_cap_respected);
    RUN_TEST(test_cap_saturated_values);
    RUN_TEST(test_deterministic);
    RUN_TEST(test_rand_pct_reduced_modulo_100);
    return UNITY_END();
}
