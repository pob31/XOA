/*
    XoaRotationTests.cpp - WP4 tests: SO(3) soundfield rotation
    (Ivanic-Ruedenberg 1996 + 1998 erratum) and mirror planes.

    Correctness triangle (all independent):
      R2  property  apply(build(R), encode(d)) == encode(R·d)  via WP3 encode
      R3  goldens   quadrature-projected matrices (no Wigner-D, no I-R)
      R5  orthogonality per block

    Golden data tests/data/rotation_reference.json is produced by
    tools/reference/gen_rotation_reference.py.
*/

#include "XoaTestFramework.h"

#include "DSP/AmbiMirror.h"
#include "DSP/AmbiRotation.h"
#include "DSP/AmbiSphericalHarmonics.h"
#include "Helpers/XoaCoordinates.h"
#include "XoaConstants.h"

#include <array>
#include <cmath>
#include <vector>

namespace rot = xoa::rot;
namespace mir = xoa::mirror;
namespace sh = xoa::sh;

//==============================================================================
static juce::var loadRotJson()
{
    const auto file = juce::File (XOA_TESTS_DATA_DIR).getChildFile ("rotation_reference.json");
    const auto parsed = juce::JSON::parse (file.loadFileAsString());
    CHECK (parsed.isObject());
    return parsed;
}

static double rnum (const juce::var& v) { return static_cast<double> (v); }
static bool rapprox (double a, double b, double tol) noexcept { return std::abs (a - b) <= tol; }

static double rmaxDiff (const double* a, const double* b, int n) noexcept
{
    double m = 0.0;
    for (int i = 0; i < n; ++i)
        m = std::max (m, std::abs (a[i] - b[i]));
    return m;
}

