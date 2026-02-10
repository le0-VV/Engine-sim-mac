#include "../include/synthesizer.h"

#include "../include/utilities.h"
#include "../include/delta.h"
#include "../include/debug_trace.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

#undef min
#undef max

Synthesizer::Synthesizer() {
    m_inputChannels = nullptr;
    m_inputChannelCount = 0;
    m_inputBufferSize = 0;
    m_inputWriteOffset = 0.0;
    m_inputSamplesRead = 0;

    m_audioBufferSize = 0;

    m_inputSampleRate = 0.0;
    m_audioSampleRate = 0.0;

    m_lastInputSampleOffset = 0.0;

    m_run = true;
    m_thread = nullptr;
    m_filters = nullptr;
}

Synthesizer::~Synthesizer() {
    assert(m_inputChannels == nullptr);
    assert(m_thread == nullptr);
    assert(m_filters == nullptr);
}

void Synthesizer::initialize(const Parameters &p) {
    m_inputChannelCount = std::max(0, p.inputChannelCount);
    m_inputBufferSize = std::max(1, p.inputBufferSize);
    m_inputWriteOffset = m_inputBufferSize;
    m_audioBufferSize = std::max(1, p.audioBufferSize);
    m_inputSampleRate = (p.inputSampleRate > 0.0f) ? p.inputSampleRate : 1.0f;
    m_audioSampleRate = (p.audioSampleRate > 0.0f) ? p.audioSampleRate : 1.0f;
    m_audioParameters = p.initialAudioParameters;

    m_inputSamplesRead = 0;

    m_inputWriteOffset = 0;
    m_processed = true;

    m_audioBuffer.initialize((size_t)m_audioBufferSize);
    m_inputChannels = new InputChannel[m_inputChannelCount];
    for (int i = 0; i < m_inputChannelCount; ++i) {
        m_inputChannels[i].transferBuffer = new float[m_inputBufferSize];
        m_inputChannels[i].data.initialize((size_t)m_inputBufferSize);
    }

    m_filters = new ProcessingFilters[m_inputChannelCount];
    for (int i = 0; i < m_inputChannelCount; ++i) {
        m_filters[i].airNoiseLowPass.setCutoffFrequency(
            m_audioParameters.airNoiseFrequencyCutoff, m_audioSampleRate);

        m_filters[i].derivative.m_dt = 1 / m_audioSampleRate;

        m_filters[i].inputDcFilter.setCutoffFrequency(10.0);
        m_filters[i].inputDcFilter.m_dt = 1 / m_audioSampleRate;

        m_filters[i].jitterFilter.initialize(
            10,
            m_audioParameters.inputSampleNoiseFrequencyCutoff,
            m_audioSampleRate);

        m_filters[i].antialiasing.setCutoffFrequency(1900.0f, m_audioSampleRate);

        // Default to a safe identity convolution until an impulse response is loaded.
        m_filters[i].convolution.initialize(1);
        m_filters[i].convolution.getImpulseResponse()[0] = 1.0f;
    }

    m_levelingFilter.p_target = m_audioParameters.levelerTarget;
    m_levelingFilter.p_maxLevel = m_audioParameters.levelerMaxGain;
    m_levelingFilter.p_minLevel = m_audioParameters.levelerMinGain;
    m_antialiasing.setCutoffFrequency(m_audioSampleRate * 0.45f, m_audioSampleRate);

    for (int i = 0; i < m_audioBufferSize; ++i) {
        m_audioBuffer.write(0);
    }
}

