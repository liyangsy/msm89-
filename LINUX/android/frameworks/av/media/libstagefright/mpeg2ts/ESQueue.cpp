/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file was modified by Dolby Laboratories, Inc. The portions of the
 * code that are surrounded by "DOLBY..." are copyrighted and
 * licensed separately, as follows:
 *
 *  (C) 2011-2015 Dolby Laboratories, Inc.
 *  (C) 2015 DTS, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "ESQueue"
#include <media/stagefright/foundation/ADebug.h>

#include "ESQueue.h"

#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>

#include "include/avc_utils.h"

#include <inttypes.h>
#include <netinet/in.h>
#ifdef DOLBY_ENABLE
#include "DolbyESQueueExtImpl.h"
#endif // DOLBY_END
#include <stagefright/AVExtensions.h>

namespace android {

ElementaryStreamQueue::ElementaryStreamQueue(Mode mode, uint32_t flags)
    : mMode(mode),
      mFlags(flags),
      mEOSReached(false) {
#ifdef DOLBY_ENABLE
    independent_streams_processed = 0;
#endif // DOLBY_END
}

sp<MetaData> ElementaryStreamQueue::getFormat() {
    return mFormat;
}

void ElementaryStreamQueue::clear(bool clearFormat) {
    if (mBuffer != NULL) {
        mBuffer->setRange(0, 0);
    }

    mRangeInfos.clear();

    if (clearFormat) {
        mFormat.clear();
    }

    mEOSReached = false;
}

// Parse AC3 header assuming the current ptr is start position of syncframe,
// update metadata only applicable, and return the payload size
static unsigned parseAC3SyncFrame(
        const uint8_t *ptr, size_t size, sp<MetaData> *metaData) {
    static const unsigned channelCountTable[] = {2, 1, 2, 3, 3, 4, 4, 5};
    static const unsigned samplingRateTable[] = {48000, 44100, 32000};

    static const unsigned frameSizeTable[19][3] = {
        { 64, 69, 96 },
        { 80, 87, 120 },
        { 96, 104, 144 },
        { 112, 121, 168 },
        { 128, 139, 192 },
        { 160, 174, 240 },
        { 192, 208, 288 },
        { 224, 243, 336 },
        { 256, 278, 384 },
        { 320, 348, 480 },
        { 384, 417, 576 },
        { 448, 487, 672 },
        { 512, 557, 768 },
        { 640, 696, 960 },
        { 768, 835, 1152 },
        { 896, 975, 1344 },
        { 1024, 1114, 1536 },
        { 1152, 1253, 1728 },
        { 1280, 1393, 1920 },
    };

    ABitReader bits(ptr, size);
    if (bits.numBitsLeft() < 16) {
        return 0;
    }
    if (bits.getBits(16) != 0x0B77) {
        return 0;
    }

    if (bits.numBitsLeft() < 16 + 2 + 6 + 5 + 3 + 3) {
        ALOGV("Not enough bits left for further parsing");
        return 0;
    }
    bits.skipBits(16);  // crc1

    unsigned fscod = bits.getBits(2);
    if (fscod == 3) {
        ALOGW("Incorrect fscod in AC3 header");
        return 0;
    }

    unsigned frmsizecod = bits.getBits(6);
    if (frmsizecod > 37) {
        ALOGW("Incorrect frmsizecod in AC3 header");
        return 0;
    }

    unsigned bsid = bits.getBits(5);
    if (bsid > 8) {
        ALOGW("Incorrect bsid in AC3 header. Possibly E-AC-3?");
        return 0;
    }

    unsigned bsmod __unused = bits.getBits(3);
    unsigned acmod = bits.getBits(3);
    unsigned cmixlev __unused = 0;
    unsigned surmixlev __unused = 0;
    unsigned dsurmod __unused = 0;

    if ((acmod & 1) > 0 && acmod != 1) {
        if (bits.numBitsLeft() < 2) {
            return 0;
        }
        cmixlev = bits.getBits(2);
    }
    if ((acmod & 4) > 0) {
        if (bits.numBitsLeft() < 2) {
            return 0;
        }
        surmixlev = bits.getBits(2);
    }
    if (acmod == 2) {
        if (bits.numBitsLeft() < 2) {
            return 0;
        }
        dsurmod = bits.getBits(2);
    }

    if (bits.numBitsLeft() < 1) {
        return 0;
    }
    unsigned lfeon = bits.getBits(1);

    unsigned samplingRate = samplingRateTable[fscod];
    unsigned payloadSize = frameSizeTable[frmsizecod >> 1][fscod];
    if (fscod == 1) {
        payloadSize += frmsizecod & 1;
    }
    payloadSize <<= 1;  // convert from 16-bit words to bytes

    unsigned channelCount = channelCountTable[acmod] + lfeon;

    if (metaData != NULL) {
        (*metaData)->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AC3);
        (*metaData)->setInt32(kKeyChannelCount, channelCount);
        (*metaData)->setInt32(kKeySampleRate, samplingRate);
    }

    return payloadSize;
}

static bool IsSeeminglyValidAC3Header(const uint8_t *ptr, size_t size) {
    return parseAC3SyncFrame(ptr, size, NULL) > 0;
}

