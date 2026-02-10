#include "../include/gaussian_filter.h"

#include <algorithm>
#include <cmath>

GaussianFilter::GaussianFilter() {
    m_cache = nullptr;

    m_cacheSteps = 0;
    m_radius = 0.0;
    m_alpha = 0.0;

    m_exp_s = 0.0;
    m_inv_r = 0.0;
}

GaussianFilter::~GaussianFilter() {
    if (m_cache != nullptr) delete[] m_cache;
}

void GaussianFilter::initialize(double alpha, double radius, int cacheSteps) {
    delete[] m_cache;
    m_cache = nullptr;

    m_cacheSteps = std::max(cacheSteps, 33);

    m_alpha = std::max(alpha, 0.0);
    m_radius = std::max(radius, 1e-9);

    m_exp_s = std::exp(-m_alpha * m_radius * m_radius);
    m_inv_r = 1 / m_radius;

    generateCache();
}

double GaussianFilter::evaluate(double s) const {
    if (m_cache == nullptr || m_cacheSteps <= 32) {
        return calculate(s);
    }

    const int actualSteps = m_cacheSteps - 32;
    const double s_sample = actualSteps * std::abs(s) * m_inv_r;
    const int i0 = std::clamp((int)std::floor(s_sample), 0, actualSteps);
    const int i1 = std::clamp((int)std::ceil(s_sample), 0, actualSteps);
    const double d = s_sample - i0;

    return
        (1 - d) * m_cache[i0]
        + d * m_cache[i1];
}

double GaussianFilter::calculate(double s) const {
    return std::max(
            0.0,
            std::exp(-m_alpha * s * s) - m_exp_s);
}

void GaussianFilter::generateCache() {
    const int actualSteps = m_cacheSteps - 32;
    const double step = 1.0 / actualSteps;

    delete[] m_cache;
    m_cache = new double[m_cacheSteps];
    for (int i = 0; i <= actualSteps; ++i) {
        const double s = i * step * m_radius;
        m_cache[i] = calculate(s);
    }

    for (int i = actualSteps + 1; i < m_cacheSteps; ++i) {
        m_cache[i] = 0.0;
    }
}