void Synthesizer::initializeImpulseResponse(
    const int16_t *impulseResponse,
    unsigned int samples,
    float volume,
    int index)
{
    if (index < 0 || index >= m_inputChannelCount || m_filters == nullptr) {
        return;
    }

    if (impulseResponse == nullptr || samples == 0) {
        m_filters[index].convolution.initialize(1);
        m_filters[index].convolution.getImpulseResponse()[0] = 1.0f;
        return;
    }

    unsigned int clippedLength = 0;
    for (unsigned int i = 0; i < samples; ++i) {
        if (std::abs(impulseResponse[i]) > 100) {
            clippedLength = i + 1;
        }
    }

    unsigned int sampleCount = std::min(10000U, clippedLength);
    if (sampleCount == 0) sampleCount = 1;
    m_filters[index].convolution.initialize(sampleCount);
    for (unsigned int i = 0; i < sampleCount; ++i) {
        if (i < clippedLength) {
            m_filters[index].convolution.getImpulseResponse()[i] =
                volume * impulseResponse[i] / INT16_MAX;
        }
        else {
            m_filters[index].convolution.getImpulseResponse()[i] = (i == 0) ? 1.0f : 0.0f;
        }
    }
}

void Synthesizer::startAudioRenderingThread() {
    DebugTrace::Log("audio_thread", "startAudioRenderingThread requested");
    m_run = true;
    m_thread = new std::thread(&Synthesizer::audioRenderingThread, this);
}

void Synthesizer::endAudioRenderingThread() {
    if (m_thread != nullptr) {
        DebugTrace::Log("audio_thread", "endAudioRenderingThread begin");
        m_run = false;
        endInputBlock();

        m_thread->join();
        delete m_thread;

        m_thread = nullptr;
        DebugTrace::Log("audio_thread", "endAudioRenderingThread complete");
    }
}

void Synthesizer::destroy() {
    m_audioBuffer.destroy();

    for (int i = 0; i < m_inputChannelCount; ++i) {
        delete[] m_inputChannels[i].transferBuffer;
        m_inputChannels[i].transferBuffer = nullptr;
        m_inputChannels[i].data.destroy();
        m_filters[i].jitterFilter.destroy();
        m_filters[i].convolution.destroy();
    }

    delete[] m_inputChannels;
    delete[] m_filters;

    m_inputChannels = nullptr;
    m_filters = nullptr;

    m_inputChannelCount = 0;
}

int Synthesizer::readAudioOutput(int samples, int16_t *buffer) {
    if (samples <= 0 || buffer == nullptr) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_lock0);

    const int newDataLength = m_audioBuffer.size();
    if (newDataLength >= samples) {
        m_audioBuffer.readAndRemove(samples, buffer);
    }
    else {
        m_audioBuffer.readAndRemove(newDataLength, buffer);
        memset(
            buffer + newDataLength,
            0,
            sizeof(int16_t) * ((size_t)samples - newDataLength));
    }
    
    const int samplesConsumed = std::min(samples, newDataLength);

    return samplesConsumed;
}

void Synthesizer::waitProcessed() {
    {
        std::unique_lock<std::mutex> lk(m_lock0);
        m_cv0.wait(lk, [this] { return m_processed; });
    }
}

void Synthesizer::writeInput(const double *data) {
    if (data == nullptr || m_inputChannelCount <= 0 || m_inputChannels == nullptr || m_filters == nullptr) {
        return;
    }

    if (m_inputSampleRate <= 0 || m_inputBufferSize <= 0) {
        return;
    }

    m_inputWriteOffset += (double)m_audioSampleRate / m_inputSampleRate;
    if (m_inputWriteOffset >= (double)m_inputBufferSize) {
        m_inputWriteOffset -= (double)m_inputBufferSize;
    }

    for (int i = 0; i < m_inputChannelCount; ++i) {
        RingBuffer<float> &buffer = m_inputChannels[i].data;
        const double lastInputSample = m_inputChannels[i].lastInputSample;
        const size_t baseIndex = buffer.writeIndex();
        const double distance =
            inputDistance(m_inputWriteOffset, m_lastInputSampleOffset);
        if (distance <= 1e-12) {
            m_inputChannels[i].lastInputSample = data[i];
            continue;
        }

        double s =
            inputDistance(baseIndex, m_lastInputSampleOffset);
        for (; s <= distance; s += 1.0) {
            if (s >= m_inputBufferSize) s -= m_inputBufferSize;

            const double f = s / distance;
            const double sample = lastInputSample * (1 - f) + data[i] * f;

            buffer.write(m_filters[i].antialiasing.fast_f(static_cast<float>(sample)));
        }

        m_inputChannels[i].lastInputSample = data[i];
    }

    m_lastInputSampleOffset = m_inputWriteOffset;
}

