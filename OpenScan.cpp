#include "OpenScan.h"

#include "ModuleInterface.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <iterator>
#include <set>
#include <sstream>
#include <utility>

// External names used by the rest of the system
// to load particular device from the "OpenScan.dll" library
const char *const DEVICE_NAME_Hub = "OScHub";
const char *const DEVICE_NAME_Camera = "OSc-LSM";
const char *const DEVICE_NAME_Magnifier = "OSc-Magnifier";

const char *const PROPERTY_Clock = "Clock";
const char *const PROPERTY_Scanner = "Scanner";
const char *const PROPERTY_Detector = "Detector";
const char *const PROPERTY_Resolution = "Resolution";
const char *const PROPERTY_Magnification = "Magnification";

const char *const VALUE_Yes = "Yes";
const char *const VALUE_No = "No";

const char *NoHubError = "Parent Hub not defined.";

const int MIN_ADHOC_ERROR_CODE = 60001;
const int MAX_ADHOC_ERROR_CODE = 70000;

enum {
    ERR_SCANNER_AND_DETECTOR_REQUIRED = 30000,
};

MODULE_API void InitializeModuleData() {
    if (!OSc_CheckVersion()) {
        // Unfortunately we have no way of logging the error here.
        // We could wait until the hub Initialize() is called, but that would
        // require complicating the constructor code with conditionals.
        // Instead, for now we create a dummy device to report the error.
        RegisterDevice("Error", MM::GenericDevice,
                       "Incompatible OpenScanLib version");
        return;
    }

    RegisterDevice(DEVICE_NAME_Hub, MM::HubDevice,
                   "OpenScan Laser Scanning System");
}

MODULE_API MM::Device *CreateDevice(const char *deviceName) {
    if (std::string(deviceName) == DEVICE_NAME_Camera)
        return new OpenScan();
    else if (std::string(deviceName) == DEVICE_NAME_Magnifier)
        return new OpenScanMagnifier();
    else if (std::string(deviceName) == DEVICE_NAME_Hub)
        return new OpenScanHub();
    return 0;
}

MODULE_API void DeleteDevice(MM::Device *device) { delete device; }

OpenScan::OpenScan()
    : nextAdHocErrorCode_(MIN_ADHOC_ERROR_CODE), oscLSM_(0), acqTemplate_(0),
      sequenceAcquisition_(0), sequenceAcquisitionStopOnOverflow_(false) {
    const char *paths[] = {".", NULL};
    OSc_SetDeviceModuleSearchPaths(paths);

    size_t count;
    if (OSc_GetNumberOfAvailableDevices(&count) != OSc_OK)
        return;
    OSc_Device **devices;
    if (OSc_GetAllDevices(&devices, &count) != OSc_OK)
        return;
    for (size_t i = 0; i < count; ++i) {
        OSc_Device *device = devices[i];
        const char *name = NULL;
        if (OSc_Device_GetDisplayName(device, &name) != OSc_OK || !name ||
            !name[0])
            continue;

        bool flag = false;
        if (OSc_Device_HasClock(device, &flag) == OSc_OK && flag)
            clockDevices_[name] = device;
        if (OSc_Device_HasScanner(device, &flag) == OSc_OK && flag)
            scannerDevices_[name] = device;
        if (OSc_Device_HasDetector(device, &flag) == OSc_OK && flag)
            detectorDevices_[name] = device;
    }

    CreateStringProperty(PROPERTY_Clock, "Unselected", false, 0, true);
    AddAllowedValue(PROPERTY_Clock, "Unselected");
    for (std::map<std::string, OSc_Device *>::iterator
             it = clockDevices_.begin(),
             end = clockDevices_.end();
         it != end; ++it) {
        AddAllowedValue(PROPERTY_Clock, it->first.c_str());
    }

    CreateStringProperty(PROPERTY_Scanner, "Unselected", false, 0, true);
    AddAllowedValue(PROPERTY_Scanner, "Unselected");
    for (std::map<std::string, OSc_Device *>::iterator
             it = scannerDevices_.begin(),
             end = scannerDevices_.end();
         it != end; ++it) {
        AddAllowedValue(PROPERTY_Scanner, it->first.c_str());
    }

    CreateStringProperty(PROPERTY_Detector, "Unselected", false, 0, true);
    AddAllowedValue(PROPERTY_Detector, "Unselected");
    for (std::map<std::string, OSc_Device *>::iterator
             it = detectorDevices_.begin(),
             end = detectorDevices_.end();
         it != end; ++it) {
        AddAllowedValue(PROPERTY_Detector, it->first.c_str());
    }
}

