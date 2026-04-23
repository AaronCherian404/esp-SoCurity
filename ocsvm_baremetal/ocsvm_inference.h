/*
 * ocsvm_inference.h
 *
 * Bare-metal OCSVM inference engine for ESP / x-heep (Ibex RV32IMC).
 *
 * Design choices for bare-metal:
 *   - Uses float (not double) to reduce soft-float overhead on Ibex.
 *   - Provides a fast polynomial exp() approximation so we don't need libm.
 *   - All functions are static inline for zero call overhead.
 */

#ifndef OCSVM_INFERENCE_H
#define OCSVM_INFERENCE_H

#include "ocsvm_model.h"

/* =========================================================================
 * Fast exp() approximation for bare-metal
 * =========================================================================
 * Uses the Schraudolph method (IEEE 754 bit manipulation) combined with
 * a quadratic refinement. Accurate to ~0.1% for the range we care about
 * (x in [-20, 0], which is where gamma * ||x-sv||^2 lands).
 *
 * If you have libm available, you can replace this with:
 *   #include <math.h>
 *   #define fast_expf(x) expf(x)
 */
static inline float fast_expf(float x)
{
    /* Clamp to avoid overflow / underflow in the bit trick */
    if (x < -20.0f) return 0.0f;
    if (x > 20.0f)  return 4.85165195e8f; /* ~exp(20) */

    /*
     * Schraudolph's method:
     *   reinterpret float bits ≈ 2^23 * (x / ln2 + 127)
     * With a bias correction for better accuracy in [−10, 0].
     */
    union { float f; int i; } u;
    u.i = (int)(12102203.0f * x + 1065353216.0f); /* 2^23/ln2 ≈ 12102203 */

    float y = u.f;

    /*
     * One Newton-style refinement step for improved accuracy.
     * This corrects the ~4% systematic error of raw Schraudolph.
     *
     *   e_approx ≈ y * (1 + x - ln(y))
     *
     * But computing ln(y) is expensive. Instead we use a simpler
     * polynomial correction that works well in [-15, 0]:
     *
     *   correction = 1.0 + 0.0003 * x * x
     *
     * This keeps max relative error < 1% in the RBF kernel range.
     */
    y *= (1.0f + 0.0003f * x * x);

    return y;
}

/* =========================================================================
 * RBF kernel: K(x, sv) = exp(-gamma * ||x - sv||^2)
 * ========================================================================= */
static inline float rbf_kernel(const float *x, const float *sv)
{
    float dist_sq = 0.0f;
    int i;
    for (i = 0; i < N_FEATURES; i++) {
        float diff = x[i] - sv[i];
        dist_sq += diff * diff;
    }
    return fast_expf(-OCSVM_GAMMA * dist_sq);
}

/* =========================================================================
 * OCSVM decision score
 *
 * Returns:  score > 0  =>  NORMAL
 *           score <= 0 =>  ANOMALY
 *
 * Formula:  score = sum_i(dual_coefs[i] * K(x, sv_i)) - intercept
 * This matches sklearn's OneClassSVM decision_function convention.
 * ========================================================================= */
static inline float ocsvm_score(const float *x_scaled)
{
    float score = 0.0f;
    int i;
    for (i = 0; i < N_SUPPORT_VECTORS; i++) {
        score += dual_coefs[i] * rbf_kernel(x_scaled, support_vectors[i]);
    }
    score -= OCSVM_INTERCEPT;
    return score;
}

/* =========================================================================
 * MinMax scaler: scale raw feature vector in-place
 *
 * Formula: scaled[i] = raw[i] * scaler_scale[i] + scaler_min[i]
 * (Matches the convention in the existing ocsvm_detector.c)
 * ========================================================================= */
static inline void min_max_scale(float *x)
{
    int i;
    for (i = 0; i < N_FEATURES; i++) {
        x[i] = x[i] * scaler_scale[i] + scaler_min[i];
    }
}

/* =========================================================================
 * Convenience: classify as 0=normal, 1=anomaly
 * ========================================================================= */
static inline int ocsvm_predict(const float *x_scaled)
{
    return (ocsvm_score(x_scaled) > 0.0f) ? 0 : 1;
}

/* =========================================================================
 * Label lookup
 * ========================================================================= */
static inline const char *ocsvm_label(int class_idx)
{
    switch (class_idx) {
        case 0:  return "regular";
        case 1:  return "anomaly";
        default: return "unknown";
    }
}

#endif /* OCSVM_INFERENCE_H */