// Deterministic random unit quaternion (reject near-zero norm) and direction.
static rot::Quaternion randomUnitQuat (juce::Random& r)
{
    for (;;)
    {
        rot::Quaternion q { r.nextDouble() * 2 - 1, r.nextDouble() * 2 - 1,
                            r.nextDouble() * 2 - 1, r.nextDouble() * 2 - 1 };
        const double n = std::sqrt (q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        if (n > 0.25)
            return { q.w / n, q.x / n, q.y / n, q.z / n };
    }
}

static xoa::coords::Cartesian randomDirection (juce::Random& r)
{
    const double z = r.nextDouble() * 2 - 1;
    const double az = juce::degreesToRadians (r.nextDouble() * 360 - 180);
    const double rho = std::sqrt (std::max (0.0, 1.0 - z * z));
    return { rho * std::cos (az), rho * std::sin (az), z };
}

// Test-local Hamilton product (R4): R(q1∘q2) = R(q1)·R(q2), q2 applied first.
static rot::Quaternion quatMul (const rot::Quaternion& a, const rot::Quaternion& b)
{
    return { a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
             a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
}

//==============================================================================
// R1 - storage / identity
//==============================================================================
static void testStorage()
{
    // blockOffset closed form == running sum of blockSize
    int running = 0;
    for (int l = 0; l <= xoa::kAmbisonicOrder; ++l)
    {
        CHECK (rot::blockOffset (l) == running);
        running += rot::blockSize (l);
    }
    CHECK (rot::numCoeffs (xoa::kAmbisonicOrder) == rot::kNumRotationCoeffs);
    CHECK (rot::kNumRotationCoeffs == 1771);

    // entry() addressing bijective over all (l,m,m')
    rot::RotationMatrix M;
    for (double& c : M.coeffs) c = -1.0;
    int idx = 0;
    for (int l = 0; l <= xoa::kAmbisonicOrder; ++l)
        for (int m = -l; m <= l; ++m)
            for (int mp = -l; mp <= l; ++mp)
                M.entry (l, m, mp) = static_cast<double> (idx++);
    CHECK (idx == 1771);
    idx = 0;
    for (double c : M.coeffs) CHECK (c == static_cast<double> (idx++));   // no gaps/overlaps

    // identity
    rot::RotationMatrix I;
    rot::identity (I);
    for (int l = 0; l <= xoa::kAmbisonicOrder; ++l)
        for (int m = -l; m <= l; ++m)
            for (int mp = -l; mp <= l; ++mp)
                CHECK (I.entry (l, m, mp) == (m == mp ? 1.0 : 0.0));

    double v[xoa::kNumSHChannels], out[xoa::kNumSHChannels];
    for (int c = 0; c < xoa::kNumSHChannels; ++c) v[c] = 0.3 * c - 5.0;
    rot::apply (I, xoa::kAmbisonicOrder, v, out);
    CHECK (rmaxDiff (v, out, xoa::kNumSHChannels) == 0.0);
}

//==============================================================================
// R2 - the decisive property: apply(build(R), encode(d)) == encode(R·d)
//==============================================================================
static void testRotationProperty()
{
    juce::Random r (0x584f4152);
    for (int t = 0; t < 15; ++t)
    {
        const auto q = randomUnitQuat (r);
        const auto M = rot::quaternionToMatrix (q);
        rot::RotationMatrix R;
        if (t % 2 == 0) rot::buildFromQuaternion (q, R);
        else            rot::buildFromCartesian (M, R);

        const auto d = randomDirection (r);
        const auto rd = rot::transform (M, d);

        double enc[xoa::kNumSHChannels], rotated[xoa::kNumSHChannels], encRd[xoa::kNumSHChannels];
        sh::evaluate (d, xoa::kAmbisonicOrder, enc);
        sh::evaluate (rd, xoa::kAmbisonicOrder, encRd);

        // Every order 0..10 must satisfy the property at that order.
        for (int order = 0; order <= xoa::kAmbisonicOrder; ++order)
        {
            rot::apply (R, order, enc, rotated);
            for (int c = 0; c < sh::numChannels (order); ++c)
                CHECK (rapprox (rotated[c], encRd[c], 1e-12));
        }
    }
}

//==============================================================================
// R3 - goldens via all three build entry points
//==============================================================================
static void testGoldenMatrices()
{
    const auto doc = loadRotJson();
    const double tol = rnum (doc["provenance"]["testTolerance"]);   // 1e-12
    const auto rotations = doc["rotations"];

    for (int e = 0; e < rotations.size(); ++e)
    {
        const auto entry = rotations[e];
        const auto blocksVar = entry["blocks"];
        if (! blocksVar.isArray())
            continue;   // composition entries carry no blocks

        // reference flat coefficients
        rot::RotationMatrix ref;
        int flat = 0;
        for (int l = 0; l <= xoa::kAmbisonicOrder; ++l)
        {
            const auto blk = blocksVar[l];
            CHECK (blk.size() == rot::blockSize (l));
            for (int i = 0; i < blk.size(); ++i)
                ref.coeffs[flat++] = rnum (blk[i]);
        }
        CHECK (flat == rot::kNumRotationCoeffs);

        const auto ypr = entry["yawPitchRollDeg"];
        rot::RotationMatrix fromYpr, fromQuat, fromCart;
        rot::buildFromYawPitchRoll (rnum (ypr[0]), rnum (ypr[1]), rnum (ypr[2]), fromYpr);

        const auto q = entry["quaternion"];
        rot::buildFromQuaternion ({ rnum (q[0]), rnum (q[1]), rnum (q[2]), rnum (q[3]) }, fromQuat);

        const auto cart = entry["cartesian"];
        rot::Mat3 m;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                m.m[i][j] = rnum (cart[i][j]);
        rot::buildFromCartesian (m, fromCart);

        CHECK (rmaxDiff (fromYpr.coeffs, ref.coeffs, rot::kNumRotationCoeffs) < tol);
        CHECK (rmaxDiff (fromQuat.coeffs, ref.coeffs, rot::kNumRotationCoeffs) < tol);
        CHECK (rmaxDiff (fromCart.coeffs, ref.coeffs, rot::kNumRotationCoeffs) < tol);
    }
}

//==============================================================================
// R4 - composition
//==============================================================================
static void testComposition()
{
    const auto doc = loadRotJson();
    const auto rotations = doc["rotations"];

    // gather quaternions by name
    auto findQuat = [&] (const juce::String& name) -> rot::Quaternion
    {
        for (int e = 0; e < rotations.size(); ++e)
        {
            const auto entry = rotations[e];
            if (entry["name"].toString() == name)
            {
                const auto q = entry["quaternion"];
                return { rnum (q[0]), rnum (q[1]), rnum (q[2]), rnum (q[3]) };
            }
        }
        CHECK (false);
        return { 1, 0, 0, 0 };
    };

    for (int e = 0; e < rotations.size(); ++e)
    {
        const auto entry = rotations[e];
        const auto composed = entry["composedFrom"];
        if (! composed.isArray())
            continue;

        const auto qA = findQuat (composed[0].toString());
        const auto qB = findQuat (composed[1].toString());
        const auto q12golden = entry["quaternion"];

        // (c) test-local Hamilton multiply reproduces the golden composed quat (up to sign)
        const auto q12 = quatMul (qA, qB);
        const double s = (q12.w * rnum (q12golden[0]) < 0) ? -1.0 : 1.0;
        CHECK (rapprox (s * q12.w, rnum (q12golden[0]), 1e-12));
        CHECK (rapprox (s * q12.x, rnum (q12golden[1]), 1e-12));
        CHECK (rapprox (s * q12.y, rnum (q12golden[2]), 1e-12));
        CHECK (rapprox (s * q12.z, rnum (q12golden[3]), 1e-12));

        rot::RotationMatrix RA, RB, R12;
        rot::buildFromQuaternion (qA, RA);
        rot::buildFromQuaternion (qB, RB);
        rot::buildFromQuaternion (q12, R12);

        // (a) block-level R^l(qA)·R^l(qB) == R^l(q12)
        for (int l = 0; l <= xoa::kAmbisonicOrder; ++l)
        {
            const int n = 2 * l + 1;
            const double* a = RA.block (l);
            const double* b = RB.block (l);
            const double* c = R12.block (l);
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j)
                {
                    double acc = 0.0;
                    for (int k = 0; k < n; ++k)
                        acc += a[i * n + k] * b[k * n + j];
                    CHECK (rapprox (acc, c[i * n + j], 1e-12));
                }
        }

        // (b) vector-level apply-chain == apply(compose)
        double v[xoa::kNumSHChannels], viaB[xoa::kNumSHChannels], chain[xoa::kNumSHChannels],
               direct[xoa::kNumSHChannels];
        for (int i = 0; i < xoa::kNumSHChannels; ++i) v[i] = std::sin (0.7 * i) + 0.3;
        rot::apply (RB, xoa::kAmbisonicOrder, v, viaB);
        rot::apply (RA, xoa::kAmbisonicOrder, viaB, chain);
        rot::apply (R12, xoa::kAmbisonicOrder, v, direct);
        CHECK (rmaxDiff (chain, direct, xoa::kNumSHChannels) < 1e-12);
    }
}

//==============================================================================
// R5 - orthogonality
//==============================================================================
static void testOrthogonality()
{
    juce::Random r (0x0157a11e);
    std::vector<rot::RotationMatrix> mats (6);
    rot::buildFromYawPitchRoll (90, 0, 0, mats[0]);
    rot::buildFromYawPitchRoll (0, 90, 0, mats[1]);
    rot::buildFromYawPitchRoll (0, 0, 90, mats[2]);
    for (int k = 3; k < 6; ++k)
        rot::buildFromQuaternion (randomUnitQuat (r), mats[k]);

    for (const auto& R : mats)
    {
        for (int l = 0; l <= xoa::kAmbisonicOrder; ++l)
        {
            const int n = 2 * l + 1;
            const double* b = R.block (l);
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j)
                {
                    double acc = 0.0;   // (R^T R)_{ij} = sum_k R_{ki} R_{kj}
                    for (int k = 0; k < n; ++k)
                        acc += b[k * n + i] * b[k * n + j];
                    CHECK (rapprox (acc, (i == j ? 1.0 : 0.0), 1e-12));
                }
        }

        // l=1 det == +1
        const double* b = R.block (1);
        const double det = b[0] * (b[4] * b[8] - b[5] * b[7])
                         - b[1] * (b[3] * b[8] - b[5] * b[6])
                         + b[2] * (b[3] * b[7] - b[4] * b[6]);
        CHECK (rapprox (det, 1.0, 1e-12));
    }
}

