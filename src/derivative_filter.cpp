#include "../include/derivative_filter.h"

#include <cmath>

DerivativeFilter::DerivativeFilter() {
    m_previous = 0;
    m_dt = 0;
}

DerivativeFilter::~DerivativeFilter() {
    /* void */
}

float DerivativeFilter::f(float sample) {
    const float temp = m_previous;
    m_previous = sample;

    if (std::abs(m_dt) <= 1e-12f) {
        return 0.0f;
    }

    return (sample - temp) / m_dt;
}
