/* Test host rate-limit distance e ciclo periodico. */
#include <string.h>
#include "unity.h"
#include "ha_state.h"

static ha_report_state_t s_st;

void setUp(void)
{
    memset(&s_st, 0, sizeof s_st);
}

void tearDown(void)
{
}

static void test_null_state(void)
{
    TEST_ASSERT_FALSE(ha_rate_distance(NULL, 1000, 0));
    TEST_ASSERT_FALSE(ha_rate_periodic(NULL, 0));
}

static void test_first_sample_no_publish(void)
{
    TEST_ASSERT_FALSE(ha_rate_distance(&s_st, 1000, 0));
    TEST_ASSERT_TRUE(s_st.distance_seen);
    TEST_ASSERT_EQUAL_INT32(1000, s_st.last_distance_mm);
}

static void test_delta_and_interval(void)
{
    /* primo campione a t=0 */
    TEST_ASSERT_FALSE(ha_rate_distance(&s_st, 1000, 0));
    /* delta 30 mm dopo 2000 ms -> sotto soglia -> false */
    TEST_ASSERT_FALSE(ha_rate_distance(&s_st, 1030, 2000));
    /* delta 51 mm dopo altri 2000 ms (t=4000) -> true e pubblica */
    TEST_ASSERT_TRUE(ha_rate_distance(&s_st, 1051, 4000));
    TEST_ASSERT_EQUAL_INT32(1051, s_st.last_distance_mm);
    /* delta 200 dopo 200 ms dal publish (t=4200) -> intervallo insufficiente */
    TEST_ASSERT_FALSE(ha_rate_distance(&s_st, 1251, 4200));
    /* delta 10 dopo altri 900 ms (t=5100, tot >= 1000) -> delta insufficiente */
    TEST_ASSERT_FALSE(ha_rate_distance(&s_st, 1061, 5100));
    /* delta 200 dopo 1000+ ms (t=6100) -> true */
    TEST_ASSERT_TRUE(ha_rate_distance(&s_st, 1251, 6100));
    TEST_ASSERT_EQUAL_INT32(1251, s_st.last_distance_mm);
}

static void test_compare_against_last_published(void)
{
    /* publish iniziale a 1000 mm */
    TEST_ASSERT_FALSE(ha_rate_distance(&s_st, 1000, 0));
    /* 1040 (delta 40 <= 50) -> false */
    TEST_ASSERT_FALSE(ha_rate_distance(&s_st, 1040, 200));
    /* 1051 prima dei 1000 ms -> false */
    TEST_ASSERT_FALSE(ha_rate_distance(&s_st, 1051, 500));
    /* 1051 dopo i 1000 ms -> true (delta ancora 51 da 1000, ultimo pubblicato) */
    TEST_ASSERT_TRUE(ha_rate_distance(&s_st, 1051, 1500));
    TEST_ASSERT_EQUAL_INT32(1051, s_st.last_distance_mm);
}

static void test_periodic(void)
{
    /* prima chiamata -> true */
    TEST_ASSERT_TRUE(ha_rate_periodic(&s_st, 0));
    /* dopo 30 s -> false */
    TEST_ASSERT_FALSE(ha_rate_periodic(&s_st, 30000));
    /* dopo altri 31 s (61 s) -> true */
    TEST_ASSERT_TRUE(ha_rate_periodic(&s_st, 61000));
    /* subito dopo -> false */
    TEST_ASSERT_FALSE(ha_rate_periodic(&s_st, 62000));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_null_state);
    RUN_TEST(test_first_sample_no_publish);
    RUN_TEST(test_delta_and_interval);
    RUN_TEST(test_compare_against_last_published);
    RUN_TEST(test_periodic);
    return UNITY_END();
}