OpenScan::~OpenScan() {}

extern "C" {
static void LogOpenScan(const char *msg, OSc_LogLevel level, void *data) {
    OpenScan *self = (OpenScan *)data;
    self->LogOpenScanMessage(msg, level);
}
}

void OpenScan::LogOpenScanMessage(const char *msg, OSc_LogLevel level) {
    LogMessage(msg, level <= OSc_LogLevel_Info);
}

static void MagChangeCallback(OSc_Setting *, void *hub) {
    static_cast<OpenScanHub *>(hub)->OnMagnifierChanged();
}

int OpenScan::Initialize() {
    OSc_RichError *err = OSc_LSM_Create(&oscLSM_);
    if (err != OSc_OK)
        return AdHocErrorCode(err);

    char clockName[MM::MaxStrLength + 1];
    int stat = GetProperty(PROPERTY_Clock, clockName);
    if (stat != DEVICE_OK)
        return stat;

    char scannerName[MM::MaxStrLength + 1];
    stat = GetProperty(PROPERTY_Scanner, scannerName);
    if (stat != DEVICE_OK)
        return stat;

    char detectorName[MM::MaxStrLength + 1];
    stat = GetProperty(PROPERTY_Detector, detectorName);
    if (stat != DEVICE_OK)
        return stat;

    OSc_Device *clockDevice;
    OSc_Device *scannerDevice;
    OSc_Device *detectorDevice;
    std::string dummy = "Dummy Detector@Dummy detector device";
    std::string unsel = "Unselected";
    try {
        clockDevice = clockDevices_.at(clockName);
        scannerDevice = scannerDevices_.at(scannerName);

        std::string tmp = std::string(detectorName);
        if (unsel.compare(tmp) == 0)
            detectorDevice = detectorDevices_.at(dummy);
        else
            detectorDevice = detectorDevices_.at(detectorName);
    } catch (const std::exception &) {
        return ERR_SCANNER_AND_DETECTOR_REQUIRED;
    }

    OSc_Device_SetLogFunc(clockDevice, LogOpenScan, this);
    OSc_Device_SetLogFunc(scannerDevice, LogOpenScan, this);
    OSc_Device_SetLogFunc(detectorDevice, LogOpenScan, this);

    err = OSc_Device_Open(clockDevice, oscLSM_);
    if (err != OSc_OK)
        return AdHocErrorCode(err);
    if (scannerDevice != clockDevice) {
        err = OSc_Device_Open(scannerDevice, oscLSM_);
        if (err != OSc_OK)
            return AdHocErrorCode(err);
    }
    if (detectorDevice != scannerDevice && detectorDevice != clockDevice) {
        err = OSc_Device_Open(detectorDevice, oscLSM_);
        if (err != OSc_OK)
            return AdHocErrorCode(err);
    }

    err = OSc_LSM_SetClockDevice(oscLSM_, clockDevice);
    if (err != OSc_OK)
        return AdHocErrorCode(err);
    err = OSc_LSM_SetScannerDevice(oscLSM_, scannerDevice);
    if (err != OSc_OK)
        return AdHocErrorCode(err);
    err = OSc_LSM_SetDetectorDevice(oscLSM_, detectorDevice);
    if (err != OSc_OK)
        return AdHocErrorCode(err);

    err = OSc_AcqTemplate_Create(&acqTemplate_, oscLSM_);
    if (err != OSc_OK)
        return AdHocErrorCode(err);

    int errCode = GenerateProperties();
    if (errCode != DEVICE_OK)
        return errCode;

    // Register callback for magnification change
    OSc_Setting *magSetting;
    err = OSc_AcqTemplate_GetMagnificationSetting(acqTemplate_, &magSetting);
    if (err != OSc_OK)
        return AdHocErrorCode(err);
    OSc_Setting_SetInvalidateCallback(magSetting, MagChangeCallback,
                                      GetParentHub());

    // Standard properties Exposure and Binning - not used for LSM
    errCode = CreateFloatProperty(MM::g_Keyword_Exposure, 0.0, false);
    if (errCode != DEVICE_OK)
        return errCode;
    errCode = AddAllowedValue(MM::g_Keyword_Exposure, "0.0000");
    if (errCode != DEVICE_OK)
        return errCode;
    errCode = CreateIntegerProperty(MM::g_Keyword_Binning, 1, false);
    if (errCode != DEVICE_OK)
        return errCode;
    errCode = AddAllowedValue(MM::g_Keyword_Binning, "1");
    if (errCode != DEVICE_OK)
        return errCode;

    OpenScanHub *pHub = static_cast<OpenScanHub *>(GetParentHub());
    pHub->SetCameraDevice(this);

    return DEVICE_OK;
}

