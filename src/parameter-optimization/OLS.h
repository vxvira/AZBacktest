// @file Closed form OLS
// Opt-in: #define AZBT_OLS before including (needs -Ivendor/eigen), so
// users who never call fitOLS don't need Eigen on their include path.

#ifdef AZBT_OLS

#include <Eigen/Dense>

struct LinFit {
    Eigen::VectorXd w;  // weights, one per feature
    double b;           // bias (intercept)
};

// X: n x p features (no column of 1s), y: n targets
LinFit fitOLS(const Eigen::MatrixXd& X, const Eigen::VectorXd& y) {
    const Eigen::Index n = X.rows(), p = X.cols();
    Eigen::MatrixXd A(n, p + 1);
    A.col(0).setOnes();        // bias column
    A.rightCols(p) = X;
    Eigen::VectorXd theta = A.colPivHouseholderQr().solve(y);
    return { theta.tail(p), theta(0) };
}

#endif // AZBT_OLS