#ifdef DTS_CODEC_M_
// Parse DTS header assuming the current ptr is start position of syncframe,
// update metadata only if applicable, and return the payload size
static unsigned parseDTSSyncFrame(
        const uint8_t *ptr, size_t size, sp<MetaData> *metaData) {

    #define DTS_SYNCWORD_CORE       0x7FFE8001
    #define DTS_SYNCWORD_SUBSTREAM  0x64582025
    #define DTS_SYNCWORD_LBR        0x0a801921
    #define DTS_SYNCWORD_XLL        0x41a29547

    ABitReader bits(ptr, size);
    unsigned syncValue = 0;
    static unsigned dtsChannelCount = 0;
    static unsigned dtsSamplingRate = 0;
    unsigned payloadSize = 0;

    // Check for DTS core or substream sync
    if (bits.numBitsLeft() < 32) {
        return 0;
    }

    // Get the sync value
    syncValue = bits.getBits(32);
    if (!((syncValue == DTS_SYNCWORD_CORE) ||
        (syncValue == DTS_SYNCWORD_SUBSTREAM))){
        return 0;
    }

    if(syncValue == DTS_SYNCWORD_CORE) {
        // Stream with a core
        if (bits.numBitsLeft() < 1 + 5 + 1 + 7 + 14 + 6 + 4) { //FTYPE(1) + SHORT(5) + CRC(1) + NBLKS(7) + FSIZE(14) + AMODE(6) + SFREQ(4)
            ALOGV("DTSHD Not enough bits left for further parsing");
            return 0;
        }
        bits.skipBits(14);  //FTYPE(1) + SHORT(5) + CRC(1) + NBLKS(7)

        // Get frame size
        unsigned frameByteSize = bits.getBits(14) + 1;
        payloadSize = frameByteSize;
        ALOGV(" DTSHD Core Stream found with frame size = %d", frameByteSize);

        // Get channel count
        static const unsigned dtsChannelCountTable[] = { 1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 6, 6, 6, 7, 8, 8 };
        unsigned aMode = bits.getBits(6);
        dtsChannelCount = (aMode < 16) ? dtsChannelCountTable[aMode] : 0;

        // Get sampling freq
        static const unsigned samplingRateTable[] = {   0, 8000, 16000, 32000, 0,
                                                        0, 11025, 22050, 44100, 0,
                                                        0, 12000, 24000, 48000, 0, 0};
        unsigned srIndex = bits.getBits(4);
        dtsSamplingRate = samplingRateTable[srIndex];

    } else {
        // Stream with extension substream
        if (bits.numBitsLeft() < 2 + 2 + 1) { //UserDefinedBits(2) + nExtSSIndex(2) + bHeaderSizeType(1)
            ALOGV("DTSHD Not enough bits left for further parsing");
            return 0;
        }
        bits.skipBits(4); //UserDefinedBits(2) + nExtSSIndex(2)

        unsigned bHeaderSizeType = bits.getBits(1);
        unsigned numBits4Header = 0;
        unsigned numBits4ExSSFsize = 0;

        if(bHeaderSizeType == 0) {
            numBits4Header = 8;
            numBits4ExSSFsize = 16;
        } else {
            numBits4Header = 12;
            numBits4ExSSFsize = 20;
        }

        if(bits.numBitsLeft() < numBits4Header + numBits4ExSSFsize) {
            ALOGV("DTSHD Not enough bits left for further parsing");
            return 0;
        }
        unsigned numExtHeadersize = bits.getBits(numBits4Header) + 1;
        unsigned numExtSSFsize = bits.getBits(numBits4ExSSFsize) + 1;
        unsigned frameByteSize = numExtSSFsize +  numExtHeadersize;
        payloadSize = frameByteSize;
        ALOGV(" DTSHD extension sub stream found with frame size = %d", frameByteSize);
        dtsChannelCount = 8;
        dtsSamplingRate = 48000;
        }

    if (metaData != NULL) {
        (*metaData)->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_DTS);
        (*metaData)->setInt32(kKeyChannelCount, dtsChannelCount);
        (*metaData)->setInt32(kKeySampleRate, dtsSamplingRate);
    }

    return payloadSize;
}

static bool IsSeeminglyValidDTSHeader(const uint8_t *ptr, size_t size) {
    return parseDTSSyncFrame(ptr, size, NULL) > 0;
}
#endif

static bool IsSeeminglyValidADTSHeader(
        const uint8_t *ptr, size_t size, size_t *frameLength) {
    if (size < 7) {
        // Not enough data to verify header.
        return false;
    }

    if (ptr[0] != 0xff || (ptr[1] >> 4) != 0x0f) {
        return false;
    }

    unsigned layer = (ptr[1] >> 1) & 3;

    if (layer != 0) {
        return false;
    }

    unsigned ID = (ptr[1] >> 3) & 1;
    unsigned profile_ObjectType = ptr[2] >> 6;

    if (ID == 1 && profile_ObjectType == 3) {
        // MPEG-2 profile 3 is reserved.
        return false;
    }

    size_t frameLengthInHeader =
            ((ptr[3] & 3) << 11) + (ptr[4] << 3) + ((ptr[5] >> 5) & 7);
    if (frameLengthInHeader > size) {
        return false;
    }

    *frameLength = frameLengthInHeader;
    return true;
}

static bool IsSeeminglyValidMPEGAudioHeader(const uint8_t *ptr, size_t size) {
    if (size < 3) {
        // Not enough data to verify header.
        return false;
    }

    if (ptr[0] != 0xff || (ptr[1] >> 5) != 0x07) {
        return false;
    }

    unsigned ID = (ptr[1] >> 3) & 3;

    if (ID == 1) {
        return false;  // reserved
    }

    unsigned layer = (ptr[1] >> 1) & 3;

    if (layer == 0) {
        return false;  // reserved
    }

    unsigned bitrateIndex = (ptr[2] >> 4);

    if (bitrateIndex == 0x0f) {
        return false;  // reserved
    }

    unsigned samplingRateIndex = (ptr[2] >> 2) & 3;

    if (samplingRateIndex == 3) {
        return false;  // reserved
    }

    return true;
}