int OpenScan::Shutdown() {
    if (!oscLSM_)
        return DEVICE_OK;

    StopSequenceAcquisition();

    OpenScanHub *pHub = static_cast<OpenScanHub *>(GetParentHub());
    if (pHub)
        pHub->SetCameraDevice(0);

    OSc_LSM_Destroy(oscLSM_);
    oscLSM_ = 0;

    return DEVICE_OK;
}

int OpenScan::GenerateProperties() {
    // TODO Property names should be prefixed with device name

    OSc_Device *clockDevice = OSc_LSM_GetClockDevice(oscLSM_);
    OSc_Device *scannerDevice = OSc_LSM_GetScannerDevice(oscLSM_);
    OSc_Device *detectorDevice = OSc_LSM_GetDetectorDevice(oscLSM_);

    OSc_Setting **settings;
    size_t count;

    OSc_RichError *err;
    int errCode;
    err = OSc_Device_GetSettings(clockDevice, &settings, &count);
    if (err != OSc_OK)
        return AdHocErrorCode(err);
    errCode = GenerateProperties(settings, count, clockDevice);
    if (errCode != DEVICE_OK)
        return errCode;

    if (scannerDevice != clockDevice) {
        err = OSc_Device_GetSettings(scannerDevice, &settings, &count);
        if (err != OSc_OK)
            return AdHocErrorCode(err);
        errCode = GenerateProperties(settings, count, scannerDevice);
        if (errCode != DEVICE_OK)
            return errCode;
    }

    if (detectorDevice != scannerDevice && detectorDevice != clockDevice) {
        err = OSc_Device_GetSettings(detectorDevice, &settings, &count);
        if (err != OSc_OK)
            return AdHocErrorCode(err);
        errCode = GenerateProperties(settings, count, detectorDevice);
        if (errCode != DEVICE_OK)
            return errCode;
    }

    OSc_Setting *acqSettings[3];
    err = OSc_AcqTemplate_GetPixelRateSetting(acqTemplate_, &acqSettings[0]);
    if (err != OSc_OK)
        return AdHocErrorCode(err);
    err = OSc_AcqTemplate_GetResolutionSetting(acqTemplate_, &acqSettings[1]);
    if (err != OSc_OK)
        return AdHocErrorCode(err);
    err = OSc_AcqTemplate_GetZoomFactorSetting(acqTemplate_, &acqSettings[2]);
    if (err != OSc_OK)
        return AdHocErrorCode(err);
    errCode = GenerateProperties(acqSettings, 3, NULL);
    if (errCode != DEVICE_OK)
        return errCode;

    return DEVICE_OK;
}

