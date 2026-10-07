// Unit tests for SetAnalytics::computeCorrelation, the Pearson correlation
// coefficient between the object's own series and a second vector.

#include "tests/testFramework.h"

#include "src/analytics/SetAnalytics.h"

TEST(perfectPositiveLinearCorrelationIsOne) {
    std::vector<double> x = {1, 2, 3, 4, 5};
    std::vector<double> y = {2, 4, 6, 8, 10}; // y = 2x
    SetAnalytics sa(x);
    CHECK_F(sa.computeCorrelation(y), 1.0);
}

TEST(perfectNegativeLinearCorrelationIsNegativeOne) {
    std::vector<double> x = {1, 2, 3, 4, 5};
    std::vector<double> y = {10, 8, 6, 4, 2}; // y = -2x + 12
    SetAnalytics sa(x);
    CHECK_F(sa.computeCorrelation(y), -1.0);
}

TEST(identicalSeriesCorrelatesWithItself) {
    std::vector<double> x = {3, 1, 4, 1, 5, 9, 2, 6};
    std::vector<double> y = x;
    SetAnalytics sa(x);
    CHECK_F(sa.computeCorrelation(y), 1.0);
}

// a flat series has zero variance, which would divide by zero in the raw
// formula, computeCorrelation guards that and returns 0 instead
TEST(constantSeriesReturnsZeroInsteadOfDividingByZero) {
    std::vector<double> x = {5, 5, 5, 5, 5};
    std::vector<double> y = {1, 2, 3, 4, 5};
    SetAnalytics sa(x);
    CHECK_F(sa.computeCorrelation(y), 0.0);
}

TEST(knownDatasetMatchesHandComputedPearsonR) {
    std::vector<double> x = {10, 20, 30, 40, 50};
    std::vector<double> y = {15, 25, 35, 45, 60};
    SetAnalytics sa(x);
    CHECK_NEAR(sa.computeCorrelation(y), 0.9958932064677039, 1e-9);
}

TEST(shuffledSeriesMatchesHandComputedPearsonR) {
    std::vector<double> x = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<double> y = {5, 1, 4, 2, 8, 3, 7, 6};
    SetAnalytics sa(x);
    CHECK_NEAR(sa.computeCorrelation(y), 0.47619047619047616, 1e-9);
}

// bucket(): values land in [k*size, (k+1)*size), keyed by the lower edge

TEST(bucketByOneCountsPerUnitInterval) {
    SetAnalytics sa({0.5, 0.7, 1.8, 4.3});
    std::map<double, int> b = sa.bucket(1.0);
    CHECK_EQ((int)b.size(), 3);
    CHECK_EQ(b[0.0], 2);
    CHECK_EQ(b[1.0], 1);
    CHECK_EQ(b[4.0], 1);
}

TEST(bucketEdgeValueGoesToUpperBucket) {
    SetAnalytics sa({1.0, 2.0, 2.0});
    std::map<double, int> b = sa.bucket(1.0);
    CHECK_EQ(b[1.0], 1);
    CHECK_EQ(b[2.0], 2);
}

TEST(bucketNegativesFloorDownNotTowardZero) {
    SetAnalytics sa({-0.5, -1.5, 0.5});
    std::map<double, int> b = sa.bucket(1.0);
    CHECK_EQ(b[-1.0], 1);
    CHECK_EQ(b[-2.0], 1);
    CHECK_EQ(b[0.0], 1);
}

TEST(bucketWiderSize) {
    SetAnalytics sa({0.5, 0.7, 1.8, 4.3});
    std::map<double, int> b = sa.bucket(2.0);
    CHECK_EQ((int)b.size(), 2);
    CHECK_EQ(b[0.0], 3);
    CHECK_EQ(b[4.0], 1);
}

TEST(bucketNonPositiveSizeThrows) {
    SetAnalytics sa({1.0});
    bool threw = false;
    try { sa.bucket(0.0); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}
