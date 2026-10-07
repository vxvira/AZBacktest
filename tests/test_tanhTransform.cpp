// Unit tests for SetAnalytics::tanhTransform, the squashing counterpart to
// fisherTransform - tanh is atanh's inverse, so the two are tested as a pair
// where it makes sense. unlike fisherTransform it takes any real input, there
// is no pole to guard against

#include "tests/testFramework.h"

#include "src/analytics/SetAnalytics.h"

// ------------------------------------------------------------ happy path

TEST(transformsEveryValueInOrder) {
    std::vector<double> series = {-0.9, -0.5, -0.1, 0.0, 0.1, 0.5, 0.9};
    SetAnalytics sa(series);
    auto out = sa.tanhTransform();

    REQUIRE(out.size() == series.size());
    for (std::size_t i = 0; i < series.size(); i++) {
        CHECK_NEAR(out[i], std::tanh(series[i]), 1e-12);
    }
}

TEST(zeroMapsToZero) {
    SetAnalytics sa({0.0});
    auto out = sa.tanhTransform();

    REQUIRE(out.size() == (std::size_t)1);
    CHECK_NEAR(out[0], 0.0, 1e-12);
}

TEST(isOddSymmetric) {
    SetAnalytics sa({0.25, -0.25, 0.75, -0.75, 4.0, -4.0});
    auto out = sa.tanhTransform();

    REQUIRE(out.size() == (std::size_t)6);
    CHECK_NEAR(out[0], -out[1], 1e-12);
    CHECK_NEAR(out[2], -out[3], 1e-12);
    CHECK_NEAR(out[4], -out[5], 1e-12);
}

// the mirror image of fisherTransform: monotonic, but it compresses toward the
// origin instead of stretching away from it
TEST(isMonotonicAndCompresses) {
    std::vector<double> series = {-8.0, -0.99, -0.5, 0.0, 0.5, 0.99, 8.0};
    SetAnalytics sa(series);
    auto out = sa.tanhTransform();

    REQUIRE(out.size() == series.size());
    for (std::size_t i = 1; i < out.size(); i++) CHECK(out[i] > out[i - 1]);

    // |tanh(x)| < |x| everywhere except 0
    for (std::size_t i = 0; i < series.size(); i++) {
        if (series[i] != 0.0) CHECK(std::fabs(out[i]) < std::fabs(series[i]));
    }
}

TEST(knownValues) {
    SetAnalytics sa({0.5, -0.5, 0.9, 2.0});
    auto out = sa.tanhTransform();

    REQUIRE(out.size() == (std::size_t)4);
    CHECK_NEAR(out[0],  0.4621171572600098, 1e-12);
    CHECK_NEAR(out[1], -0.4621171572600098, 1e-12);
    CHECK_NEAR(out[2],  0.7162978701990245, 1e-12);
    CHECK_NEAR(out[3],  0.9640275800758169, 1e-12);
}

TEST(emptySeriesYieldsEmptyResult) {
    SetAnalytics sa({});
    auto out = sa.tanhTransform();
    CHECK_EQ(out.size(), (std::size_t)0);
}

// no tail-slice / warmup convention here, same as fisherTransform, every input
// point produces an output point no matter how short the series is
TEST(doesNotSliceOrWarmUp) {
    SetAnalytics sa({0.1, 0.2, 0.3});
    auto out = sa.tanhTransform();
    CHECK_EQ(out.size(), (std::size_t)3);
    CHECK_NEAR(out[0], std::tanh(0.1), 1e-12);
}

TEST(picksUpValuesAddedByUpdateVector) {
    SetAnalytics sa({0.1});
    sa.updateVector(0.2);
    sa.updateVector(0.3);

    auto out = sa.tanhTransform();
    REQUIRE(out.size() == (std::size_t)3);
    CHECK_NEAR(out[2], std::tanh(0.3), 1e-12);
}

// ------------------------------------------------------- unbounded domain

// the whole point of dropping the +/-1 guard: tanh is total on R, so anything
// a strategy can hand it comes back with a real answer
TEST(acceptsValuesWellOutsideTheUnitInterval) {
    std::vector<double> series = {-100.0, -3.0, -1.0, 1.0, 3.0, 100.0};
    SetAnalytics sa(series);
    auto out = sa.tanhTransform();

    REQUIRE(out.size() == series.size());
    for (std::size_t i = 0; i < series.size(); i++) {
        CHECK_NEAR(out[i], std::tanh(series[i]), 1e-12);
        CHECK(std::isfinite(out[i]));
    }
}

// z-scores are the obvious feeder series and they are unbounded, so a wide one
// has to survive the transform rather than blow it up
TEST(squashesAnUnboundedSeriesIntoTheUnitInterval) {
    SetAnalytics sa({-42.0, -6.5, -0.3, 0.0, 0.3, 6.5, 42.0});
    auto out = sa.tanhTransform();

    REQUIRE(out.size() == (std::size_t)7);
    for (double v : out) {
        CHECK(std::isfinite(v));
        CHECK(v >= -1.0);
        CHECK(v <=  1.0);
    }
}

// far out in the tails tanh saturates rather than diverging, which is the
// property that makes it usable as a clamp. in exact math it approaches +/-1
// without ever reaching it, but in double it rounds all the way to 1.0 once
// |x| passes about 19.06, so the reachable range is CLOSED, not open. anything
// downstream dividing by (1 - tanh(x)) has to expect a zero denominator
TEST(saturatesToExactlyPlusMinusOnePastTheRoundingThreshold) {
    SetAnalytics sa({20.0, -20.0});
    auto out = sa.tanhTransform();

    REQUIRE(out.size() == (std::size_t)2);
    CHECK_EQ(out[0],  1.0);
    CHECK_EQ(out[1], -1.0);
}

// just under that threshold it is still strictly inside the interval, so the
// saturation above is a floating point artifact rather than a clamp in the code
TEST(staysStrictlyInsideTheIntervalBelowTheThreshold) {
    SetAnalytics sa({19.0, -19.0});
    auto out = sa.tanhTransform();

    REQUIRE(out.size() == (std::size_t)2);
    CHECK(out[0] <  1.0);
    CHECK(out[1] > -1.0);
    CHECK_NEAR(out[0],  1.0, 1e-12);
    CHECK_NEAR(out[1], -1.0, 1e-12);
}

// ------------------------------------------------- relationship to fisher

// tanh undoes atanh, and with the guard gone that holds across fisher's whole
// output range rather than only while it stays inside +/-1
TEST(roundTripsWithFisherTransform) {
    std::vector<double> series = {-0.95, -0.6, -0.25, 0.0, 0.25, 0.6, 0.95};

    SetAnalytics fwd(series);
    auto fishered = fwd.fisherTransform();
    REQUIRE(fishered.size() == series.size());

    // 0.95 stretches to 1.83, the kind of value the old guard rejected
    CHECK(std::fabs(fishered[0]) > 1.0);
    CHECK(std::fabs(fishered[6]) > 1.0);

    SetAnalytics back(fishered);
    auto out = back.tanhTransform();

    REQUIRE(out.size() == series.size());
    for (std::size_t i = 0; i < series.size(); i++) CHECK_NEAR(out[i], series[i], 1e-12);
}