int OpenScan::GenerateProperties(OSc_Setting **settings, size_t count,
                                 OSc_Device *device) {
    OSc_RichError *err;
    int errCode;
    for (size_t i = 0; i < count; ++i) {
        OSc_Setting *setting = settings[i];

        long index = static_cast<long>(settingIndex_.size());
        settingIndex_.push_back(setting);

        const char *device_name;
        char setting_name[OSc_MAX_STR_LEN + 1];

        if (!device) {
            device_name = "LSM";
        } else {
            err = OSc_Device_GetName(device, &device_name);
            if (err != OSc_OK)
                return AdHocErrorCode(err);
        }

        err = OSc_Setting_GetName(setting, setting_name);
        if (err != OSc_OK)
            return AdHocErrorCode(err);

        char name[OSc_MAX_STR_LEN + 1];
        snprintf(name, OSc_MAX_STR_LEN + 1, "%s-%s", device_name,
                 setting_name);

        OSc_ValueType valueType;
        err = OSc_Setting_GetValueType(setting, &valueType);
        if (err != OSc_OK)
            return AdHocErrorCode(err);
        bool writable;
        err = OSc_Setting_IsWritable(setting, &writable);
        if (err != OSc_OK)
            return AdHocErrorCode(err);

        switch (valueType) {
        case OSc_ValueType_String: {
            char value[OSc_MAX_STR_LEN + 1];
            err = OSc_Setting_GetStringValue(setting, value);
            if (err != OSc_OK)
                return AdHocErrorCode(err);
            CPropertyActionEx *handler = new CPropertyActionEx(
                this, &OpenScan::OnStringProperty, index);
            errCode = CreateStringProperty(name, value, !writable, handler);
            if (errCode != DEVICE_OK)
                return errCode;
            break;
        }
        case OSc_ValueType_Bool: {
            bool value;
            err = OSc_Setting_GetBoolValue(setting, &value);
            if (err != OSc_OK)
                return AdHocErrorCode(err);
            CPropertyActionEx *handler =
                new CPropertyActionEx(this, &OpenScan::OnBoolProperty, index);
            errCode = CreateStringProperty(name, value ? VALUE_Yes : VALUE_No,
                                           !writable, handler);
            if (errCode != DEVICE_OK)
                return errCode;
            errCode = AddAllowedValue(name, VALUE_Yes);
            if (errCode != DEVICE_OK)
                return errCode;
            errCode = AddAllowedValue(name, VALUE_No);
            if (errCode != DEVICE_OK)
                return errCode;
            break;
        }
        case OSc_ValueType_Int32: {
            int32_t value;
            err = OSc_Setting_GetInt32Value(setting, &value);
            if (err != OSc_OK)
                return AdHocErrorCode(err);
            CPropertyActionEx *handler =
                new CPropertyActionEx(this, &OpenScan::OnInt32Property, index);
            errCode = CreateIntegerProperty(name, value, !writable, handler);
            if (errCode != DEVICE_OK)
                return errCode;
            OSc_ValueConstraint constraint;
            err = OSc_Setting_GetNumericConstraintType(setting, &constraint);
            if (err != OSc_OK)
                return AdHocErrorCode(err);
            switch (constraint) {
            case OSc_ValueConstraint_Discrete:
                int32_t *values;
                size_t numValues;
                err = OSc_Setting_GetInt32DiscreteValues(setting, &values,
                                                         &numValues);
                if (err != OSc_OK)
                    return AdHocErrorCode(err);
                for (int j = 0; j < numValues; ++j) {
                    char valueStr[OSc_MAX_STR_LEN + 1];
                    snprintf(valueStr, OSc_MAX_STR_LEN, "%d", values[j]);
                    errCode = AddAllowedValue(name, valueStr);
                    if (errCode != DEVICE_OK)
                        return errCode;
                }
                break;
            case OSc_ValueConstraint_Continuous:
                int32_t min, max;
                err = OSc_Setting_GetInt32ContinuousRange(setting, &min, &max);
                if (err != OSc_OK)
                    return AdHocErrorCode(err);
                SetPropertyLimits(name, min, max);
                break;
            }
            break;
        }
        case OSc_ValueType_Float64: {
            double value;
            err = OSc_Setting_GetFloat64Value(setting, &value);
            if (err != OSc_OK)
                return AdHocErrorCode(err);
            CPropertyActionEx *handler = new CPropertyActionEx(
                this, &OpenScan::OnFloat64Property, index);
            errCode = CreateFloatProperty(name, value, !writable, handler);
            if (errCode != DEVICE_OK)
                return errCode;
            OSc_ValueConstraint constraint;
            err = OSc_Setting_GetNumericConstraintType(setting, &constraint);
            if (err != OSc_OK)
                return AdHocErrorCode(err);
            switch (constraint) {
            case OSc_ValueConstraint_Discrete:
                double *values;
                size_t numValues;
                err = OSc_Setting_GetFloat64DiscreteValues(setting, &values,
                                                           &numValues);
                for (int j = 0; j < numValues; ++j) {
                    char valueStr[OSc_MAX_STR_LEN + 1];
                    snprintf(valueStr, OSc_MAX_STR_LEN, "%0.4f", values[j]);
                    errCode = AddAllowedValue(name, valueStr);
                    if (errCode != DEVICE_OK)
                        return errCode;
                }
                break;
            case OSc_ValueConstraint_Continuous:
                double min, max;
                err =
                    OSc_Setting_GetFloat64ContinuousRange(setting, &min, &max);
                SetPropertyLimits(name, min, max);
                break;
            }
            break;
        }
        case OSc_ValueType_Enum: {
            uint32_t value;
            err = OSc_Setting_GetEnumValue(setting, &value);
            char valueStr[OSc_MAX_STR_LEN + 1];
            err = OSc_Setting_GetEnumNameForValue(setting, value, valueStr);
            CPropertyActionEx *handler =
                new CPropertyActionEx(this, &OpenScan::OnEnumProperty, index);
            errCode = CreateStringProperty(name, valueStr, !writable, handler);
            if (errCode != DEVICE_OK)
                return errCode;
            uint32_t numValues;
            err = OSc_Setting_GetEnumNumValues(setting, &numValues);
            if (err != OSc_OK)
                return AdHocErrorCode(err);
            for (uint32_t j = 0; j < numValues; ++j) {
                err = OSc_Setting_GetEnumNameForValue(setting, j, valueStr);
                if (err != OSc_OK)
                    return AdHocErrorCode(err);
                errCode = AddAllowedValue(name, valueStr);
                if (errCode != DEVICE_OK)
                    return errCode;
            }
            break;
        }
        }
    }
    return DEVICE_OK;
}

