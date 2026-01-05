#include <gtest/gtest.h>
#include "../daemon/client.h"

class ClientModeTest : public ::testing::Test {
protected:
    ModeContext* ctx;

    void SetUp() override {
        ctx = lm_alloc();
        ASSERT_NE(ctx, nullptr);
    }

    void TearDown() override {
        if (ctx) {
            lm_destroy(ctx);
        }
    }
};

TEST_F(ClientModeTest, InitialState) {
    // Initial state should be LIST_NORMAL
    // lm_get_filter_text returns NULL if filter is empty in LIST_NORMAL
    char* filter = lm_get_filter_text(ctx);
    EXPECT_EQ(filter, nullptr);
}

TEST_F(ClientModeTest, FilterLifecycle) {
    LmModeTransition tx;
    LmMode old_mode, new_mode;

    // 1. Start Filtering (/)
    lm_process_key_event(ctx, 44, '/', 0, 0, &tx, &old_mode, &new_mode);
    EXPECT_EQ(tx, LM_MODETS_ADHERE_TO_MODE);
    EXPECT_EQ(new_mode, LM_MODE_LIST_FILTER_INFLIGHT);

    // 2. Type "a"
    lm_process_key_event(ctx, 0, 'a', 0, 0, &tx, &old_mode, &new_mode);
    EXPECT_EQ(tx, LM_MODETS_UPDATE_LIST_FILTER);
    // In INFLIGHT, get_filter_text should return the buffer
    char* f_inflight = lm_get_filter_text(ctx);
    ASSERT_NE(f_inflight, nullptr);
    EXPECT_STREQ(f_inflight, "a");

    // Type "b"
    lm_process_key_event(ctx, 11, 'b', 0, 0, &tx, &old_mode, &new_mode);
    f_inflight = lm_get_filter_text(ctx);
    ASSERT_NE(f_inflight, nullptr);
    EXPECT_STREQ(f_inflight, "ab");

    // 3. Commit (Enter) -> LIST_NORMAL
    lm_process_key_event(ctx, 36, 0, 0, 0, &tx, &old_mode, &new_mode);
    EXPECT_EQ(new_mode, LM_MODE_LIST_NORMAL);

    // Filter text should persist in LIST_NORMAL
    char* f = lm_get_filter_text(ctx);
    ASSERT_NE(f, nullptr);
    EXPECT_STREQ(f, "ab");

    // 4. Esc -> Clear Filter
    lm_process_key_event(ctx, 53, 0, 0, 0, &tx, &old_mode, &new_mode);
    EXPECT_EQ(tx, LM_MODETS_UPDATE_LIST_FILTER);
    EXPECT_EQ(new_mode, LM_MODE_LIST_NORMAL);

    f = lm_get_filter_text(ctx);
    EXPECT_EQ(f, nullptr);

    // 5. Esc again -> Hide UI
    lm_process_key_event(ctx, 53, 0, 0, 0, &tx, &old_mode, &new_mode);
    EXPECT_EQ(tx, LM_MODETS_HIDE_UI);
}

TEST_F(ClientModeTest, MultiselectFilterPersistence) {
    LmModeTransition tx;
    LmMode old_mode, new_mode;
    char* f;

    // 1. Create a filter "xyz"
    lm_process_key_event(ctx, 44, '/', 0, 0, &tx, &old_mode, &new_mode);
    lm_process_key_event(ctx, 0, 'x', 0, 0, &tx, &old_mode, &new_mode);
    lm_process_key_event(ctx, 0, 'y', 0, 0, &tx, &old_mode, &new_mode);
    lm_process_key_event(ctx, 0, 'z', 0, 0, &tx, &old_mode, &new_mode);
    lm_process_key_event(ctx, 36, 0, 0, 0, &tx, &old_mode, &new_mode); // Enter -> LIST_NORMAL

    f = lm_get_filter_text(ctx);
    ASSERT_NE(f, nullptr);
    EXPECT_STREQ(f, "xyz");

    // 2. Transition to MULTISELECT (Cmd+A)
    lm_process_key_event(ctx, 0, 0, 1 /*cmd*/, 0, &tx, &old_mode, &new_mode);
    EXPECT_EQ(new_mode, LM_MODE_LIST_MULTISELECT);

    // Filter should still be visible/active in Multiselect
    f = lm_get_filter_text(ctx);
    ASSERT_NE(f, nullptr);
    EXPECT_STREQ(f, "xyz");

    // 3. Escape MULTISELECT -> LIST_NORMAL
    lm_process_key_event(ctx, 53, 0, 0, 0, &tx, &old_mode, &new_mode);
    EXPECT_EQ(tx, LM_MODETS_ADHERE_TO_MODE); // Just mode switch, selection clear implied
    EXPECT_EQ(new_mode, LM_MODE_LIST_NORMAL);

    // Filter should persist back in LIST_NORMAL
    f = lm_get_filter_text(ctx);
    ASSERT_NE(f, nullptr);
    EXPECT_STREQ(f, "xyz");

    // 4. Escape again -> Clear Filter
    lm_process_key_event(ctx, 53, 0, 0, 0, &tx, &old_mode, &new_mode);
    EXPECT_EQ(tx, LM_MODETS_UPDATE_LIST_FILTER);
    f = lm_get_filter_text(ctx);
    EXPECT_EQ(f, nullptr);

    // 5. Escape again -> Hide UI
    lm_process_key_event(ctx, 53, 0, 0, 0, &tx, &old_mode, &new_mode);
    EXPECT_EQ(tx, LM_MODETS_HIDE_UI);
}

TEST_F(ClientModeTest, NewSearchClearsOld) {
    LmModeTransition tx;
    LmMode old_mode, new_mode;
    char* f;

    // 1. Set filter "abc"
    lm_process_key_event(ctx, 44, '/', 0, 0, &tx, &old_mode, &new_mode);
    lm_process_key_event(ctx, 0, 'a', 0, 0, &tx, &old_mode, &new_mode);
    lm_process_key_event(ctx, 0, 'b', 0, 0, &tx, &old_mode, &new_mode);
    lm_process_key_event(ctx, 0, 'c', 0, 0, &tx, &old_mode, &new_mode);
    lm_process_key_event(ctx, 36, 0, 0, 0, &tx, &old_mode, &new_mode); // Enter

    f = lm_get_filter_text(ctx);
    ASSERT_NE(f, nullptr);
    EXPECT_STREQ(f, "abc");

    // 2. Start NEW search (/)
    lm_process_key_event(ctx, 44, '/', 0, 0, &tx, &old_mode, &new_mode);
    EXPECT_EQ(new_mode, LM_MODE_LIST_FILTER_INFLIGHT);

    // Initial inflight buffer should be empty
    f = lm_get_filter_text(ctx);
    EXPECT_EQ(f, nullptr);

    // 3. Type "d"
    lm_process_key_event(ctx, 0, 'd', 0, 0, &tx, &old_mode, &new_mode);
    f = lm_get_filter_text(ctx);
    ASSERT_NE(f, nullptr);
    EXPECT_STREQ(f, "d"); // Should be "d", NOT "abcd"
}
