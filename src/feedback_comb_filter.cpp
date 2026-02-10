#include "../include/feedback_comb_filter.h"

#include <assert.h>
#include <cstring>

FeedbackCombFilter::FeedbackCombFilter() {
    M = 0;
    a_M = 1.0;
    m_y = nullptr;
    m_offset = 0;
}

FeedbackCombFilter::~FeedbackCombFilter() {
    assert(m_y == nullptr);
}

void FeedbackCombFilter::initialize(int M) {
    destroy();

    if (M <= 0) {
        this->M = 0;
        m_offset = 0;
        return;
    }

    this->M = M;
    m_y = new float[M];
    m_offset = 0;
    std::memset(m_y, 0, sizeof(float) * (size_t)M);
}

float FeedbackCombFilter::f(float sample) {
    if (m_y == nullptr || M <= 0) {
        return sample;
    }

    const float y_n_min_M = m_y[m_offset];

    const float y_n = sample + a_M * y_n_min_M;

    m_y[m_offset] = y_n;
    m_offset = (m_offset + 1) % M;

    return y_n;
}

void FeedbackCombFilter::destroy() {
    delete[] m_y;

    m_y = nullptr;
    M = 0;
    m_offset = 0;
}