status_t ElementaryStreamQueue::appendData(
        const void *data, size_t size, int64_t timeUs) {

    if (mEOSReached) {
        ALOGE("appending data after EOS");
        return ERROR_MALFORMED;
    }
    if (mBuffer == NULL || mBuffer->size() == 0) {
        switch (mMode) {
            case H264:
            case H265:
            case MPEG_VIDEO:
            {
#if 0
                if (size < 4 || memcmp("\x00\x00\x00\x01", data, 4)) {
                    return ERROR_MALFORMED;
                }
#else
                uint8_t *ptr = (uint8_t *)data;

                ssize_t startOffset = -1;
                for (size_t i = 0; i + 2 < size; ++i) {
                    if (!memcmp("\x00\x00\x01", &ptr[i], 3)) {
                        startOffset = i;
                        break;
                    }
                }

                if (startOffset < 0) {
                    return ERROR_MALFORMED;
                }

                if (startOffset > 0) {
                    ALOGI("found something resembling an H.264/MPEG syncword "
                          "at offset %zd",
                          startOffset);
                }

                data = &ptr[startOffset];
                size -= startOffset;
#endif
                break;
            }

            case MPEG4_VIDEO:
            {
#if 0
                if (size < 3 || memcmp("\x00\x00\x01", data, 3)) {
                    return ERROR_MALFORMED;
                }
#else
                uint8_t *ptr = (uint8_t *)data;

                ssize_t startOffset = -1;
                for (size_t i = 0; i + 2 < size; ++i) {
                    if (!memcmp("\x00\x00\x01", &ptr[i], 3)) {
                        startOffset = i;
                        break;
                    }
                }

                if (startOffset < 0) {
                    return ERROR_MALFORMED;
                }

                if (startOffset > 0) {
                    ALOGI("found something resembling an H.264/MPEG syncword "
                          "at offset %zd",
                          startOffset);
                }

                data = &ptr[startOffset];
                size -= startOffset;
#endif
                break;
            }

            case AAC:
            {
                uint8_t *ptr = (uint8_t *)data;

#if 0
                if (size < 2 || ptr[0] != 0xff || (ptr[1] >> 4) != 0x0f) {
                    return ERROR_MALFORMED;
                }
#else
                ssize_t startOffset = -1;
                size_t frameLength;
                for (size_t i = 0; i < size; ++i) {
                    if (IsSeeminglyValidADTSHeader(
                            &ptr[i], size - i, &frameLength)) {
                        startOffset = i;
                        break;
                    }
                }

                if (startOffset < 0) {
                    return ERROR_MALFORMED;
                }

                if (startOffset > 0) {
                    ALOGI("found something resembling an AAC syncword at "
                          "offset %zd",
                          startOffset);
                }

                if (frameLength != size - startOffset) {
                    ALOGV("First ADTS AAC frame length is %zd bytes, "
                          "while the buffer size is %zd bytes.",
                          frameLength, size - startOffset);
                }

                data = &ptr[startOffset];
                size -= startOffset;
#endif
                break;
            }

            case AC3:
            {
                uint8_t *ptr = (uint8_t *)data;

                ssize_t startOffset = -1;
                for (size_t i = 0; i < size; ++i) {
                    if (IsSeeminglyValidAC3Header(&ptr[i], size - i)) {
                        startOffset = i;
                        break;
                    }
                }

                if (startOffset < 0) {
                    return ERROR_MALFORMED;
                }

                if (startOffset > 0) {
                    ALOGI("found something resembling an AC3 syncword at "
                          "offset %zd",
                          startOffset);
                }

                data = &ptr[startOffset];
                size -= startOffset;
                break;
            }
#ifdef DOLBY_ENABLE
            case EAC3:
            {
                uint8_t *ptr = (uint8_t *)data;

                ssize_t startOffset = -1;
                for (size_t i = 0; i < size; ++i) {
                    if (IsSeeminglyValidEAC3AudioHeader(&ptr[i], size - i)) {
                        startOffset = i;
                        break;
                    }
                }

                if (startOffset < 0) {
                    return ERROR_MALFORMED;
                }

                if (startOffset > 0) {
                    ALOGI("found something resembling a DDP audio "
                         "syncword at offset %zd",
                         startOffset);
                }

                data = &ptr[startOffset];
                size -= startOffset;
                break;
            }

#endif // DOLBY_END

            case MPEG_AUDIO:
            {
                uint8_t *ptr = (uint8_t *)data;

                ssize_t startOffset = -1;
                for (size_t i = 0; i < size; ++i) {
                    if (IsSeeminglyValidMPEGAudioHeader(&ptr[i], size - i)) {
                        startOffset = i;
                        break;
                    }
                }

                if (startOffset < 0) {
                    return ERROR_MALFORMED;
                }

                if (startOffset > 0) {
                    ALOGI("found something resembling an MPEG audio "
                          "syncword at offset %zd",
                          startOffset);
                }

                data = &ptr[startOffset];
                size -= startOffset;
                break;
            }

            case PCM_AUDIO:
            case METADATA:
            {
                break;
            }

#ifdef DTS_CODEC_M_
            case DTSHD:
            {
                uint8_t *ptr = (uint8_t *)data;

                ssize_t startOffset = -1;
                for (size_t i = 0; i < size; ++i) {
                    if (IsSeeminglyValidDTSHeader(&ptr[i], size - i)) {
                        startOffset = i;
                        break;
                    }
                }

                if (startOffset < 0) {
                    return ERROR_MALFORMED;
                }

                if (startOffset > 0) {
                    ALOGI("found something resembling an DTS Sync word at "
                          "syncword at offset %zd",
                          startOffset);
                }

                data = &ptr[startOffset];
                size -= startOffset;
                break;
            }
#endif

            default:
                ALOGE("Unknown mode: %d", mMode);
                return ERROR_MALFORMED;
        }
    }

    size_t neededSize = (mBuffer == NULL ? 0 : mBuffer->size()) + size;
    if (mBuffer == NULL || neededSize > mBuffer->capacity()) {
        neededSize = (neededSize + 65535) & ~65535;

        ALOGV("resizing buffer to size %zu", neededSize);

        sp<ABuffer> buffer = new ABuffer(neededSize);
        if (mBuffer != NULL) {
            memcpy(buffer->data(), mBuffer->data(), mBuffer->size());
            buffer->setRange(0, mBuffer->size());
        } else {
            buffer->setRange(0, 0);
        }

        mBuffer = buffer;
    }

    memcpy(mBuffer->data() + mBuffer->size(), data, size);
    mBuffer->setRange(0, mBuffer->size() + size);

    RangeInfo info;
    info.mLength = size;
    info.mTimestampUs = timeUs;
    mRangeInfos.push_back(info);

#if 0
    if (mMode == AAC) {
        ALOGI("size = %zu, timeUs = %.2f secs", size, timeUs / 1E6);
        hexdump(data, size);
    }
#endif

    return OK;
}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnit() {
    if ((mFlags & kFlag_AlignedData) && mMode == H264) {
        if (mRangeInfos.empty()) {
            return NULL;
        }

        RangeInfo info = *mRangeInfos.begin();
        mRangeInfos.erase(mRangeInfos.begin());

        sp<ABuffer> accessUnit = new ABuffer(info.mLength);
        memcpy(accessUnit->data(), mBuffer->data(), info.mLength);
        accessUnit->meta()->setInt64("timeUs", info.mTimestampUs);

        memmove(mBuffer->data(),
                mBuffer->data() + info.mLength,
                mBuffer->size() - info.mLength);

        mBuffer->setRange(0, mBuffer->size() - info.mLength);

        if (mFormat == NULL) {
            mFormat = MakeAVCCodecSpecificData(accessUnit);
        }

        return accessUnit;
    }

    switch (mMode) {
        case H264:
            return dequeueAccessUnitH264();
        case H265:
            return dequeueAccessUnitH265();
        case AAC:
            return dequeueAccessUnitAAC();
        case AC3:
            return dequeueAccessUnitAC3();
#ifdef DOLBY_ENABLE
        case EAC3:
            return dequeueAccessUnitEAC3();
#endif // DOLBY_END
        case MPEG_VIDEO:
            return dequeueAccessUnitMPEGVideo();
        case MPEG4_VIDEO:
            return dequeueAccessUnitMPEG4Video();
        case PCM_AUDIO:
            return dequeueAccessUnitPCMAudio();
        case METADATA:
            return dequeueAccessUnitMetadata();
#ifdef DTS_CODEC_M_
        case DTSHD:
            return dequeueAccessUnitDTS();
#endif
        default:
            if (mMode != MPEG_AUDIO) {
                ALOGE("Unknown mode");
                return NULL;
            }
            return dequeueAccessUnitMPEGAudio();
    }
}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitAC3() {
    unsigned syncStartPos = 0;  // in bytes
    unsigned payloadSize = 0;
    sp<MetaData> format = new MetaData;
    while (true) {
        if (syncStartPos + 2 >= mBuffer->size()) {
            return NULL;
        }

        payloadSize = parseAC3SyncFrame(
                mBuffer->data() + syncStartPos,
                mBuffer->size() - syncStartPos,
                &format);
        if (payloadSize > 0) {
            break;
        }
        ++syncStartPos;
    }

    if (mBuffer->size() < syncStartPos + payloadSize) {
        ALOGV("Not enough buffer size for AC3");
        return NULL;
    }

    if (mFormat == NULL) {
        mFormat = format;
    }

    sp<ABuffer> accessUnit = new ABuffer(syncStartPos + payloadSize);
    memcpy(accessUnit->data(), mBuffer->data(), syncStartPos + payloadSize);

    int64_t timeUs = fetchTimestamp(syncStartPos + payloadSize);
    if (timeUs < 0ll) {
        ALOGE("negative timeUs");
        return NULL;
    }
    accessUnit->meta()->setInt64("timeUs", timeUs);
    accessUnit->meta()->setInt32("isSync", 1);

    memmove(
            mBuffer->data(),
            mBuffer->data() + syncStartPos + payloadSize,
            mBuffer->size() - syncStartPos - payloadSize);

    mBuffer->setRange(0, mBuffer->size() - syncStartPos - payloadSize);

    return accessUnit;
}