//==============================================================================
// R6 - YPR semantics (the DOCUMENTED convention)
//==============================================================================
static void testYawPitchRoll()
{
    double a[xoa::kNumSHChannels], b[xoa::kNumSHChannels], enc[xoa::kNumSHChannels];

    // (a) yaw+ turns the scene toward +Y (az increases)
    {
        rot::RotationMatrix R;
        rot::buildFromYawPitchRoll (40, 0, 0, R);
        sh::evaluate (10.0, 25.0, xoa::kAmbisonicOrder, enc);
        rot::apply (R, xoa::kAmbisonicOrder, enc, a);
        sh::evaluate (50.0, 25.0, xoa::kAmbisonicOrder, b);   // az 10 + 40
        CHECK (rmaxDiff (a, b, xoa::kNumSHChannels) < 1e-12);

        // all-orders closed-form yaw ladder (golden-free)
        const double alpha = juce::degreesToRadians (40.0);
        for (int l = 1; l <= xoa::kAmbisonicOrder; ++l)
            for (int mu = 1; mu <= l; ++mu)
            {
                CHECK (rapprox (R.entry (l, -mu, -mu), std::cos (mu * alpha), 1e-12));
                CHECK (rapprox (R.entry (l,  mu,  mu), std::cos (mu * alpha), 1e-12));
                CHECK (rapprox (R.entry (l, -mu,  mu), std::sin (mu * alpha), 1e-12));
                CHECK (rapprox (R.entry (l,  mu, -mu), -std::sin (mu * alpha), 1e-12));
            }
    }

    // (b) pitch+ tips the front DOWN: encode(0,0) -> encode(0,-b); zenith -> (0,90-b)
    {
        rot::RotationMatrix R;
        rot::buildFromYawPitchRoll (0, 30, 0, R);
        sh::evaluate (0.0, 0.0, xoa::kAmbisonicOrder, enc);
        rot::apply (R, xoa::kAmbisonicOrder, enc, a);
        sh::evaluate (0.0, -30.0, xoa::kAmbisonicOrder, b);
        CHECK (rmaxDiff (a, b, xoa::kNumSHChannels) < 1e-12);

        sh::evaluate (0.0, 90.0, xoa::kAmbisonicOrder, enc);   // zenith
        rot::apply (R, xoa::kAmbisonicOrder, enc, a);
        sh::evaluate (0.0, 60.0, xoa::kAmbisonicOrder, b);     // 90 - 30 toward front
        CHECK (rmaxDiff (a, b, xoa::kNumSHChannels) < 1e-12);
    }

    // (c) roll+ raises the left side; front invariant
    {
        rot::RotationMatrix R;
        rot::buildFromYawPitchRoll (0, 0, 35, R);
        sh::evaluate (0.0, 0.0, xoa::kAmbisonicOrder, enc);    // front
        rot::apply (R, xoa::kAmbisonicOrder, enc, a);
        CHECK (rmaxDiff (a, enc, xoa::kNumSHChannels) < 1e-12);

        sh::evaluate (90.0, 0.0, xoa::kAmbisonicOrder, enc);   // left
        rot::apply (R, xoa::kAmbisonicOrder, enc, a);
        sh::evaluate (90.0, 35.0, xoa::kAmbisonicOrder, b);    // raised
        CHECK (rmaxDiff (a, b, xoa::kNumSHChannels) < 1e-12);
    }

    // (d) composition order == Rz·Ry·Rx (roll applied first)
    {
        rot::RotationMatrix Rypr, Rz, Ry, Rx;
        rot::buildFromYawPitchRoll (25, -40, 70, Rypr);
        rot::buildFromYawPitchRoll (25, 0, 0, Rz);
        rot::buildFromYawPitchRoll (0, -40, 0, Ry);
        rot::buildFromYawPitchRoll (0, 0, 70, Rx);

        double v[xoa::kNumSHChannels], t1[xoa::kNumSHChannels], t2[xoa::kNumSHChannels],
               chain[xoa::kNumSHChannels], direct[xoa::kNumSHChannels];
        for (int i = 0; i < xoa::kNumSHChannels; ++i) v[i] = std::cos (0.5 * i) - 0.2;
        rot::apply (Rx, xoa::kAmbisonicOrder, v, t1);
        rot::apply (Ry, xoa::kAmbisonicOrder, t1, t2);
        rot::apply (Rz, xoa::kAmbisonicOrder, t2, chain);
        rot::apply (Rypr, xoa::kAmbisonicOrder, v, direct);
        CHECK (rmaxDiff (chain, direct, xoa::kNumSHChannels) < 1e-12);
    }
}