void Synthesizer::endInputBlock() {
    std::unique_lock<std::mutex> lk(m_inputLock); 

    for (int i = 0; i < m_inputChannelCount; ++i) {
        m_inputChannels[i].data.removeBeginning(m_inputSamplesRead);
    }

    if (m_inputChannelCount != 0) {
        m_latency = m_inputChannels[0].data.size();
    }
    
    m_inputSamplesRead = 0;
    m_processed = false;

    lk.unlock();
    m_cv0.notify_one();
}

void Synthesizer::audioRenderingThread() {
    DebugTrace::Log("audio_thread", "audioRenderingThread started");
    auto nextHeartbeat = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    int cyclesSinceHeartbeat = 0;
    int underrunCount = 0;
    int overrunCount = 0;
    long long totalCycleMicros = 0;

    while (m_run) {
        const auto cycleStart = std::chrono::steady_clock::now();
        renderAudio();
        ++cyclesSinceHeartbeat;
        const auto cycleEnd = std::chrono::steady_clock::now();
        totalCycleMicros += std::chrono::duration_cast<std::chrono::microseconds>(cycleEnd - cycleStart).count();

        if (m_inputChannelCount > 0 && m_inputChannels != nullptr) {
            if (m_inputChannels[0].data.size() <= 0) {
                ++underrunCount;
            }
            else if (m_inputChannels[0].data.size() > (size_t)(m_inputBufferSize * 3 / 4)) {
                ++overrunCount;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextHeartbeat) {
            const double avgCycleMicros =
                (cyclesSinceHeartbeat > 0)
                ? static_cast<double>(totalCycleMicros) / cyclesSinceHeartbeat
                : 0.0;
            DebugTrace::Log(
                "audio_thread",
                "heartbeat cycles=%d input_channels=%d input_buffer=%d audio_buffer=%d latency=%.6f processed=%d avg_cycle_us=%.2f underrun=%d overrun=%d",
                cyclesSinceHeartbeat,
                m_inputChannelCount,
                (m_inputChannelCount > 0 && m_inputChannels != nullptr) ? static_cast<int>(m_inputChannels[0].data.size()) : 0,
                static_cast<int>(m_audioBuffer.size()),
                getLatency(),
                m_processed ? 1 : 0,
                avgCycleMicros,
                underrunCount,
                overrunCount);
            cyclesSinceHeartbeat = 0;
            totalCycleMicros = 0;
            underrunCount = 0;
            overrunCount = 0;
            nextHeartbeat = now + std::chrono::seconds(1);
        }
    }

    DebugTrace::Log("audio_thread", "audioRenderingThread exiting");
}

#undef max
void Synthesizer::renderAudio() {
    std::unique_lock<std::mutex> lk0(m_lock0);

    m_cv0.wait(lk0, [this] {
        const bool hasInputChannel = m_inputChannelCount > 0 && m_inputChannels != nullptr;
        const bool inputAvailable =
            hasInputChannel
            && m_inputChannels[0].data.size() > 0
            && m_audioBuffer.size() < 2000;
        return !m_run || (inputAvailable && !m_processed);
    });

    if (!m_run) {
        return;
    }

    if (m_inputChannelCount <= 0 || m_inputChannels == nullptr || m_filters == nullptr) {
        m_processed = true;
        lk0.unlock();
        m_cv0.notify_one();
        return;
    }

    const int n = std::min(
        std::max(0, 2000 - (int)m_audioBuffer.size()),
        (int)m_inputChannels[0].data.size());

    for (int i = 0; i < m_inputChannelCount; ++i) {
        m_inputChannels[i].data.read(n, m_inputChannels[i].transferBuffer);
    }
    
    m_inputSamplesRead = n;
    m_processed = true;

    lk0.unlock();

    for (int i = 0; i < m_inputChannelCount; ++i) {
        m_filters[i].airNoiseLowPass.setCutoffFrequency(
            static_cast<float>(m_audioParameters.airNoiseFrequencyCutoff), m_audioSampleRate);
        m_filters[i].jitterFilter.setJitterScale(m_audioParameters.inputSampleNoise);
    }

    for (int i = 0; i < n; ++i) {
        m_audioBuffer.write(renderAudio(i));
    }

    m_cv0.notify_one();
}

double Synthesizer::getLatency() const {
    if (m_audioSampleRate <= 0) {
        return 0.0;
    }

    return (double)m_latency / m_audioSampleRate;
}

int Synthesizer::inputDelta(int s1, int s0) const {
    return (s1 < s0)
        ? m_inputBufferSize - s0 + s1
        : s1 - s0;
}

double Synthesizer::inputDistance(double s1, double s0) const {
    return (s1 < s0)
        ? (double)m_inputBufferSize - s0 + s1
        : s1 - s0;
}

void Synthesizer::setInputSampleRate(double sampleRate) {
    if (sampleRate <= 0) {
        return;
    }

    if (sampleRate != m_inputSampleRate) {
        std::lock_guard<std::mutex> lock(m_lock0);
        m_inputSampleRate = sampleRate;
    }
}

int16_t Synthesizer::renderAudio(int inputSample) {
    if (m_inputChannelCount <= 0 || m_inputChannels == nullptr || m_filters == nullptr) {
        return 0;
    }

    const float airNoise = m_audioParameters.airNoise;
    const float dF_F_mix = m_audioParameters.dF_F_mix;
    const float convAmount = m_audioParameters.convolution;

    float signal = 0;
    for (int i = 0; i < m_inputChannelCount; ++i) {
        const float r_0 = 2.0 * ((double)rand() / RAND_MAX) - 1.0;

        const float jitteredSample =
            m_filters[i].jitterFilter.fast_f(m_inputChannels[i].transferBuffer[inputSample]);

        const float f_in = jitteredSample;
        const float f_dc = m_filters[i].inputDcFilter.fast_f(f_in);
        const float f = f_in - f_dc;
        const float f_p = m_filters[i].derivative.f(f_in);

        const float noise = 2.0 * ((double)rand() / RAND_MAX) - 1.0;
        const float r =
            m_filters[i].airNoiseLowPass.fast_f(noise);
        const float r_mixed =
            airNoise * r + (1 - airNoise);

        float v_in =
            f_p * dF_F_mix
            + f * r_mixed * (1 - dF_F_mix);
        if (std::fpclassify(v_in) == FP_SUBNORMAL) {
            v_in = 0;
        }

        const float v =
            convAmount * m_filters[i].convolution.f(v_in)
            + (1 - convAmount) * v_in;

        signal += v;
    }

    signal = m_antialiasing.fast_f(signal);

    m_levelingFilter.p_target = m_audioParameters.levelerTarget;
    const float v_leveled = m_levelingFilter.f(signal) * m_audioParameters.volume;
    int r_int = std::lround(v_leveled);
    if (r_int > INT16_MAX) {
        r_int = INT16_MAX;
    }
    else if (r_int < INT16_MIN) {
        r_int = INT16_MIN;
    }

    return static_cast<int16_t>(r_int);
}

double Synthesizer::getLevelerGain() {
    std::lock_guard<std::mutex> lock(m_lock0);
    return m_levelingFilter.getAttenuation();
}

Synthesizer::AudioParameters Synthesizer::getAudioParameters() {
    std::lock_guard<std::mutex> lock(m_lock0);
    return m_audioParameters;
}

void Synthesizer::setAudioParameters(const AudioParameters &params) {
    std::lock_guard<std::mutex> lock(m_lock0);
    m_audioParameters = params;
}