#ifdef DTS_CODEC_M_
sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitDTS() {
    unsigned syncStartPos = 0;  // in bytes
    unsigned payloadSize = 0;
    sp<MetaData> format = new MetaData;
    while (true) {
        if (syncStartPos + 2 >= mBuffer->size()) {
            return NULL;
        }

        payloadSize = parseDTSSyncFrame(
                mBuffer->data() + syncStartPos,
                mBuffer->size() - syncStartPos,
                &format);
        if (payloadSize > 0) {
            break;
        }
        ++syncStartPos;
    }

    if (mBuffer->size() < syncStartPos + payloadSize) {
        ALOGV("Not enough buffer size for DTSHD");
        return NULL;
    }

    if (mFormat == NULL) {
        mFormat = format;
    }

    sp<ABuffer> accessUnit = new ABuffer(syncStartPos + payloadSize);
    memcpy(accessUnit->data(), mBuffer->data(), syncStartPos + payloadSize);

    int64_t timeUs = fetchTimestamp(syncStartPos + payloadSize);
    CHECK_GE(timeUs, 0ll);
    accessUnit->meta()->setInt64("timeUs", timeUs);

    memmove(
            mBuffer->data(),
            mBuffer->data() + syncStartPos + payloadSize,
            mBuffer->size() - syncStartPos - payloadSize);

    mBuffer->setRange(0, mBuffer->size() - syncStartPos - payloadSize);

    return accessUnit;
}
#endif

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitPCMAudio() {
    if (mBuffer->size() < 4) {
        return NULL;
    }

    ABitReader bits(mBuffer->data(), 4);
    if (bits.getBits(8) != 0xa0) {
        ALOGE("Unexpected bit values");
        return NULL;
    }
    unsigned numAUs = bits.getBits(8);
    bits.skipBits(8);
    unsigned quantization_word_length __unused = bits.getBits(2);
    unsigned audio_sampling_frequency = bits.getBits(3);
    unsigned num_channels = bits.getBits(3);

    if (audio_sampling_frequency != 2) {
        ALOGE("Wrong sampling freq");
        return NULL;
    }
    if (num_channels != 1u) {
        ALOGE("Wrong channel #");
        return NULL;
    }

    if (mFormat == NULL) {
        mFormat = new MetaData;
        mFormat->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);
        mFormat->setInt32(kKeyChannelCount, 2);
        mFormat->setInt32(kKeySampleRate, 48000);
    }

    static const size_t kFramesPerAU = 80;
    size_t frameSize = 2 /* numChannels */ * sizeof(int16_t);

    size_t payloadSize = numAUs * frameSize * kFramesPerAU;

    if (mBuffer->size() < 4 + payloadSize) {
        return NULL;
    }

    sp<ABuffer> accessUnit = new ABuffer(payloadSize);
    memcpy(accessUnit->data(), mBuffer->data() + 4, payloadSize);

    int64_t timeUs = fetchTimestamp(payloadSize + 4);
    if (timeUs < 0ll) {
        ALOGE("Negative timeUs");
        return NULL;
    }
    accessUnit->meta()->setInt64("timeUs", timeUs);
    accessUnit->meta()->setInt32("isSync", 1);

    int16_t *ptr = (int16_t *)accessUnit->data();
    for (size_t i = 0; i < payloadSize / sizeof(int16_t); ++i) {
        ptr[i] = ntohs(ptr[i]);
    }

    memmove(
            mBuffer->data(),
            mBuffer->data() + 4 + payloadSize,
            mBuffer->size() - 4 - payloadSize);

    mBuffer->setRange(0, mBuffer->size() - 4 - payloadSize);

    return accessUnit;
}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitAAC() {
    if (mBuffer->size() == 0) {
        return NULL;
    }

    if (mRangeInfos.empty()) {
        return NULL;
    }

    const RangeInfo &info = *mRangeInfos.begin();
    if (mBuffer->size() < info.mLength) {
        return NULL;
    }

    if (info.mTimestampUs < 0ll) {
        ALOGE("Negative info.mTimestampUs");
        return NULL;
    }

    // The idea here is consume all AAC frames starting at offsets before
    // info.mLength so we can assign a meaningful timestamp without
    // having to interpolate.
    // The final AAC frame may well extend into the next RangeInfo but
    // that's ok.
    size_t offset = 0;
    while (offset < info.mLength) {
        if (offset + 7 > mBuffer->size()) {
            return NULL;
        }

        ABitReader bits(mBuffer->data() + offset, mBuffer->size() - offset);

        // adts_fixed_header

        if (bits.getBits(12) != 0xfffu) {
            ALOGE("Wrong atds_fixed_header");
            return NULL;
        }
        bits.skipBits(3);  // ID, layer
        bool protection_absent __unused = bits.getBits(1) != 0;

        if (mFormat == NULL) {
            unsigned profile = bits.getBits(2);
            if (profile == 3u) {
                ALOGE("profile should not be 3");
                return NULL;
            }
            unsigned sampling_freq_index = bits.getBits(4);
            bits.getBits(1);  // private_bit
            unsigned channel_configuration = bits.getBits(3);
            if (channel_configuration == 0u) {
                ALOGE("channel_config should not be 0");
                return NULL;
            }
            bits.skipBits(2);  // original_copy, home

            mFormat = MakeAACCodecSpecificData(
                    profile, sampling_freq_index, channel_configuration);

            mFormat->setInt32(kKeyIsADTS, true);

            int32_t sampleRate;
            int32_t numChannels;
            if (!mFormat->findInt32(kKeySampleRate, &sampleRate)) {
                ALOGE("SampleRate not found");
                return NULL;
            }
            if (!mFormat->findInt32(kKeyChannelCount, &numChannels)) {
                ALOGE("ChannelCount not found");
                return NULL;
            }

            ALOGI("found AAC codec config (%d Hz, %d channels)",
                 sampleRate, numChannels);
        } else {
            // profile_ObjectType, sampling_frequency_index, private_bits,
            // channel_configuration, original_copy, home
            bits.skipBits(12);
        }

        // adts_variable_header

        // copyright_identification_bit, copyright_identification_start
        bits.skipBits(2);

        unsigned aac_frame_length = bits.getBits(13);

        bits.skipBits(11);  // adts_buffer_fullness

        unsigned number_of_raw_data_blocks_in_frame = bits.getBits(2);

        if (number_of_raw_data_blocks_in_frame != 0) {
            // To be implemented.
            ALOGE("Should not reach here.");
            return NULL;
        }

        if (offset + aac_frame_length > mBuffer->size()) {
            return NULL;
        }

        size_t headerSize __unused = protection_absent ? 7 : 9;

        offset += aac_frame_length;
    }

    int64_t timeUs = fetchTimestamp(offset);

    sp<ABuffer> accessUnit = new ABuffer(offset);
    memcpy(accessUnit->data(), mBuffer->data(), offset);

    memmove(mBuffer->data(), mBuffer->data() + offset,
            mBuffer->size() - offset);
    mBuffer->setRange(0, mBuffer->size() - offset);

    accessUnit->meta()->setInt64("timeUs", timeUs);
    accessUnit->meta()->setInt32("isSync", 1);

    return accessUnit;
}