//==============================================================================
// R7 - quaternion behavior
//==============================================================================
static void testQuaternion()
{
    juce::Random r (0x9ee7c0de);
    for (int t = 0; t < 5; ++t)
    {
        const auto q = randomUnitQuat (r);
        rot::RotationMatrix Rq, Rneg, Rscaled;
        rot::buildFromQuaternion (q, Rq);
        rot::buildFromQuaternion ({ -q.w, -q.x, -q.y, -q.z }, Rneg);
        rot::buildFromQuaternion ({ 2 * q.w, 2 * q.x, 2 * q.y, 2 * q.z }, Rscaled);
        CHECK (rmaxDiff (Rq.coeffs, Rneg.coeffs, rot::kNumRotationCoeffs) < 1e-12);   // q == -q
        CHECK (rmaxDiff (Rq.coeffs, Rscaled.coeffs, rot::kNumRotationCoeffs) < 1e-12); // normalize
    }

    // identity quaternion == identity()
    rot::RotationMatrix Rid, I;
    rot::buildFromQuaternion ({ 1, 0, 0, 0 }, Rid);
    rot::identity (I);
    CHECK (rmaxDiff (Rid.coeffs, I.coeffs, rot::kNumRotationCoeffs) == 0.0);

    // Pure-axis quaternions vs the physically-anchored YPR builds (R6 pins YPR
    // to encode). All three axes checked so the x/y quaternion sense is pinned
    // by an encode-anchored path, not only by shared-formula goldens: qz(d) is
    // yaw d, qy(d) is pitch d, qx(d) is roll d.
    const double half = juce::degreesToRadians (55.0) / 2;
    const double c = std::cos (half), s = std::sin (half);
    rot::RotationMatrix RqZ, RqY, RqX, RyprZ, RyprY, RyprX;
    rot::buildFromQuaternion ({ c, 0, 0, s }, RqZ);
    rot::buildFromQuaternion ({ c, 0, s, 0 }, RqY);
    rot::buildFromQuaternion ({ c, s, 0, 0 }, RqX);
    rot::buildFromYawPitchRoll (55, 0, 0, RyprZ);
    rot::buildFromYawPitchRoll (0, 55, 0, RyprY);
    rot::buildFromYawPitchRoll (0, 0, 55, RyprX);
    CHECK (rmaxDiff (RqZ.coeffs, RyprZ.coeffs, rot::kNumRotationCoeffs) < 1e-12);
    CHECK (rmaxDiff (RqY.coeffs, RyprY.coeffs, rot::kNumRotationCoeffs) < 1e-12);
    CHECK (rmaxDiff (RqX.coeffs, RyprX.coeffs, rot::kNumRotationCoeffs) < 1e-12);
}

