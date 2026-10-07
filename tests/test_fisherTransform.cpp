// Unit tests for SetAnalytics::fisherTransform, the atanh-style series
// transform that stretches values in (-1, 1) toward +/- infinity.

#include "tests/testFramework.h"

#include "src/analytics/SetAnalytics.h"

#include <stdexcept>

// atanh(x) written out the way the implementation does it, so a test failure
// points at the loop/bounds logic rather than at a hand-typed constant
static double expectedFisher(double x) { return 0.5 * std::log((1.0 + x) / (1.0 - x)); }

// runs the transform and reports whether it threw, so the throw cases read as
// plain assertions instead of a try/catch per test
static bool threw(std::vector<double> series, double lo = -1.0, double hi = 1.0) {
    SetAnalytics sa(series);
    try {
        sa.fisherTransform(lo, hi);
        return false;
    } catch (const std::out_of_range&) {
        return true;
    }
}

// ------------------------------------------------------------ happy path

TEST(transformsEveryValueInOrder) {
    std::vector<double> series = {-0.9, -0.5, -0.1, 0.0, 0.1, 0.5, 0.9};
    SetAnalytics sa(series);
    auto out = sa.fisherTransform();

    REQUIRE(out.size() == series.size());
    for (std::size_t i = 0; i < series.size(); i++) {
        CHECK_NEAR(out[i], expectedFisher(series[i]), 1e-12);
    }
}

TEST(zeroMapsToZero) {
    SetAnalytics sa({0.0});
    auto out = sa.fisherTransform();

    REQUIRE(out.size() == (std::size_t)1);
    CHECK_NEAR(out[0], 0.0, 1e-12);
}

TEST(isOddSymmetric) {
    SetAnalytics sa({0.25, -0.25, 0.75, -0.75});
    auto out = sa.fisherTransform();

    REQUIRE(out.size() == (std::size_t)4);
    CHECK_NEAR(out[0], -out[1], 1e-12);
    CHECK_NEAR(out[2], -out[3], 1e-12);
}

// the whole point of the transform: it's monotonic and it amplifies, so tails
// stretch further than the middle does
TEST(isMonotonicAndAmplifiesTails) {
    SetAnalytics sa({-0.99, -0.5, 0.0, 0.5, 0.99});
    auto out = sa.fisherTransform();

    REQUIRE(out.size() == (std::size_t)5);
    for (std::size_t i = 1; i < out.size(); i++) CHECK(out[i] > out[i - 1]);

    // |fisher(x)| > |x| everywhere except 0
    CHECK(std::fabs(out[0]) > 0.99);
    CHECK(std::fabs(out[4]) > 0.99);
    CHECK(std::fabs(out[4]) > std::fabs(out[3]) * 2.0); // 0.99 stretches far past 0.5
}

TEST(knownValues) {
    SetAnalytics sa({0.5, -0.5, 0.9});
    auto out = sa.fisherTransform();

    REQUIRE(out.size() == (std::size_t)3);
    CHECK_NEAR(out[0],  0.5493061443340549, 1e-12);
    CHECK_NEAR(out[1], -0.5493061443340549, 1e-12);
    CHECK_NEAR(out[2],  1.4722194895832204, 1e-12);
}

TEST(emptySeriesYieldsEmptyResult) {
    SetAnalytics sa({});
    auto out = sa.fisherTransform();
    CHECK_EQ(out.size(), (std::size_t)0);
}

// no tail-slice / warmup convention here unlike the rolling functions, every
// input point produces an output point no matter how short the series is
TEST(doesNotSliceOrWarmUp) {
    SetAnalytics sa({0.1, 0.2, 0.3});
    auto out = sa.fisherTransform();
    CHECK_EQ(out.size(), (std::size_t)3);
    CHECK_NEAR(out[0], expectedFisher(0.1), 1e-12);
}

TEST(picksUpValuesAddedByUpdateVector) {
    SetAnalytics sa({0.1});
    sa.updateVector(0.2);
    sa.updateVector(0.3);

    auto out = sa.fisherTransform();
    REQUIRE(out.size() == (std::size_t)3);
    CHECK_NEAR(out[2], expectedFisher(0.3), 1e-12);
}

// ------------------------------------------------------------ bounds

TEST(throwsOnValueAtOrOutsideDefaultBounds) {
    CHECK(threw({0.5, 1.0}));    // +1 is a pole, not a valid input
    CHECK(threw({-1.0, 0.5}));   // and so is -1
    CHECK(threw({0.5, 1.5}));
    CHECK(threw({-2.0}));
}