int64_t ElementaryStreamQueue::fetchTimestamp(size_t size) {
    int64_t timeUs = -1;
    bool first = true;

    while (size > 0) {
        if (mRangeInfos.empty()) {
            return timeUs;
        }

        RangeInfo *info = &*mRangeInfos.begin();

        if (first) {
            timeUs = info->mTimestampUs;
            first = false;
        }

        if (info->mLength > size) {
            info->mLength -= size;
            size = 0;
        } else {
            size -= info->mLength;

            mRangeInfos.erase(mRangeInfos.begin());
            info = NULL;
        }

    }

    if (timeUs == 0ll) {
        ALOGV("Returning 0 timestamp");
    }

    return timeUs;
}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitH264() {
    const uint8_t *data = mBuffer->data();

    size_t size = mBuffer->size();
    Vector<NALPosition> nals;

    size_t totalSize = 0;
    size_t seiCount = 0;

    status_t err;
    const uint8_t *nalStart;
    size_t nalSize;
    bool foundSlice = false;
    bool foundIDR = false;
    while ((err = getNextNALUnit(&data, &size, &nalStart, &nalSize)) == OK) {
        if (nalSize == 0) continue;

        unsigned nalType = nalStart[0] & 0x1f;
        bool flush = false;

        if (nalType == 1 || nalType == 5) {
            if (nalType == 5) {
                foundIDR = true;
            }
            if (foundSlice) {
                ABitReader br(nalStart + 1, nalSize);
                unsigned first_mb_in_slice = parseUE(&br);

                if (first_mb_in_slice == 0) {
                    // This slice starts a new frame.

                    flush = true;
                }
            }

            foundSlice = true;
        } else if ((nalType == 9 || nalType == 7) && foundSlice) {
            // Access unit delimiter and SPS will be associated with the
            // next frame.

            flush = true;
        } else if (nalType == 6 && nalSize > 0) {
            // found non-zero sized SEI
            ++seiCount;
        }

        if (flush) {
            // The access unit will contain all nal units up to, but excluding
            // the current one, separated by 0x00 0x00 0x00 0x01 startcodes.

            size_t auSize = 4 * nals.size() + totalSize;
            sp<ABuffer> accessUnit = new ABuffer(auSize);
            sp<ABuffer> sei;

            if (seiCount > 0) {
                sei = new ABuffer(seiCount * sizeof(NALPosition));
                accessUnit->meta()->setBuffer("sei", sei);
            }

#if !LOG_NDEBUG
            AString out;
#endif

            size_t dstOffset = 0;
            size_t seiIndex = 0;
            for (size_t i = 0; i < nals.size(); ++i) {
                const NALPosition &pos = nals.itemAt(i);

                unsigned nalType = mBuffer->data()[pos.nalOffset] & 0x1f;

                if (nalType == 6 && pos.nalSize > 0) {
                    if (seiIndex >= sei->size() / sizeof(NALPosition)) {
                        ALOGE("Wrong seiIndex");
                        return NULL;
                    }
                    NALPosition &seiPos = ((NALPosition *)sei->data())[seiIndex++];
                    seiPos.nalOffset = dstOffset + 4;
                    seiPos.nalSize = pos.nalSize;
                }

#if !LOG_NDEBUG
                char tmp[128];
                sprintf(tmp, "0x%02x", nalType);
                if (i > 0) {
                    out.append(", ");
                }
                out.append(tmp);
#endif

                memcpy(accessUnit->data() + dstOffset, "\x00\x00\x00\x01", 4);

                memcpy(accessUnit->data() + dstOffset + 4,
                       mBuffer->data() + pos.nalOffset,
                       pos.nalSize);

                dstOffset += pos.nalSize + 4;
            }

#if !LOG_NDEBUG
            ALOGV("accessUnit contains nal types %s", out.c_str());
#endif

            const NALPosition &pos = nals.itemAt(nals.size() - 1);
            size_t nextScan = pos.nalOffset + pos.nalSize;

            memmove(mBuffer->data(),
                    mBuffer->data() + nextScan,
                    mBuffer->size() - nextScan);

            mBuffer->setRange(0, mBuffer->size() - nextScan);

            int64_t timeUs = fetchTimestamp(nextScan);
            if (timeUs < 0ll) {
                ALOGE("Negative timeUs");
                return NULL;
            }

            accessUnit->meta()->setInt64("timeUs", timeUs);
            if (foundIDR) {
                accessUnit->meta()->setInt32("isSync", 1);
            }

            if (mFormat == NULL) {
                mFormat = MakeAVCCodecSpecificData(accessUnit);
            }

            return accessUnit;
        }

        NALPosition pos;
        pos.nalOffset = nalStart - mBuffer->data();
        pos.nalSize = nalSize;

        nals.push(pos);

        totalSize += nalSize;
    }
    if (err != (status_t)-EAGAIN) {
        ALOGE("Unexpeted err");
        return NULL;
    }

    return NULL;
}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitMPEGAudio() {
    const uint8_t *data = mBuffer->data();
    size_t size = mBuffer->size();

    if (size < 4) {
        return NULL;
    }

    uint32_t header = U32_AT(data);

    size_t frameSize;
    int samplingRate, numChannels, bitrate, numSamples;
    if (!GetMPEGAudioFrameSize(
                header, &frameSize, &samplingRate, &numChannels,
                &bitrate, &numSamples)) {
        ALOGE("Failed to get audio frame size");
        return NULL;
    }

    if (size < frameSize) {
        return NULL;
    }

    unsigned layer = 4 - ((header >> 17) & 3);

    sp<ABuffer> accessUnit = new ABuffer(frameSize);
    memcpy(accessUnit->data(), data, frameSize);

    memmove(mBuffer->data(),
            mBuffer->data() + frameSize,
            mBuffer->size() - frameSize);

    mBuffer->setRange(0, mBuffer->size() - frameSize);

    int64_t timeUs = fetchTimestamp(frameSize);
    if (timeUs < 0ll) {
        ALOGE("Negative timeUs");
        return NULL;
    }

    accessUnit->meta()->setInt64("timeUs", timeUs);
    accessUnit->meta()->setInt32("isSync", 1);

    if (mFormat == NULL) {
        mFormat = new MetaData;

        switch (layer) {
            case 1:
                mFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_I);
                break;
            case 2:
                mFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II);
                break;
            case 3:
                mFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG);
                break;
            default:
                return NULL;
        }

        mFormat->setInt32(kKeySampleRate, samplingRate);
        mFormat->setInt32(kKeyChannelCount, numChannels);
    }

    return accessUnit;
}

