#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

//==============================================================================
// XOA - small dense linear algebra (SVD + pseudo-inverse).
//
// spatcore carries no linear algebra, so this is app-local. The single client
// today is the WP5 decoder designer (mode-matching pseudo-inverse of the
// 121xL spherical-harmonic sampling matrix); WP7 AllRAD/VBAP reuse it.
//
// Algorithm: one-sided (Hestenes) Jacobi SVD by cyclic column orthogonalization.
// Deterministic, double precision, allocation only at entry. Chosen over
// Golub-Kahan for its ~100-line footprint, unconditional stability on
// rank-deficient inputs (the coplanar-ring case, FR-17), and high relative
// accuracy on the small singular values that drive the condition number.
//
// Convention: A = U * diag(sigma) * V^T, with A m-by-n (m >= n), U m-by-n
// (orthonormal columns), sigma length n (>= 0, DESCENDING), V n-by-n
// orthogonal. Singular-vector column signs are NOT pinned (only sigma and the
// pseudo-inverse are convention-stable); tests never compare U/V entries.
//==============================================================================

namespace xoa::linalg
{

// One-sided Jacobi rotates a pair only when the columns are non-orthogonal
// beyond this relative threshold; the absolute floor lets exactly-zero columns
// (rank-deficient inputs) skip cleanly without a 0/0.
constexpr double kJacobiRelTol = 1.0e-15;
constexpr double kJacobiAbsFloor = 1.0e-300;
constexpr int    kJacobiMaxSweeps = 30;

struct SvdResult
{
    int m = 0, n = 0;
    std::vector<double> u;       // m*n, row-major
    std::vector<double> sigma;   // n, descending, >= 0
    std::vector<double> v;       // n*n, row-major
    int  sweepsUsed = 0;
    bool converged  = false;
};

/** One-sided Jacobi SVD of a row-major m-by-n matrix, m >= n. */
inline SvdResult jacobiSvd (const double* a, int m, int n)
{
    jassert (a != nullptr && m >= n && n > 0);

    SvdResult r;
    r.m = m;
    r.n = n;

    // Work matrix W = A (columns get orthogonalized in place); V starts at I.
    std::vector<double> w (static_cast<size_t> (m) * n);
    for (int i = 0; i < m; ++i)
        for (int j = 0; j < n; ++j)
            w[static_cast<size_t> (i) * n + j] = a[static_cast<size_t> (i) * n + j];

    std::vector<double> v (static_cast<size_t> (n) * n, 0.0);
    for (int j = 0; j < n; ++j)
        v[static_cast<size_t> (j) * n + j] = 1.0;

    auto colDot = [&] (int p, int q)
    {
        double s = 0.0;
        for (int i = 0; i < m; ++i)
            s += w[static_cast<size_t> (i) * n + p] * w[static_cast<size_t> (i) * n + q];
        return s;
    };

    int sweep = 0;
    for (; sweep < kJacobiMaxSweeps; ++sweep)
    {
        bool rotated = false;
        for (int p = 0; p < n - 1; ++p)
        {
            for (int q = p + 1; q < n; ++q)
            {
                const double alpha = colDot (p, p);
                const double beta  = colDot (q, q);
                const double gamma = colDot (p, q);

                if (std::abs (gamma) <= kJacobiRelTol * std::sqrt (alpha * beta) + kJacobiAbsFloor)
                    continue;

                // Rutishauser rotation that diagonalizes the 2x2 Gram block.
                const double zeta = (beta - alpha) / (2.0 * gamma);
                const double t = (zeta >= 0.0 ? 1.0 : -1.0)
                                 / (std::abs (zeta) + std::sqrt (1.0 + zeta * zeta));
                const double c = 1.0 / std::sqrt (1.0 + t * t);
                const double s = c * t;

                for (int i = 0; i < m; ++i)
                {
                    double& wp = w[static_cast<size_t> (i) * n + p];
                    double& wq = w[static_cast<size_t> (i) * n + q];
                    const double tp = wp, tq = wq;
                    wp = c * tp - s * tq;
                    wq = s * tp + c * tq;
                }
                for (int i = 0; i < n; ++i)
                {
                    double& vp = v[static_cast<size_t> (i) * n + p];
                    double& vq = v[static_cast<size_t> (i) * n + q];
                    const double tp = vp, tq = vq;
                    vp = c * tp - s * tq;
                    vq = s * tp + c * tq;
                }
                rotated = true;
            }
        }
        if (! rotated)
            break;
    }
    r.sweepsUsed = sweep + (sweep < kJacobiMaxSweeps ? 1 : 0);
    r.converged  = sweep < kJacobiMaxSweeps;

    // Singular values = column norms of W; U = normalized columns.
    std::vector<double> sig (static_cast<size_t> (n));
    for (int j = 0; j < n; ++j)
    {
        double s = 0.0;
        for (int i = 0; i < m; ++i)
            s += w[static_cast<size_t> (i) * n + j] * w[static_cast<size_t> (i) * n + j];
        sig[static_cast<size_t> (j)] = std::sqrt (s);
    }

    // Descending sort (stable), permuting sigma, W-columns (-> U), V-columns.
    std::vector<int> order (static_cast<size_t> (n));
    std::iota (order.begin(), order.end(), 0);
    std::stable_sort (order.begin(), order.end(),
                      [&] (int i, int j) { return sig[static_cast<size_t> (i)] > sig[static_cast<size_t> (j)]; });

    r.sigma.resize (static_cast<size_t> (n));
    r.u.assign (static_cast<size_t> (m) * n, 0.0);
    r.v.assign (static_cast<size_t> (n) * n, 0.0);
    for (int newCol = 0; newCol < n; ++newCol)
    {
        const int src = order[static_cast<size_t> (newCol)];
        const double sv = sig[static_cast<size_t> (src)];
        r.sigma[static_cast<size_t> (newCol)] = sv;
        if (sv > 0.0)
            for (int i = 0; i < m; ++i)
                r.u[static_cast<size_t> (i) * n + newCol] = w[static_cast<size_t> (i) * n + src] / sv;
        for (int i = 0; i < n; ++i)
            r.v[static_cast<size_t> (i) * n + newCol] = v[static_cast<size_t> (i) * n + src];
    }

    return r;
}

struct PinvOptions
{
    double rankToleranceRel  = 1.0e-9;   // singular values <= this * sigmaMax are dropped
    double tikhonovLambdaRel = 0.0;      // lambda = this * sigmaMax; 0 => plain 1/sigma
};

struct PinvResult
{
    std::vector<double> pinv;   // n*m, row-major (pseudo-inverse of the n>... input)
    double sigmaMax = 0.0, sigmaMin = 0.0, conditionNumber = 0.0;
    int  effectiveRank = 0;
    bool converged = false;
};

/** Moore-Penrose pseudo-inverse of a row-major m-by-n matrix (m >= n), with
    truncated-SVD rank control and optional Tikhonov shaping. Returns the n-by-m
    pseudo-inverse. conditionNumber is sigmaMax/sigmaMin over ALL n singular
    values (a rig property; infinite when sigmaMin underflows). */
inline PinvResult pseudoInverse (const double* a, int m, int n, const PinvOptions& opts = {})
{
    const auto svd = jacobiSvd (a, m, n);

    PinvResult r;
    r.converged = svd.converged;
    r.sigmaMax = svd.sigma.front();
    r.sigmaMin = svd.sigma.back();
    r.conditionNumber = (r.sigmaMin > 0.0) ? (r.sigmaMax / r.sigmaMin)
                                           : std::numeric_limits<double>::infinity();

    const double cutoff = opts.rankToleranceRel * r.sigmaMax;
    const double lambda = opts.tikhonovLambdaRel * r.sigmaMax;

    // Per-singular-value filter f(sigma).
    std::vector<double> f (static_cast<size_t> (n), 0.0);
    for (int j = 0; j < n; ++j)
    {
        const double sv = svd.sigma[static_cast<size_t> (j)];
        if (sv > cutoff)
        {
            f[static_cast<size_t> (j)] = sv / (sv * sv + lambda * lambda);
            ++r.effectiveRank;
        }
    }

    // pinv = V * diag(f) * U^T, n-by-m.
    r.pinv.assign (static_cast<size_t> (n) * m, 0.0);
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < m; ++k)
        {
            double acc = 0.0;
            for (int j = 0; j < n; ++j)
                acc += svd.v[static_cast<size_t> (i) * n + j] * f[static_cast<size_t> (j)]
                       * svd.u[static_cast<size_t> (k) * n + j];
            r.pinv[static_cast<size_t> (i) * m + k] = acc;
        }

    return r;
}

} // namespace xoa::linalg