int OpenScan::GetMagnification(double *magnification) {
    // We define magnification 1.0 as default resolution at Zoom 1.0.

    OSc_RichError *err;
    OSc_Setting *magSetting;
    if (OSc_CHECK_ERROR(err, OSc_AcqTemplate_GetMagnificationSetting(
                                 acqTemplate_, &magSetting))) {
        return AdHocErrorCode(err);
    }
    return AdHocErrorCode(
        OSc_Setting_GetFloat64Value(magSetting, magnification));
}

bool OpenScan::Busy() { return false; }

void OpenScan::GetName(char *name) const {
    CDeviceUtils::CopyLimitedString(name, DEVICE_NAME_Camera);
}

extern "C" {
static bool SnapFrameCallback(OSc_Acquisition *acq, uint32_t chan,
                              void *pixels, void *data) {
    OpenScan *self = static_cast<OpenScan *>(data);
    self->StoreSnapImage(acq, chan, pixels);
    return true;
}
}

int OpenScan::SnapImage() {
    if (IsCapturing())
        return DEVICE_CAMERA_BUSY_ACQUIRING;

    DiscardPreviouslySnappedImages();

    OSc_Acquisition *acq;
    OSc_RichError *err = OSc_Acquisition_Create(&acq, acqTemplate_);
    if (err)
        return AdHocErrorCode(err);

    err = OSc_Acquisition_SetData(acq, this);
    if (err)
        goto error;

    err = OSc_Acquisition_SetNumberOfFrames(acq, 1);
    if (err)
        goto error;

    err = OSc_Acquisition_SetFrameCallback(acq, SnapFrameCallback);
    if (err)
        goto error;

    err = OSc_Acquisition_Arm(acq);
    if (err)
        goto error;

    err = OSc_Acquisition_Start(acq);
    if (err)
        goto error;

    err = OSc_Acquisition_Wait(acq);
    if (err)
        goto error;

    OSc_Acquisition_Destroy(acq);

    return DEVICE_OK;

error:
    int errCode = AdHocErrorCode(err);
    OSc_Acquisition_Destroy(acq);
    return errCode;
}

void OpenScan::StoreSnapImage(OSc_Acquisition *, uint32_t chan, void *pixels) {
    size_t bufSize = GetImageBufferSize();
    void *buffer = malloc(bufSize);
    memcpy(buffer, pixels, bufSize);

    if (snappedImages_.size() < chan + 1)
        snappedImages_.resize(chan + 1, 0);
    if (snappedImages_[chan])
        free(snappedImages_[chan]);
    snappedImages_[chan] = buffer;
}

void OpenScan::DiscardPreviouslySnappedImages() {
    for (std::vector<void *>::iterator it = snappedImages_.begin(),
                                       end = snappedImages_.end();
         it != end; ++it) {
        if (*it)
            free(*it);
    }
    snappedImages_.clear();
}