static void EncodeSize14(uint8_t **_ptr, size_t size) {
    if (size > 0x3fff) {
        ALOGE("Wrong size");
        return;
    }

    uint8_t *ptr = *_ptr;

    *ptr++ = 0x80 | (size >> 7);
    *ptr++ = size & 0x7f;

    *_ptr = ptr;
}

static sp<ABuffer> MakeMPEGVideoESDS(const sp<ABuffer> &csd) {
    sp<ABuffer> esds = new ABuffer(csd->size() + 25);

    uint8_t *ptr = esds->data();
    *ptr++ = 0x03;
    EncodeSize14(&ptr, 22 + csd->size());

    *ptr++ = 0x00;  // ES_ID
    *ptr++ = 0x00;

    *ptr++ = 0x00;  // streamDependenceFlag, URL_Flag, OCRstreamFlag

    *ptr++ = 0x04;
    EncodeSize14(&ptr, 16 + csd->size());

    *ptr++ = 0x40;  // Audio ISO/IEC 14496-3

    for (size_t i = 0; i < 12; ++i) {
        *ptr++ = 0x00;
    }

    *ptr++ = 0x05;
    EncodeSize14(&ptr, csd->size());

    memcpy(ptr, csd->data(), csd->size());

    return esds;
}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitMPEGVideo() {
    const uint8_t *data = mBuffer->data();
    size_t size = mBuffer->size();

    bool sawPictureStart = false;
    int pprevStartCode = -1;
    int prevStartCode = -1;
    int currentStartCode = -1;
    bool gopFound = false;
    bool isClosedGop = false;
    bool brokenLink = false;

    size_t offset = 0;
    while (offset + 3 < size) {
        if (memcmp(&data[offset], "\x00\x00\x01", 3)) {
            ++offset;
            continue;
        }

        pprevStartCode = prevStartCode;
        prevStartCode = currentStartCode;
        currentStartCode = data[offset + 3];

        if (currentStartCode == 0xb3 && mFormat == NULL) {
            memmove(mBuffer->data(), mBuffer->data() + offset, size - offset);
            size -= offset;
            (void)fetchTimestamp(offset);
            offset = 0;
            mBuffer->setRange(0, size);
        }

        if ((prevStartCode == 0xb3 && currentStartCode != 0xb5)
                || (pprevStartCode == 0xb3 && prevStartCode == 0xb5)) {
            // seqHeader without/with extension

            if (mFormat == NULL) {
                if (size < 7u) {
                    ALOGE("Size too small");
                    return NULL;
                }

                unsigned width =
                    (data[4] << 4) | data[5] >> 4;

                unsigned height =
                    ((data[5] & 0x0f) << 8) | data[6];

                mFormat = new MetaData;
                mFormat->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG2);
                mFormat->setInt32(kKeyWidth, width);
                mFormat->setInt32(kKeyHeight, height);

                ALOGI("found MPEG2 video codec config (%d x %d)", width, height);

                sp<ABuffer> csd = new ABuffer(offset);
                memcpy(csd->data(), data, offset);

                memmove(mBuffer->data(),
                        mBuffer->data() + offset,
                        mBuffer->size() - offset);

                mBuffer->setRange(0, mBuffer->size() - offset);
                size -= offset;
                (void)fetchTimestamp(offset);
                offset = 0;

                // hexdump(csd->data(), csd->size());

                sp<ABuffer> esds = MakeMPEGVideoESDS(csd);
                mFormat->setData(
                        kKeyESDS, kTypeESDS, esds->data(), esds->size());

                return NULL;
            }
        }

        if (mFormat != NULL && currentStartCode == 0xb8) {
            // GOP layer
            gopFound = true;
            isClosedGop = (data[offset + 7] & 0x40) != 0;
            brokenLink = (data[offset + 7] & 0x20) != 0;
        }

        if (mFormat != NULL && currentStartCode == 0x00) {
            // Picture start

            if (!sawPictureStart) {
                sawPictureStart = true;
            } else {
                sp<ABuffer> accessUnit = new ABuffer(offset);
                memcpy(accessUnit->data(), data, offset);

                memmove(mBuffer->data(),
                        mBuffer->data() + offset,
                        mBuffer->size() - offset);

                mBuffer->setRange(0, mBuffer->size() - offset);

                int64_t timeUs = fetchTimestamp(offset);
                if (timeUs < 0ll) {
                    ALOGE("Negative timeUs");
                    return NULL;
                }

                offset = 0;

                accessUnit->meta()->setInt64("timeUs", timeUs);
                if (gopFound && (!brokenLink || isClosedGop)) {
                    accessUnit->meta()->setInt32("isSync", 1);
                }

                ALOGV("returning MPEG video access unit at time %" PRId64 " us",
                      timeUs);

                // hexdump(accessUnit->data(), accessUnit->size());

                return accessUnit;
            }
        }

        ++offset;
    }

    return NULL;
}