//==============================================================================
// R8 - mirror
//==============================================================================
static void testMirror()
{
    juce::Random r (0x5117e12d);
    const int order = xoa::kAmbisonicOrder;

    // (a) property: mirror::apply(P, encode(d)) == encode(d_P)
    for (int t = 0; t < 8; ++t)
    {
        const auto d = randomDirection (r);
        double enc[xoa::kNumSHChannels], mirrored[xoa::kNumSHChannels], encMir[xoa::kNumSHChannels];
        sh::evaluate (d, order, enc);

        struct Case { mir::Plane plane; xoa::coords::Cartesian dp; };
        const Case cases[] = {
            { mir::Plane::leftRight, { d.x, -d.y, d.z } },
            { mir::Plane::frontBack, { -d.x, d.y, d.z } },
            { mir::Plane::upDown,    { d.x, d.y, -d.z } },
        };
        for (const auto& c : cases)
        {
            mir::apply (c.plane, order, enc, mirrored);
            sh::evaluate (c.dp, order, encMir);
            CHECK (rmaxDiff (mirrored, encMir, xoa::kNumSHChannels) < 1e-13);
        }
    }

    // (b) involution: apply twice == original; signs^2 == 1
    {
        double v[xoa::kNumSHChannels], once[xoa::kNumSHChannels], twice[xoa::kNumSHChannels];
        for (int i = 0; i < xoa::kNumSHChannels; ++i) v[i] = 0.4 * i - 7.0;
        for (auto plane : { mir::Plane::leftRight, mir::Plane::frontBack, mir::Plane::upDown })
        {
            mir::apply (plane, order, v, once);
            mir::apply (plane, order, once, twice);
            CHECK (rmaxDiff (v, twice, xoa::kNumSHChannels) == 0.0);
        }
    }

    // (c) cross-checks: leftRight∘frontBack == 180deg yaw; triple == antipode
    {
        const auto d = randomDirection (r);
        double enc[xoa::kNumSHChannels], lr[xoa::kNumSHChannels], lrfb[xoa::kNumSHChannels],
               yaw180[xoa::kNumSHChannels];
        sh::evaluate (d, order, enc);
        mir::apply (mir::Plane::frontBack, order, enc, lr);
        mir::apply (mir::Plane::leftRight, order, lr, lrfb);
        rot::RotationMatrix R180;
        rot::buildFromYawPitchRoll (180, 0, 0, R180);
        rot::apply (R180, order, enc, yaw180);
        CHECK (rmaxDiff (lrfb, yaw180, xoa::kNumSHChannels) < 1e-12);

        // upDown∘leftRight∘frontBack == encode(-d)  (antipode)
        double triple[xoa::kNumSHChannels], encNeg[xoa::kNumSHChannels];
        mir::apply (mir::Plane::upDown, order, lrfb, triple);
        sh::evaluate (xoa::coords::Cartesian { -d.x, -d.y, -d.z }, order, encNeg);
        CHECK (rmaxDiff (triple, encNeg, xoa::kNumSHChannels) < 1e-13);
    }

    // (d) in == out aliasing
    {
        double v[xoa::kNumSHChannels], ref[xoa::kNumSHChannels];
        for (int i = 0; i < xoa::kNumSHChannels; ++i) v[i] = std::sin (0.9 * i);
        mir::apply (mir::Plane::frontBack, order, v, ref);
        mir::apply (mir::Plane::frontBack, order, v, v);   // alias
        CHECK (rmaxDiff (v, ref, xoa::kNumSHChannels) == 0.0);
    }
}