TEST(acceptsValuesJustInsideDefaultBounds) {
    CHECK(!threw({0.999999, -0.999999}));
}

TEST(customBoundsTightenTheAcceptedRange) {
    CHECK(threw({0.5}, -0.4, 0.4));   // fine by default, rejected at +/-0.4
    CHECK(!threw({0.3}, -0.4, 0.4));
    CHECK(threw({0.4}, -0.4, 0.4));   // bounds are exclusive on both ends
    CHECK(threw({-0.4}, -0.4, 0.4));
}

// the throw is all-or-nothing from the caller's side: a bad value anywhere in
// the series means no result comes back, not a partial one
TEST(throwsEvenWhenTheBadValueIsLast) {
    CHECK(threw({0.1, 0.2, 0.3, 5.0}));
}

// ------------------------------------------------------ arbitrary bounds
//
// the bounds no longer just gate the input, they define the interval that gets
// mapped onto (-inf, +inf). the poles are the bounds themselves rather than a
// hardcoded +/-1, so a series on any scale can go through

// the case that used to come back NaN: 2.0 is a perfectly ordinary point
// inside (-10, 10) and now transforms like one
TEST(wideBoundsRescaleRatherThanProducingNaN) {
    SetAnalytics sa({2.0});
    auto out = sa.fisherTransform(-10.0, 10.0);

    REQUIRE(out.size() == (std::size_t)1);
    CHECK(std::isfinite(out[0]));
    CHECK_NEAR(out[0], 0.5 * std::log(12.0 / 8.0), 1e-12);
}

// 1.0 used to be a pole no matter what bounds you passed, now it is just
// another interior point when the bounds are wider than the unit interval
TEST(theUnitIntervalIsNoLongerSpecial) {
    SetAnalytics sa({1.0, -1.0});
    auto out = sa.fisherTransform(-10.0, 10.0);

    REQUIRE(out.size() == (std::size_t)2);
    CHECK(std::isfinite(out[0]));
    CHECK(std::isfinite(out[1]));
    CHECK_NEAR(out[0], -out[1], 1e-12); // still odd about the midpoint
}

// scaling the bounds and scaling the data by the same factor is a no-op, which
// is the property that says the rescaling is actually a rescaling
TEST(symmetricBoundsMatchTheDefaultAfterScaling) {
    const double k = 7.5;
    std::vector<double> series = {-0.9, -0.3, 0.0, 0.3, 0.9};
    std::vector<double> scaled;
    for (double v : series) scaled.push_back(v * k);

    SetAnalytics plain(series);
    SetAnalytics wide(scaled);
    auto a = plain.fisherTransform();
    auto b = wide.fisherTransform(-k, k);

    REQUIRE(a.size() == series.size());
    REQUIRE(b.size() == series.size());
    for (std::size_t i = 0; i < a.size(); i++) CHECK_NEAR(b[i], a[i], 1e-12);
}

// asymmetric bounds work too, and the midpoint of the interval is the new zero
TEST(asymmetricBoundsCenterOnTheirMidpoint) {
    SetAnalytics sa({5.0, 2.5, 7.5});
    auto out = sa.fisherTransform(0.0, 10.0);

    REQUIRE(out.size() == (std::size_t)3);
    CHECK_NEAR(out[0], 0.0, 1e-12);      // midpoint maps to zero
    CHECK_NEAR(out[1], -out[2], 1e-12);  // and it stays odd around that midpoint
}

TEST(isMonotonicWithinArbitraryBounds) {
    SetAnalytics sa({0.1, 2.0, 5.0, 8.0, 9.9});
    auto out = sa.fisherTransform(0.0, 10.0);

    REQUIRE(out.size() == (std::size_t)5);
    for (std::size_t i = 1; i < out.size(); i++) CHECK(out[i] > out[i - 1]);
}

// the poles moved to the bounds, so values crowding either bound blow up the
// same way values used to crowd +/-1. the guard keeps the pole itself out
TEST(divergesTowardTheBoundsRatherThanPlusMinusOne) {
    SetAnalytics sa({0.001, 9.999});
    auto out = sa.fisherTransform(0.0, 10.0);

    REQUIRE(out.size() == (std::size_t)2);
    CHECK(out[0] < -4.0);
    CHECK(out[1] >  4.0);
    CHECK(std::isfinite(out[0]));
    CHECK(std::isfinite(out[1]));
}
