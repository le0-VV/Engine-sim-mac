#include "../include/jitter_filter.h"

#include <cstring>

JitterFilter::JitterFilter() {
    m_history = nullptr;
    m_maxJitter = 0;
    m_offset = 0;
    m_jitterScale = 0.0f;
}

JitterFilter::~JitterFilter() {
    destroy();
}

void JitterFilter::initialize(
    int maxJitter,
    float cutoffFrequency,
    float audioFrequency)
{
    destroy();

    if (maxJitter <= 0) {
        m_maxJitter = 0;
        m_offset = 0;
        return;
    }

    m_maxJitter = maxJitter;

    m_history = new float[maxJitter];
    m_offset = 0;
    std::memset(m_history, 0, sizeof(float) * (size_t)maxJitter);

    if (audioFrequency > 0.0f && cutoffFrequency > 0.0f) {
        m_noiseFilter.setCutoffFrequency(cutoffFrequency, audioFrequency);
    }
}

float JitterFilter::f(float sample) {
    return fast_f(sample);
}

void JitterFilter::destroy() {
    delete[] m_history;

    m_history = nullptr;
    m_maxJitter = 0;
    m_offset = 0;
}