//==============================================================================
// R9 - guards / utilities
//==============================================================================
static void testGuards()
{
    juce::Random r (0xa11ce5ed);
    const auto q = randomUnitQuat (r);
    rot::RotationMatrix R;
    rot::buildFromQuaternion (q, R);

    // W channel invariant: l=0 block == 1, apply leaves channel 0 exact
    CHECK (R.coeffs[0] == 1.0);
    double v[xoa::kNumSHChannels], out[xoa::kNumSHChannels];
    for (int i = 0; i < xoa::kNumSHChannels; ++i) v[i] = 0.11 * i - 4.0;
    rot::apply (R, xoa::kAmbisonicOrder, v, out);
    CHECK (out[0] == v[0]);

    // in == out apply == separate-buffer apply
    double aliased[xoa::kNumSHChannels];
    for (int i = 0; i < xoa::kNumSHChannels; ++i) aliased[i] = v[i];
    rot::apply (R, xoa::kAmbisonicOrder, aliased, aliased);
    CHECK (rmaxDiff (aliased, out, xoa::kNumSHChannels) < 1e-15);

    // lower-order apply touches only numChannels(order) outputs
    double canary[xoa::kNumSHChannels];
    for (double& c : canary) c = 123.5;
    rot::apply (R, 3, v, canary);
    for (int c = sh::numChannels (3); c < xoa::kNumSHChannels; ++c)
        CHECK (canary[c] == 123.5);

    // release-safe order guards no-op against a canary buffer
    double guard[xoa::kNumSHChannels];
    for (double& c : guard) c = 77.0;
    rot::apply (R, 11, v, guard);
    for (double c : guard) CHECK (c == 77.0);
    double mguard[xoa::kNumSHChannels];
    for (double& c : mguard) c = 88.0;
    mir::apply (mir::Plane::upDown, -1, v, mguard);
    for (double c : mguard) CHECK (c == 88.0);
}