static ssize_t getNextChunkSize(
        const uint8_t *data, size_t size) {
    static const char kStartCode[] = "\x00\x00\x01";

    if (size < 3) {
        return -EAGAIN;
    }

    if (memcmp(kStartCode, data, 3)) {
        return -EAGAIN;
    }

    size_t offset = 3;
    while (offset + 2 < size) {
        if (!memcmp(&data[offset], kStartCode, 3)) {
            return offset;
        }

        ++offset;
    }

    return -EAGAIN;
}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitMPEG4Video() {
    uint8_t *data = mBuffer->data();
    size_t size = mBuffer->size();

    enum {
        SKIP_TO_VISUAL_OBJECT_SEQ_START,
        EXPECT_VISUAL_OBJECT_START,
        EXPECT_VO_START,
        EXPECT_VOL_START,
        WAIT_FOR_VOP_START,
        SKIP_TO_VOP_START,

    } state;

    if (mFormat == NULL) {
        state = SKIP_TO_VISUAL_OBJECT_SEQ_START;
    } else {
        state = SKIP_TO_VOP_START;
    }

    int32_t width = -1, height = -1;

    size_t offset = 0;
    ssize_t chunkSize;
    while ((chunkSize = getNextChunkSize(
                    &data[offset], size - offset)) > 0) {
        bool discard = false;

        unsigned chunkType = data[offset + 3];

        switch (state) {
            case SKIP_TO_VISUAL_OBJECT_SEQ_START:
            {
                if (chunkType == 0xb0) {
                    // Discard anything before this marker.

                    state = EXPECT_VISUAL_OBJECT_START;
                } else {
                    discard = true;
                }
                break;
            }

            case EXPECT_VISUAL_OBJECT_START:
            {
                if (chunkType != 0xb5) {
                    ALOGE("Unexpected chunkType");
                    return NULL;
                }
                state = EXPECT_VO_START;
                break;
            }

            case EXPECT_VO_START:
            {
                if (chunkType > 0x1f) {
                    ALOGE("Unexpected chunkType");
                    return NULL;
                }
                state = EXPECT_VOL_START;
                break;
            }

            case EXPECT_VOL_START:
            {
                if ((chunkType & 0xf0) != 0x20) {
                    ALOGE("Wrong chunkType");
                    return NULL;
                }

                if (!ExtractDimensionsFromVOLHeader(
                            &data[offset], chunkSize,
                            &width, &height)) {
                    ALOGE("Failed to get dimension");
                    return NULL;
                }

                state = WAIT_FOR_VOP_START;
                break;
            }

            case WAIT_FOR_VOP_START:
            {
                if (chunkType == 0xb3 || chunkType == 0xb6) {
                    // group of VOP or VOP start.

                    mFormat = new MetaData;
                    mFormat->setCString(
                            kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG4);

                    mFormat->setInt32(kKeyWidth, width);
                    mFormat->setInt32(kKeyHeight, height);

                    ALOGI("found MPEG4 video codec config (%d x %d)",
                         width, height);

                    sp<ABuffer> csd = new ABuffer(offset);
                    memcpy(csd->data(), data, offset);

                    // hexdump(csd->data(), csd->size());

                    sp<ABuffer> esds = MakeMPEGVideoESDS(csd);
                    mFormat->setData(
                            kKeyESDS, kTypeESDS,
                            esds->data(), esds->size());

                    discard = true;
                    state = SKIP_TO_VOP_START;
                }

                break;
            }

            case SKIP_TO_VOP_START:
            {
                if (chunkType == 0xb6) {
                    int vopCodingType = (data[offset + 4] & 0xc0) >> 6;

                    offset += chunkSize;

                    sp<ABuffer> accessUnit = new ABuffer(offset);
                    memcpy(accessUnit->data(), data, offset);

                    memmove(data, &data[offset], size - offset);
                    size -= offset;
                    mBuffer->setRange(0, size);

                    int64_t timeUs = fetchTimestamp(offset);
                    if (timeUs < 0ll) {
                        ALOGE("Negative timeus");
                        return NULL;
                    }

                    offset = 0;

                    accessUnit->meta()->setInt64("timeUs", timeUs);
                    if (vopCodingType == 0) {  // intra-coded VOP
                        accessUnit->meta()->setInt32("isSync", 1);
                    }

                    ALOGV("returning MPEG4 video access unit at time %" PRId64 " us",
                         timeUs);

                    // hexdump(accessUnit->data(), accessUnit->size());

                    return accessUnit;
                } else if (chunkType != 0xb3) {
                    offset += chunkSize;
                    discard = true;
                }

                break;
            }

            default:
                ALOGE("Unknown state: %d", state);
                return NULL;
        }

        if (discard) {
            (void)fetchTimestamp(offset);
            memmove(data, &data[offset], size - offset);
            size -= offset;
            offset = 0;
            mBuffer->setRange(0, size);
        } else {
            offset += chunkSize;
        }
    }

    return NULL;
}

void ElementaryStreamQueue::signalEOS() {
    if (!mEOSReached) {
        if (mMode == MPEG_VIDEO) {
            const char *theEnd = "\x00\x00\x01\x00";
            appendData(theEnd, 4, 0);
        }
        mEOSReached = true;
    } else {
        ALOGW("EOS already signaled");
    }
}

sp<ABuffer> ElementaryStreamQueue::dequeueAccessUnitMetadata() {
    size_t size = mBuffer->size();
    if (!size) {
        return NULL;
    }

    sp<ABuffer> accessUnit = new ABuffer(size);
    int64_t timeUs = fetchTimestamp(size);
    accessUnit->meta()->setInt64("timeUs", timeUs);

    memcpy(accessUnit->data(), mBuffer->data(), size);
    mBuffer->setRange(0, 0);

    if (mFormat == NULL) {
        mFormat = new MetaData;
        mFormat->setCString(kKeyMIMEType, MEDIA_MIMETYPE_DATA_TIMED_ID3);
    }

    return accessUnit;
}

}  // namespace android