const unsigned char *OpenScan::GetImageBuffer(unsigned chan) {
    if (chan >= GetNumberOfChannels() || chan >= snappedImages_.size())
        return 0;
    return reinterpret_cast<unsigned char *>(snappedImages_[chan]);
}

long OpenScan::GetImageBufferSize() const {
    return GetImageWidth() * GetImageHeight() * GetImageBytesPerPixel();
}

unsigned OpenScan::GetImageWidth() const {
    uint32_t xOffset, yOffset, width, height;
    OSc_AcqTemplate_GetROI(acqTemplate_, &xOffset, &yOffset, &width, &height);
    return width;
}

unsigned OpenScan::GetImageHeight() const {
    uint32_t xOffset, yOffset, width, height;
    OSc_AcqTemplate_GetROI(acqTemplate_, &xOffset, &yOffset, &width, &height);
    return height;
}

unsigned OpenScan::GetImageBytesPerPixel() const {
    uint32_t bps;
    OSc_AcqTemplate_GetBytesPerSample(acqTemplate_, &bps);
    return bps;
}

unsigned OpenScan::GetNumberOfComponents() const { return 1; }

unsigned OpenScan::GetNumberOfChannels() const {
    uint32_t nChannels;
    OSc_AcqTemplate_GetNumberOfChannels(acqTemplate_, &nChannels);
    return nChannels;
}

int OpenScan::GetChannelName(unsigned channel, char *name) {
    snprintf(name, MM::MaxStrLength, "OpenScanChannel-%u", channel);
    return DEVICE_OK;
}

unsigned OpenScan::GetBitDepth() const {
    // TODO Get from OpenScan
    return 16;
}

int OpenScan::SetROI(unsigned x, unsigned y, unsigned width, unsigned height) {
    return AdHocErrorCode(
        OSc_AcqTemplate_SetROI(acqTemplate_, x, y, width, height));
}

int OpenScan::GetROI(unsigned &x, unsigned &y, unsigned &xSize,
                     unsigned &ySize) {
    return AdHocErrorCode(
        OSc_AcqTemplate_GetROI(acqTemplate_, &x, &y, &xSize, &ySize));
}

int OpenScan::ClearROI() {
    OSc_AcqTemplate_ResetROI(acqTemplate_);
    return DEVICE_OK;
}

extern "C" {
static bool SequenceFrameCallback(OSc_Acquisition *acq, uint32_t chan,
                                  void *pixels, void *data) {
    OpenScan *self = static_cast<OpenScan *>(data);
    return self->SendSequenceImage(acq, chan, pixels);
}
}

int OpenScan::StartSequenceAcquisition(long count, double,
                                       bool stopOnOverflow) {
    // I cannot think of a reasonable situation
    // when IsCapturing is false while sequenceAcquisition_ is true.
    // possibly it means previous live mode is not stopped properly
    // anyway remove sequenceAcquisition_ from if ocndition for now
    // TODO: need to fully test whether this change is valid?
    if (IsCapturing())
        return DEVICE_CAMERA_BUSY_ACQUIRING;

    if (count < 1)
        return DEVICE_OK;

    OSc_Acquisition *acq;
    OSc_RichError *err = OSc_Acquisition_Create(&acq, acqTemplate_);

    err = OSc_Acquisition_SetData(acq, this);
    if (err)
        return AdHocErrorCode(err);
    err = OSc_Acquisition_SetNumberOfFrames(acq, count);
    if (err)
        return AdHocErrorCode(err);

    err = OSc_Acquisition_SetFrameCallback(acq, SequenceFrameCallback);
    if (err)
        return AdHocErrorCode(err);

    err = OSc_Acquisition_Arm(acq);
    if (err)
        return AdHocErrorCode(err);
    GetCoreCallback()->PrepareForAcq(this);

    err = OSc_Acquisition_Start(acq);
    if (err)
        return AdHocErrorCode(err);

    sequenceAcquisition_ = acq;
    sequenceAcquisitionStopOnOverflow_ = stopOnOverflow;

    return DEVICE_OK;
}

int OpenScan::StopSequenceAcquisition() {
    if (!oscLSM_)
        return DEVICE_OK;

    if (!IsCapturing() || !sequenceAcquisition_)
        return DEVICE_OK;

    OSc_RichError *err = OSc_Acquisition_Stop(sequenceAcquisition_);
    GetCoreCallback()->AcqFinished(this, DEVICE_OK);
    err = OSc_Acquisition_Destroy(sequenceAcquisition_);
    sequenceAcquisition_ = 0;

    return DEVICE_OK;
}

