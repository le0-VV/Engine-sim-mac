#ifndef ATG_ENGINE_SIM_AUDIO_BUFFER_H
#define ATG_ENGINE_SIM_AUDIO_BUFFER_H

#include "scs.h"

#include <stdint.h>
#include <cstring>

class AudioBuffer {
    public:
        AudioBuffer();
        ~AudioBuffer();

        void initialize(int sampleRate, int bufferSize);
        void destroy();

        inline double offsetToTime(int offset) const {
            return offset * m_offsetToSeconds;
        }

        inline double timeDelta(int offset0, int offset1) const {
            if (m_bufferSize <= 0) return 0;

            if (offset1 == offset0) return 0;
            else if (offset1 < offset0) {
                return offsetToTime((m_bufferSize - offset0) + offset1);
            }
            else {
                return offsetToTime(offset1 - offset0);
            }
        }

        inline int offsetDelta(int offset0, int offset1) const {
            if (m_bufferSize <= 0) return 0;

            if (offset1 == offset0) return 0;
            else if (offset1 < offset0) {
                return (m_bufferSize - offset0) + offset1;
            }
            else {
                return offset1 - offset0;
            }
        }

        inline void writeSample(int16_t sample, int offset, int index = 0) {
            if (m_samples == nullptr || m_bufferSize <= 0) return;
            m_samples[getBufferIndex(offset, index)] = sample;
        }

        inline int16_t readSample(int offset, int index) const {
            if (m_samples == nullptr || m_bufferSize <= 0) return 0;
            return m_samples[getBufferIndex(offset, index)];
        }

        inline void commitBlock(int length) {
            if (m_bufferSize <= 0) return;
            m_writePointer = getBufferIndex(m_writePointer, length);
        }

        inline int getBufferIndex(int offset, int index = 0) const {
            if (m_bufferSize <= 0) return 0;
            return (((offset + index) % m_bufferSize) + m_bufferSize) % m_bufferSize;
        }

        inline void copyBuffer(int16_t *dest, int offset, int length) {
            if (dest == nullptr || m_samples == nullptr || m_bufferSize <= 0 || length <= 0) {
                return;
            }

            const int copyLength = (length > m_bufferSize) ? m_bufferSize : length;
            const int start = getBufferIndex(offset, 0);
            const int firstSpan = copyLength < (m_bufferSize - start)
                ? copyLength
                : m_bufferSize - start;
            memcpy(dest, m_samples + start, (size_t)firstSpan * sizeof(int16_t));

            const int remaining = copyLength - firstSpan;
            if (remaining > 0) {
                memcpy(dest + firstSpan, m_samples, (size_t)remaining * sizeof(int16_t));
            }
        }

        bool checkForDiscontinuitiy(int threshold) const;

        int m_writePointer;

    protected:
        int m_sampleRate;
        int16_t *m_samples;
        int m_bufferSize;

        double m_offsetToSeconds;
};

#endif /* ATG_ENGINE_SIM_AUDIO_BUFFER_H */
