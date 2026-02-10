#include "../include/convolution_filter.h"

#include <assert.h>
#include <cstring>

ConvolutionFilter::ConvolutionFilter() {
    m_shiftRegister = nullptr;
    m_impulseResponse = nullptr;

    m_shiftOffset = 0;
    m_sampleCount = 0;
}

ConvolutionFilter::~ConvolutionFilter() {
    assert(m_shiftRegister == nullptr);
    assert(m_impulseResponse == nullptr);
}

void ConvolutionFilter::initialize(int samples) {
    destroy();

    if (samples <= 0) {
        m_sampleCount = 0;
        m_shiftOffset = 0;
        return;
    }

    m_sampleCount = samples;
    m_shiftOffset = 0;
    m_shiftRegister = new float[samples];
    m_impulseResponse = new float[samples];

    std::memset(m_shiftRegister, 0, sizeof(float) * (size_t)samples);
    std::memset(m_impulseResponse, 0, sizeof(float) * (size_t)samples);
}

void ConvolutionFilter::destroy() {
    delete[] m_shiftRegister;
    delete[] m_impulseResponse;

    m_shiftRegister = nullptr;
    m_impulseResponse = nullptr;
    m_shiftOffset = 0;
    m_sampleCount = 0;
}

float ConvolutionFilter::f(float sample) {
    if (m_shiftRegister == nullptr || m_impulseResponse == nullptr || m_sampleCount <= 0) {
        return sample;
    }
    m_shiftRegister[m_shiftOffset] = sample;

    float result = 0;
    for (int i = 0; i < m_sampleCount - m_shiftOffset; ++i) {
        result += m_impulseResponse[i] * m_shiftRegister[i + m_shiftOffset];
    }

    for (int i = m_sampleCount - m_shiftOffset; i < m_sampleCount; ++i) {
        result += m_impulseResponse[i] * m_shiftRegister[i - (m_sampleCount - m_shiftOffset)];
    }

    m_shiftOffset = (m_shiftOffset - 1 + m_sampleCount) % m_sampleCount;

    return result;
}