bool OpenScan::SendSequenceImage(OSc_Acquisition *, uint32_t chan,
                                 void *pixels) {
    char cameraName[MM::MaxStrLength];
    GetChannelName(chan, cameraName);

    Metadata md;
    md.put("Camera", cameraName);
    md.put("CameraName", cameraName);
    md.put("ChannelName", cameraName);
    md.put("Channel", cameraName);
    md.put("CameraChannelIndex", chan);
    md.put("ChannelIndex", chan);
    unsigned width = GetImageWidth();
    unsigned height = GetImageHeight();
    unsigned bytesPerPixel = GetImageBytesPerPixel();
    unsigned char *p = static_cast<unsigned char *>(pixels);
    int err = GetCoreCallback()->InsertImage(
        this, p, width, height, bytesPerPixel, md.Serialize().c_str());
    if (!sequenceAcquisitionStopOnOverflow_ && err == DEVICE_BUFFER_OVERFLOW) {
        GetCoreCallback()->ClearImageBuffer(this);
        err = GetCoreCallback()->InsertImage(this, p, width, height,
                                             bytesPerPixel,
                                             md.Serialize().c_str(), false);
        return err == DEVICE_OK;
    } else if (err != DEVICE_OK) {
        return false;
    }
    return true;
}

bool OpenScan::IsCapturing() {
    if (!oscLSM_)
        return false;

    bool isRunning;
    OSc_RichError *err = OSc_LSM_IsRunningAcquisition(oscLSM_, &isRunning);
    if (err != OSc_OK)
        return false;
    return isRunning;
}

int OpenScan::OnStringProperty(MM::PropertyBase *pProp, MM::ActionType eAct,
                               long data) {
    OSc_RichError *err;
    OSc_Setting *setting = settingIndex_[data];
    if (eAct == MM::BeforeGet) {
        char value[OSc_MAX_STR_LEN + 1];
        err = OSc_Setting_GetStringValue(setting, value);
        pProp->Set(value);
    } else if (eAct == MM::AfterSet) {
        std::string value;
        pProp->Get(value);
        err = OSc_Setting_SetStringValue(setting, value.c_str());
    }
    return DEVICE_OK;
}

int OpenScan::OnBoolProperty(MM::PropertyBase *pProp, MM::ActionType eAct,
                             long data) {
    OSc_RichError *err;
    OSc_Setting *setting = settingIndex_[data];
    if (eAct == MM::BeforeGet) {
        bool value;
        err = OSc_Setting_GetBoolValue(setting, &value);
        pProp->Set(value ? VALUE_Yes : VALUE_No);
    } else if (eAct == MM::AfterSet) {
        std::string value;
        pProp->Get(value);
        err = OSc_Setting_SetBoolValue(setting, value == VALUE_Yes);
    }
    return DEVICE_OK;
}

int OpenScan::OnInt32Property(MM::PropertyBase *pProp, MM::ActionType eAct,
                              long data) {
    OSc_RichError *err;
    OSc_Setting *setting = settingIndex_[data];
    if (eAct == MM::BeforeGet) {
        int32_t value;
        err = OSc_Setting_GetInt32Value(setting, &value);
        pProp->Set(static_cast<long>(value));
    } else if (eAct == MM::AfterSet) {
        long value;
        pProp->Get(value);
        err = OSc_Setting_SetInt32Value(setting, static_cast<int32_t>(value));
    }
    return DEVICE_OK;
}

int OpenScan::OnFloat64Property(MM::PropertyBase *pProp, MM::ActionType eAct,
                                long data) {
    OSc_RichError *err;
    OSc_Setting *setting = settingIndex_[data];
    if (eAct == MM::BeforeGet) {
        double value;
        err = OSc_Setting_GetFloat64Value(setting, &value);
        pProp->Set(value);
    } else if (eAct == MM::AfterSet) {
        double value;
        pProp->Get(value);
        err = OSc_Setting_SetFloat64Value(setting, value);
        if (err)
            return AdHocErrorCode(err);

        // TEMPORARY: Special handling for Zoom change, which affect
        // magnification. A proper interface should be added to OpenScan C API
        // that allows us to subscribe to resolutionSetting changes.
        char name[1024];
        err = OSc_Setting_GetName(setting, name);
        if (err)
            return AdHocErrorCode(err);
    }
    return DEVICE_OK;
}

