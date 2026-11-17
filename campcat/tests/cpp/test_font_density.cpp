#include "app/ui/fonts.h"

#include <cmath>

#include <gtest/gtest.h>

TEST(font_density, below_one_and_a_half_is_one) {
    EXPECT_EQ(campcat::app::font_density(1.0F), 1.0F);
    EXPECT_EQ(campcat::app::font_density(1.49F), 1.0F);
}

TEST(font_density, retina_is_two) {
    EXPECT_EQ(campcat::app::font_density(1.5F), 2.0F);
    EXPECT_EQ(campcat::app::font_density(2.0F), 2.0F);
    EXPECT_EQ(campcat::app::font_density(3.0F), 2.0F);
}

TEST(font_density, nan_is_one) {
    EXPECT_EQ(campcat::app::font_density(std::nanf("")), 1.0F);
}
