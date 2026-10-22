// The part of Signal Hound's bb_api.h that wvt uses, for the simulated BB60
// (bb_api_sim.cpp). The declarations and values match bb_api 5.0.x, so
// src/device/sdr_bb60.cpp builds against either. Build with -DWVT_BB_SIM=ON; for a
// real BB60, point SIGNALHOUND_SDK_DIR at the SDK instead.
#ifndef WVT_BB_API_SIM_H
#define WVT_BB_API_SIM_H

#include <stdint.h>

#define BB_API __attribute__((visibility("default")))

#define BB_TRUE (1)
#define BB_FALSE (0)

#define BB_DEVICE_NONE (0)
#define BB_DEVICE_BB60A (1)
#define BB_DEVICE_BB60C (2)
#define BB_DEVICE_BB60D (3)

#define BB_MAX_DEVICES (8)
#define BB_MIN_FREQ (9.0e3)
#define BB_MAX_FREQ (6.4e9)
#define BB_MAX_REFERENCE (20.0)
#define BB_AUTO_ATTEN (-1)
#define BB_MAX_ATTEN (3)
#define BB_AUTO_GAIN (-1)
#define BB_MAX_GAIN (3)
#define BB_MIN_DECIMATION (1)
#define BB_MAX_DECIMATION (8192)

#define BB_STREAMING (4)
#define BB_STREAM_IQ (0x0)

#define BB60C_PORT1_10MHZ_REF_IN (0x8)
#define BB60D_PORT1_10MHZ_REF_IN (1)
#define BB60D_PORT2_DISABLED (0)

typedef enum bbStatus {
    bbFrequencyRangeErr = -103,
    bbBandwidthErr = -106,
    bbDeviceConnectionErr = -14,
    bbDeviceInvalidErr = -9,
    bbDeviceAlreadyStreamingErr = -5,
    bbInvalidParameterErr = -4,
    bbDeviceNotStreamingErr = -2,
    bbDeviceNotOpenErr = -1,
    bbNoError = 0,
    bbADCOverflow = 2,
} bbStatus;

typedef enum bbDataType { bbDataType32fc = 0, bbDataType16sc = 1 } bbDataType;

#ifdef __cplusplus
extern "C" {
#endif

BB_API bbStatus bbGetSerialNumberList2(int serialNumbers[BB_MAX_DEVICES], int deviceTypes[BB_MAX_DEVICES],
                                       int *deviceCount);
BB_API bbStatus bbOpenDevice(int *device);
BB_API bbStatus bbOpenDeviceBySerialNumber(int *device, int serialNumber);
BB_API bbStatus bbCloseDevice(int device);
BB_API bbStatus bbGetSerialNumber(int device, uint32_t *serialNumber);
BB_API bbStatus bbGetDeviceType(int device, int *deviceType);
BB_API bbStatus bbConfigureIO(int device, uint32_t port1, uint32_t port2);
BB_API bbStatus bbConfigureIQDataType(int device, bbDataType dataType);
BB_API bbStatus bbConfigureIQCenter(int device, double centerFreq);
BB_API bbStatus bbConfigureRefLevel(int device, double refLevel);
BB_API bbStatus bbConfigureGainAtten(int device, int gain, int atten);
BB_API bbStatus bbConfigureIQ(int device, int downsampleFactor, double bandwidth);
BB_API bbStatus bbInitiate(int device, uint32_t mode, uint32_t flag);
BB_API bbStatus bbAbort(int device);
BB_API bbStatus bbQueryIQParameters(int device, double *sampleRate, double *bandwidth);
BB_API bbStatus bbGetIQUnpacked(int device, void *iqData, int iqCount, int *triggers, int triggerCount, int purge,
                                int *dataRemaining, int *sampleLoss, int *sec, int *nano);
BB_API const char *bbGetErrorString(bbStatus status);

#ifdef __cplusplus
}
#endif

#endif