int OpenScan::OnEnumProperty(MM::PropertyBase *pProp, MM::ActionType eAct,
                             long data) {
    OSc_RichError *err;
    OSc_Setting *setting = settingIndex_[data];
    if (eAct == MM::BeforeGet) {
        uint32_t value;
        err = OSc_Setting_GetEnumValue(setting, &value);
        char valueStr[OSc_MAX_STR_LEN + 1];
        err = OSc_Setting_GetEnumNameForValue(setting, value, valueStr);
        pProp->Set(valueStr);
    } else if (eAct == MM::AfterSet) {
        std::string valueStr;
        pProp->Get(valueStr);
        uint32_t value;
        err =
            OSc_Setting_GetEnumValueForName(setting, &value, valueStr.c_str());
        err = OSc_Setting_SetEnumValue(setting, value);
    }
    return DEVICE_OK;
}

int OpenScan::AdHocErrorCode(OSc_RichError *richError) {
    if (richError == OSc_OK) {
        return DEVICE_OK;
    }
    int ret = nextAdHocErrorCode_++;
    if (nextAdHocErrorCode_ > MAX_ADHOC_ERROR_CODE)
        nextAdHocErrorCode_ = MIN_ADHOC_ERROR_CODE;

    char buffer[MM::MaxStrLength];
    OSc_Error_FormatRecursive(richError, buffer, sizeof(buffer));
    SetErrorText(ret, buffer);
    OSc_Error_Destroy(richError);
    return ret;
}

int OpenScanHub::Initialize() { return DEVICE_OK; }

void OpenScanHub::GetName(char *pName) const {
    CDeviceUtils::CopyLimitedString(pName, DEVICE_NAME_Hub);
}

int OpenScanHub::DetectInstalledDevices() {
    MM::Device *camera = CreateDevice(DEVICE_NAME_Camera);
    if (camera)
        AddInstalledDevice(camera);
    MM::Device *magnifier = CreateDevice(DEVICE_NAME_Magnifier);
    if (magnifier)
        AddInstalledDevice(magnifier);
    return DEVICE_OK;
}

void OpenScanHub::SetCameraDevice(OpenScan *camera) {
    openScanCamera_ = camera;
}

void OpenScanHub::SetMagnificationChangeNotifier(
    OpenScanMagnifier *magnifier, MagChangeNotifierType notifier) {
    magnifier_ = magnifier;
    magChangeNotifier_ = notifier;
}

int OpenScanHub::GetMagnification(double *mag) {
    if (!openScanCamera_)
        return DEVICE_ERR;
    return openScanCamera_->GetMagnification(mag);
}

int OpenScanHub::OnMagnifierChanged() {
    if (!magChangeNotifier_ || !magnifier_) {
        return DEVICE_OK;
    }

    int err = (magnifier_->*magChangeNotifier_)();
    if (err != DEVICE_OK)
        return err;

    return DEVICE_OK;
}

OpenScanMagnifier::OpenScanMagnifier() {}

void OpenScanMagnifier::GetName(char *name) const {
    CDeviceUtils::CopyLimitedString(name, DEVICE_NAME_Magnifier);
}

int OpenScanMagnifier::Initialize() {
    OpenScanHub *pHub = static_cast<OpenScanHub *>(GetParentHub());
    pHub->SetMagnificationChangeNotifier(
        this, &OpenScanMagnifier::HandleMagnificationChange);

    return DEVICE_OK;
}

int OpenScanMagnifier::Shutdown() {
    OpenScanHub *pHub = static_cast<OpenScanHub *>(GetParentHub());
    if (pHub)
        pHub->SetMagnificationChangeNotifier(0, 0);
    return DEVICE_OK;
}

double OpenScanMagnifier::GetMagnification() {
    OpenScanHub *pHub = static_cast<OpenScanHub *>(GetParentHub());
    if (!pHub)
        return 0.0;

    double mag;
    int err = pHub->GetMagnification(&mag);
    if (err != DEVICE_OK)
        return 0.0;
    return mag;
}

int OpenScanMagnifier::HandleMagnificationChange() {
    return OnMagnifierChanged();
}