//==============================================================================
// WP9 C6: matrixToYawPitchRoll is the inverse of yawPitchRollToMatrix - the
// decomposition head-tracking uses to turn a quaternion into the rotation
// triple. Round-trip over random rotations (and the gimbal poles, where yaw
// and roll are degenerate but the reconstructed rotation must still match).
static double mat3MaxDiff (const rot::Mat3& a, const rot::Mat3& b) noexcept
{
    double d = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            d = std::max (d, std::abs (a.m[i][j] - b.m[i][j]));
    return d;
}

static void testEulerDecomposition()
{
    juce::Random r (0x5eed);
    for (int t = 0; t < 2000; ++t)
    {
        const auto M = rot::quaternionToMatrix (randomUnitQuat (r));
        double yaw, pitch, roll;
        rot::matrixToYawPitchRoll (M, yaw, pitch, roll);
        CHECK (pitch >= -90.0 - 1e-9 && pitch <= 90.0 + 1e-9);
        CHECK (mat3MaxDiff (M, rot::yawPitchRollToMatrix (yaw, pitch, roll)) < 1e-12);
    }

    for (double poleYaw : { -120.0, 0.0, 75.0 })
        for (double poleRoll : { -40.0, 0.0, 55.0 })
            for (double pitch : { 90.0, -90.0 })
            {
                const auto M = rot::yawPitchRollToMatrix (poleYaw, pitch, poleRoll);
                double y, p, ro;
                rot::matrixToYawPitchRoll (M, y, p, ro);
                CHECK (mat3MaxDiff (M, rot::yawPitchRollToMatrix (y, p, ro)) < 1e-12);
            }
}

void runXoaRotationTests()
{
    testStorage();
    testRotationProperty();
    testGoldenMatrices();
    testComposition();
    testOrthogonality();
    testYawPitchRoll();
    testQuaternion();
    testEulerDecomposition();
    testMirror();
    testGuards();
}
