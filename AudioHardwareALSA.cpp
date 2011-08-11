/* AudioHardwareALSA.cpp
 **
 ** Copyright 2008-2010 Wind River Systems
 ** Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>

#define LOG_TAG "AudioHardwareALSA"
#define LOG_NDEBUG 0
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>

#include "AudioHardwareALSA.h"

extern "C"
{
    //
    // Function for dlsym() to look up for creating a new AudioHardwareInterface.
    //
    android::AudioHardwareInterface *createAudioHardware(void) {
        return android::AudioHardwareALSA::create();
    }
}         // extern "C"

namespace android
{

// ----------------------------------------------------------------------------

AudioHardwareInterface *AudioHardwareALSA::create() {
    return new AudioHardwareALSA();
}

AudioHardwareALSA::AudioHardwareALSA() :
    mALSADevice(0)
{
    hw_module_t *module;
    int err = hw_get_module(ALSA_HARDWARE_MODULE_ID,
            (hw_module_t const**)&module);
    LOGV("hw_get_module(ALSA_HARDWARE_MODULE_ID) returned err %d", err);
    if (err == 0) {
        hw_device_t* device;
        err = module->methods->open(module, ALSA_HARDWARE_NAME, &device);
        if (err == 0) {
            mALSADevice = (alsa_device_t *)device;
            mALSADevice->init(mALSADevice, mDeviceList);
            mIsVoiceCallActive = 0;
            mIsFmActive = 0;
            mDmicActive = false;
            mAncActive = false;
            mBluetoothVGS = false;
            mTtyMode = TTY_OFF;
            snd_use_case_mgr_open(&mUcMgr, "snd_soc_msm");
            if (mUcMgr < 0) {
                LOGE("Failed to open ucm instance: %d", errno);
            } else {
                LOGI("ucm instance opened: %u", (unsigned)mUcMgr);
            }
        } else {
            LOGE("ALSA Module could not be opened!!!");
        }
    } else {
        LOGE("ALSA Module not found!!!");
    }
}

AudioHardwareALSA::~AudioHardwareALSA()
{
    if (mUcMgr != NULL) {
        LOGV("closing ucm instance: %u", (unsigned)mUcMgr);
        snd_use_case_mgr_close(mUcMgr);
    }
    if (mALSADevice) {
        mALSADevice->common.close(&mALSADevice->common);
    }
    for(ALSAHandleList::iterator it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
        it->useCase[0] = 0;
        mDeviceList.erase(it);
    }
}

status_t AudioHardwareALSA::initCheck()
{
    if (!mALSADevice)
        return NO_INIT;

    return NO_ERROR;
}

status_t AudioHardwareALSA::setVoiceVolume(float v)
{
    LOGD("setVoiceVolume(%f)\n", v);
    if (v < 0.0) {
        LOGW("setVoiceVolume(%f) under 0.0, assuming 0.0\n", v);
        v = 0.0;
    } else if (v > 1.0) {
        LOGW("setVoiceVolume(%f) over 1.0, assuming 1.0\n", v);
        v = 1.0;
    }

    int vol = lrint(v * 100.0);

    // Voice volume levels from android are mapped to driver volume levels as follows.
    // 0 -> 5, 20 -> 4, 40 ->3, 60 -> 2, 80 -> 1, 100 -> 0
    // So adjust the volume to get the correct volume index in driver
    vol = 100 - vol;

    // ToDo: Send mixer command only when voice call is active
    if(mALSADevice) {
        mALSADevice->setVoiceVolume(vol);
    }

    return NO_ERROR;
}

status_t  AudioHardwareALSA::setFmVolume(float value)
{
    status_t status = NO_ERROR;

    int vol = AudioSystem::logToLinear( value );

    if (vol > 100)
        vol = 100;
    else if (vol < 0)
        vol = 0;

    LOGV("setFmVolume(%f)\n", value);
    LOGV("Setting FM volume to %d (available range is 0 to 100)\n", vol);

    mALSADevice->setFmVolume(vol);

    return status;
}

status_t AudioHardwareALSA::setMasterVolume(float volume)
{
    return NO_ERROR;
}

status_t AudioHardwareALSA::setMode(int mode)
{
    status_t status = NO_ERROR;

    if (mode != mMode) {
        status = AudioHardwareBase::setMode(mode);
    }

    return status;
}

status_t AudioHardwareALSA::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key;
    String8 value;
    status_t status = NO_ERROR;
    int device;
    int btRate;
    LOGV("setParameters() %s", keyValuePairs.string());

    key = String8(TTY_MODE_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "full") {
            mTtyMode = TTY_FULL;
        } else if (value == "hco") {
            mTtyMode = TTY_HCO;
        } else if (value == "vco") {
            mTtyMode = TTY_VCO;
        } else {
            mTtyMode = TTY_OFF;
        }
        if(mMode != AudioSystem::MODE_IN_CALL){
           return NO_ERROR;
        }
        LOGI("Changed TTY Mode=%s", value.string());
        doRouting(0);
    }

    key = String8(DUALMIC_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "true") {
            mDmicActive = true;
            LOGI("DualMic feature Enabled");
        } else {
            mDmicActive = false;
            LOGI("DualMic feature Disabled");
        }
        doRouting(0);
    }

    key = String8(ANC_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "true") {
            LOGV("Enabling ANC setting in the setparameter\n");
            mAncActive= true;
        } else {
            LOGV("Disabling ANC setting in the setparameter\n");
            mAncActive= false;
        }
        doRouting(0);
    }

    key = String8(AudioParameter::keyRouting);
    if (param.getInt(key, device) == NO_ERROR) {
        // Ignore routing if device is 0.
        if(device) {
            doRouting(device);
        }
        param.remove(key);
    }

    key = String8(BT_SAMPLERATE_KEY);
    if (param.getInt(key, btRate) == NO_ERROR) {
        mALSADevice->setBtscoRate(btRate);
        param.remove(key);
    }

    key = String8(BTHEADSET_VGS);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "on") {
            mBluetoothVGS = true;
        } else {
            mBluetoothVGS = false;
        }
    }

    if (param.size()) {
        status = BAD_VALUE;
    }
    return status;
}

String8 AudioHardwareALSA::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    String8 value;

    String8 key = String8(DUALMIC_KEY);
    if (param.get(key, value) == NO_ERROR) {
        value = String8(mDmicActive ? "true" : "false");
        param.add(key, value);
    }

    key = String8("Fm-radio");
    if ( param.get(key,value) == NO_ERROR ) {
        if ( mIsFmActive ) {
            param.addInt(String8("isFMON"), true );
        }
    }

    key = String8(BTHEADSET_VGS);
    if (param.get(key, value) == NO_ERROR) {
        if(mBluetoothVGS)
           param.addInt(String8("isVGS"), true);
    }

    LOGV("AudioHardwareALSA::getParameters() %s", param.toString().string());
    return param.toString();
}

void AudioHardwareALSA::doRouting(int device)
{
    int newMode = mode();

    if ((device == AudioSystem::DEVICE_IN_VOICE_CALL) ||
        (device == AudioSystem::DEVICE_IN_FM_RX) ||
        (device == AudioSystem::DEVICE_IN_FM_RX_A2DP)) {
        LOGV("Ignoring routing for FM/INCALL recording");
        return;
    }
    if (mAncActive == true) {
        LOGV("doRouting: setting anc device device %d", device);
        if (device & AudioSystem::DEVICE_OUT_WIRED_HEADSET) {
            device &= (~AudioSystem::DEVICE_OUT_WIRED_HEADSET);
            device |= AudioSystem::DEVICE_OUT_ANC_HEADSET;
        } else if (device & AudioSystem::DEVICE_IN_WIRED_HEADSET) {
            device &= (~AudioSystem::DEVICE_IN_WIRED_HEADSET);
            device |= AudioSystem::DEVICE_IN_ANC_HEADSET;
        } else if (device == 0) {
            ALSAHandleList::iterator it = mDeviceList.end();
            it--;
            if (it->devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) {
                device = AudioSystem::DEVICE_OUT_ANC_HEADSET;
            } else if (it->devices & AudioSystem::DEVICE_IN_WIRED_HEADSET) {
                device = AudioSystem::DEVICE_IN_ANC_HEADSET;
            } else {
                LOGV("No headset connected, ignore ANC setting");
                return;
            }
        }
    } else if (mAncActive == false && device == 0){
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        if (it->devices & AudioSystem::DEVICE_OUT_ANC_HEADSET) {
            device = AudioSystem::DEVICE_OUT_WIRED_HEADSET;
        } else if (it->devices & AudioSystem::DEVICE_IN_ANC_HEADSET) {
            device = AudioSystem::DEVICE_IN_WIRED_HEADSET;
        }
    }
    if (newMode == AudioSystem::MODE_IN_CALL) {
        if ((device & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
            (device & AudioSystem::DEVICE_IN_WIRED_HEADSET)) {
            device = device | (AudioSystem::DEVICE_OUT_WIRED_HEADSET |
                      AudioSystem::DEVICE_IN_WIRED_HEADSET);
        } else if (device & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE) {
            device = device | (AudioSystem::DEVICE_OUT_WIRED_HEADPHONE |
                      AudioSystem::DEVICE_IN_BUILTIN_MIC);
        } else if ((device & AudioSystem::DEVICE_OUT_EARPIECE) ||
                  (device & AudioSystem::DEVICE_IN_BUILTIN_MIC)) {
            device = device | (AudioSystem::DEVICE_IN_BUILTIN_MIC |
                      AudioSystem::DEVICE_OUT_EARPIECE);
        } else if (device & AudioSystem::DEVICE_OUT_SPEAKER) {
            device = device | (AudioSystem::DEVICE_IN_DEFAULT |
                       AudioSystem::DEVICE_OUT_SPEAKER);
        } else if ((device & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO) ||
                   (device & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET) ||
                   (device & AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET)) {
            device = device | (AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET |
                      AudioSystem::DEVICE_OUT_BLUETOOTH_SCO);
        } else if ((device & AudioSystem::DEVICE_OUT_ANC_HEADSET) ||
                   (device & AudioSystem::DEVICE_IN_ANC_HEADSET)) {
            device = device | (AudioSystem::DEVICE_OUT_ANC_HEADSET |
                      AudioSystem::DEVICE_IN_ANC_HEADSET);
        } else if (device & AudioSystem::DEVICE_OUT_ANC_HEADPHONE) {
            device = device | (AudioSystem::DEVICE_OUT_ANC_HEADPHONE |
                      AudioSystem::DEVICE_IN_BUILTIN_MIC);
        }
    }
    if ((device & AudioSystem::DEVICE_IN_BUILTIN_MIC) && (mDmicActive == true)) {
        device |= AudioSystem::DEVICE_IN_BACK_MIC;
    } else if ((device & AudioSystem::DEVICE_IN_BACK_MIC) && (mDmicActive == false)) {
        device &= (~AudioSystem::DEVICE_IN_BACK_MIC);
    }
    LOGV("doRouting: device %d newMode %d mIsVoiceCallActive %d mIsFmActive %d",
          device, newMode, mIsVoiceCallActive, mIsFmActive);
    if((newMode == AudioSystem::MODE_IN_CALL) && (mIsVoiceCallActive == 0)) {
        // Start voice call
        unsigned long bufferSize = DEFAULT_BUFFER_SIZE;
        alsa_handle_t alsa_handle;
        char *use_case;
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_VOICECALL, sizeof(alsa_handle.useCase));
        } else {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_VOICE, sizeof(alsa_handle.useCase));
        }
        free(use_case);

        for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;
        alsa_handle.module = mALSADevice;
        alsa_handle.bufferSize = bufferSize;
        alsa_handle.devices = device;
        alsa_handle.handle = 0;
        alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
        alsa_handle.channels = VOICE_CHANNEL_MODE;
        alsa_handle.sampleRate = VOICE_SAMPLING_RATE;
        alsa_handle.latency = VOICE_LATENCY;
        alsa_handle.recHandle = 0;
        alsa_handle.ucMgr = mUcMgr;
        mIsVoiceCallActive = 1;
        mDeviceList.push_back(alsa_handle);
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        LOGV("Enabling voice call");
        mALSADevice->route(&(*it), (uint32_t)device, newMode, mTtyMode);
        if (!strcmp(it->useCase, SND_USE_CASE_VERB_VOICECALL)) {
            snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_VOICECALL);
        } else {
            snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_VOICE);
        }
        mALSADevice->startVoiceCall(&(*it));
    } else if(newMode == AudioSystem::MODE_NORMAL && mIsVoiceCallActive == 1) {
        // End voice call
        for(ALSAHandleList::iterator it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
            if((!strcmp(it->useCase, SND_USE_CASE_VERB_VOICECALL)) ||
               (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_VOICE))) {
                LOGV("Disabling voice call");
                mALSADevice->close(&(*it));
                mDeviceList.erase(it);
                mALSADevice->route(&(*it), (uint32_t)device, newMode, mTtyMode);
                break;
            }
        }
        mIsVoiceCallActive = 0;
    } else if(device & AudioSystem::DEVICE_OUT_FM && mIsFmActive == 0) {
        // Start FM Radio on current active device
        unsigned long bufferSize = FM_BUFFER_SIZE;
        alsa_handle_t alsa_handle;
        char *use_case;
        LOGV("Start FM");
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_DIGITAL_RADIO, sizeof(alsa_handle.useCase));
        } else {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_FM, sizeof(alsa_handle.useCase));
        }
        free(use_case);

        for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;
        alsa_handle.module = mALSADevice;
        alsa_handle.bufferSize = bufferSize;
        alsa_handle.devices = device;
        alsa_handle.handle = 0;
        alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
        alsa_handle.channels = DEFAULT_CHANNEL_MODE;
        alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
        alsa_handle.latency = VOICE_LATENCY;
        alsa_handle.recHandle = 0;
        alsa_handle.ucMgr = mUcMgr;
        mIsFmActive = 1;
        mDeviceList.push_back(alsa_handle);
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        mALSADevice->route(&(*it), (uint32_t)device, newMode, mTtyMode);
        if(!strcmp(it->useCase, SND_USE_CASE_VERB_DIGITAL_RADIO)) {
            snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_DIGITAL_RADIO);
        } else {
            snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_FM);
        }
        mALSADevice->startFm(&(*it));
    } else if(!(device & AudioSystem::DEVICE_OUT_FM) && mIsFmActive == 1) {
        // Stop FM Radio
        LOGV("Stop FM");
        for(ALSAHandleList::iterator it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
            if((!strcmp(it->useCase, SND_USE_CASE_VERB_DIGITAL_RADIO)) ||
              (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_FM))) {
                mALSADevice->close(&(*it));
                mDeviceList.erase(it);
                mALSADevice->route(&(*it), (uint32_t)device, newMode, mTtyMode);
                break;
            }
        }
        mIsFmActive = 0;
    } else {
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        mALSADevice->route(&(*it), (uint32_t)device, newMode, mTtyMode);
    }
}

AudioStreamOut *
AudioHardwareALSA::openOutputStream(uint32_t devices,
                                    int *format,
                                    uint32_t *channels,
                                    uint32_t *sampleRate,
                                    status_t *status)
{
    LOGV("openOutputStream: devices 0x%x channels %d sampleRate %d",
         devices, *channels, *sampleRate);

    status_t err = BAD_VALUE;
    AudioStreamOutALSA *out = 0;

    if (devices & (devices - 1)) {
        if (status) *status = err;
        LOGE("openOutputStream called with bad devices");
        return out;
    }

    alsa_handle_t alsa_handle;
    unsigned long bufferSize = DEFAULT_BUFFER_SIZE;

    for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;

    alsa_handle.module = mALSADevice;
    alsa_handle.bufferSize = bufferSize;
    alsa_handle.devices = devices;
    alsa_handle.handle = 0;
    alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
    alsa_handle.channels = DEFAULT_CHANNEL_MODE;
    alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
    alsa_handle.latency = PLAYBACK_LATENCY;
    alsa_handle.recHandle = 0;
    alsa_handle.ucMgr = mUcMgr;

    char *use_case;
    snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
    if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
        strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI, sizeof(alsa_handle.useCase));
    } else {
        strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_MUSIC, sizeof(alsa_handle.useCase));
    }
    free(use_case);
    mDeviceList.push_back(alsa_handle);
    ALSAHandleList::iterator it = mDeviceList.end();
    it--;
    LOGV("useCase %s", it->useCase);
    mALSADevice->route(&(*it), devices, mode(), mTtyMode);
    if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI)) {
        snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_HIFI);
    } else {
        snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_MUSIC);
    }
    err = mALSADevice->open(&(*it));
    if (err) {
        LOGE("Device open failed");
    } else {
        out = new AudioStreamOutALSA(this, &(*it));
        err = out->set(format, channels, sampleRate, devices);
    }

    if (status) *status = err;
    return out;
}

void
AudioHardwareALSA::closeOutputStream(AudioStreamOut* out)
{
    delete out;
}

AudioStreamOut *
AudioHardwareALSA::openOutputSession(uint32_t devices,
                                     int *format,
                                     status_t *status,
                                     int sessionId)
{
    LOGE("openOutputSession");
    AudioStreamOutALSA *out = 0;
    status_t err = BAD_VALUE;

    if (devices & (devices - 1)) {
        if (status) *status = err;
        LOGE("openOutputSession called with bad devices");
        return out;
    }

    alsa_handle_t alsa_handle;
    unsigned long bufferSize = DEFAULT_BUFFER_SIZE;

    for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;

    alsa_handle.module = mALSADevice;
    alsa_handle.bufferSize = bufferSize;
    alsa_handle.devices = devices;
    alsa_handle.handle = 0;
    alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
    alsa_handle.channels = DEFAULT_CHANNEL_MODE;
    alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
    alsa_handle.latency = VOICE_LATENCY;
    alsa_handle.recHandle = 0;
    alsa_handle.ucMgr = mUcMgr;

    char *use_case;
    snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
    if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
        strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER, sizeof(alsa_handle.useCase));
    } else {
        strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_LPA, sizeof(alsa_handle.useCase));
    }
    free(use_case);
    mDeviceList.push_back(alsa_handle);
    ALSAHandleList::iterator it = mDeviceList.end();
    it--;
    LOGV("useCase %s", it->useCase);
    mALSADevice->route(&(*it), devices, mode(), mTtyMode);
    if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) {
        snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_HIFI_LOW_POWER);
    } else {
        snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_LPA);
    }
    err = mALSADevice->open(&(*it));
    out = new AudioStreamOutALSA(this, &(*it));

    if (status) *status = err;
       return out;
}

void
AudioHardwareALSA::closeOutputSession(AudioStreamOut* out)
{
    delete out;
}

AudioStreamIn *
AudioHardwareALSA::openInputStream(uint32_t devices,
                                   int *format,
                                   uint32_t *channels,
                                   uint32_t *sampleRate,
                                   status_t *status,
                                   AudioSystem::audio_in_acoustics acoustics)
{
    char *use_case;
    int newMode = mode();

    status_t err = BAD_VALUE;
    AudioStreamInALSA *in = 0;

    LOGV("openInputStream: devices 0x%x channels %d sampleRate %d", devices, *channels, *sampleRate);
    if (devices & (devices - 1)) {
        if (status) *status = err;
        return in;
    }

    alsa_handle_t alsa_handle;
    unsigned long bufferSize = DEFAULT_IN_BUFFER_SIZE;

    for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;

    alsa_handle.module = mALSADevice;
    alsa_handle.bufferSize = bufferSize;
    alsa_handle.devices = devices;
    alsa_handle.handle = 0;
    alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
    alsa_handle.channels = VOICE_CHANNEL_MODE;
    alsa_handle.sampleRate = AudioRecord::DEFAULT_SAMPLE_RATE;
    alsa_handle.latency = RECORD_LATENCY;
    alsa_handle.recHandle = 0;
    alsa_handle.ucMgr = mUcMgr;

    snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
    if ((use_case != NULL) && (strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
        if ((devices == AudioSystem::DEVICE_IN_VOICE_CALL) &&
            (newMode == AudioSystem::MODE_IN_CALL)) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_VOICE, sizeof(alsa_handle.useCase));
        } else if((devices == AudioSystem::DEVICE_IN_FM_RX) ||
                  (devices == AudioSystem::DEVICE_IN_FM_RX_A2DP)) {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_FM, sizeof(alsa_handle.useCase));
        } else {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC, sizeof(alsa_handle.useCase));
        }
    } else {
        if ((devices == AudioSystem::DEVICE_IN_VOICE_CALL) &&
            (newMode == AudioSystem::MODE_IN_CALL)) {
            LOGE("Error opening input stream: In-call recording without voice call");
            return 0;
        } else if((devices == AudioSystem::DEVICE_IN_FM_RX) ||
                  (devices == AudioSystem::DEVICE_IN_FM_RX_A2DP)) {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_FM_REC, sizeof(alsa_handle.useCase));
        } else {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_REC, sizeof(alsa_handle.useCase));
        }
    }
    free(use_case);
    mDeviceList.push_back(alsa_handle);
    ALSAHandleList::iterator it = mDeviceList.end();
    it--;
    if ((devices & AudioSystem::DEVICE_IN_BUILTIN_MIC) && (mDmicActive == true)) {
        devices |= AudioSystem::DEVICE_IN_BACK_MIC;
    }
    mALSADevice->route(&(*it), devices, mode(), mTtyMode);
    if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC) ||
       !strcmp(it->useCase, SND_USE_CASE_VERB_FM_REC)) {
        snd_use_case_set(mUcMgr, "_verb", it->useCase);
    } else {
        snd_use_case_set(mUcMgr, "_enamod", it->useCase);
    }
    if(sampleRate) {
        it->sampleRate = *sampleRate;
    }
    if(channels) {
        it->channels = AudioSystem::popCount(*channels);
    }
    err = mALSADevice->open(&(*it));
    if (err) {
        LOGE("Error opening pcm input device");
    } else {
        in = new AudioStreamInALSA(this, &(*it), acoustics);
        err = in->set(format, channels, sampleRate, devices);
    }

    if (status) *status = err;
    return in;
}

void
AudioHardwareALSA::closeInputStream(AudioStreamIn* in)
{
    delete in;
}

status_t AudioHardwareALSA::setMicMute(bool state)
{
    if (mMicMute != state) {
        mMicMute = state;
        LOGD("setMicMute: mMicMute %d", mMicMute);
        if(mALSADevice) {
            mALSADevice->setMicMute(state);
        }
    }
    return NO_ERROR;
}

status_t AudioHardwareALSA::getMicMute(bool *state)
{
    *state = mMicMute;
    return NO_ERROR;
}

status_t AudioHardwareALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

size_t AudioHardwareALSA::getInputBufferSize(uint32_t sampleRate, int format, int channelCount)
{
    if (format != AudioSystem::PCM_16_BIT) {
        LOGW("getInputBufferSize bad format: %d", format);
        return 0;
    }
    if(sampleRate < 44100) {
        return DEFAULT_IN_BUFFER_SIZE * channelCount;
    }
    return DEFAULT_IN_BUFFER_SIZE * 8;
}

}       // namespace android
