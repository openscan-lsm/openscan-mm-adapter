#include "OpenScan.h"

#include "ModuleInterface.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
const char *const PROPERTY_Detector_Prefix = "Detector-";
const char *const PROPERTY_EnableDetector_Prefix = "LSM-EnableDetector-";
const char *const PROPERTY_Resolution = "Resolution";
const char *const PROPERTY_Magnification = "Magnification";

const char *const VALUE_Yes = "Yes";
const char *const VALUE_No = "No";

const char *const VALUE_Unselected = "Unselected";

const std::size_t MAX_DETECTOR_DEVICES = 4;

const int MIN_ADHOC_ERROR_CODE = 60001;
const int MAX_ADHOC_ERROR_CODE = 70000;

MODULE_API void InitializeModuleData() {
    if (!OSc_CheckVersion()) {
        // Unfortunately we have no way of logging the error here.
        // We could wait until the hub Initialize() is called, but that would
        // require complicating the constructor code with conditionals.
        // Instead, for now we create an empty device to report the error.
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
    const char *paths[] = {
        ".",
        std::getenv("MICROMANAGER_PATH"), // Cf. pymmcore-plus
        NULL,
    };
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

    CreateStringProperty(PROPERTY_Clock, VALUE_Unselected, false, 0, true);
    AddAllowedValue(PROPERTY_Clock, VALUE_Unselected);
    for (const auto &clk : clockDevices_) {
        AddAllowedValue(PROPERTY_Clock, clk.first.c_str());
    }

    CreateStringProperty(PROPERTY_Scanner, VALUE_Unselected, false, 0, true);
    AddAllowedValue(PROPERTY_Scanner, VALUE_Unselected);
    for (const auto &scn : scannerDevices_) {
        AddAllowedValue(PROPERTY_Scanner, scn.first.c_str());
    }

    for (std::size_t i = 0; i < MAX_DETECTOR_DEVICES; ++i) {
        const std::string propName =
            PROPERTY_Detector_Prefix + std::to_string(i);
        CreateStringProperty(propName.c_str(), VALUE_Unselected, false, 0,
                             true);
        AddAllowedValue(propName.c_str(), VALUE_Unselected);
        for (const auto &det : detectorDevices_) {
            AddAllowedValue(propName.c_str(), det.first.c_str());
        }
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

    const std::string unsel = VALUE_Unselected;

    std::vector<std::string> detectorNames;
    for (std::size_t i = 0; i < MAX_DETECTOR_DEVICES; ++i) {
        char detNam[MM::MaxStrLength + 1];
        stat = GetProperty(
            (PROPERTY_Detector_Prefix + std::to_string(i)).c_str(), detNam);
        if (stat != DEVICE_OK)
            return stat;
        if (detNam == unsel)
            continue;
        for (const auto &nam : detectorNames) {
            if (nam == detNam) {
                return AdHocErrorCode(
                    "The same detector device may not be added twice");
            }
        }
        detectorNames.push_back(detNam);
    }

    if (clockName == unsel)
        return AdHocErrorCode("Clock device must be selected");
    OSc_Device *clockDevice = clockDevices_.at(clockName);
    if (scannerName == unsel)
        return AdHocErrorCode("Scanner device must be selected");
    OSc_Device *scannerDevice = scannerDevices_.at(scannerName);
    std::vector<OSc_Device *> detectorDevices;
    for (const auto &detNam : detectorNames) {
        detectorDevices.push_back(detectorDevices_.at(detNam));
    }

    OSc_Device_SetLogFunc(clockDevice, LogOpenScan, this);
    OSc_Device_SetLogFunc(scannerDevice, LogOpenScan, this);
    for (OSc_Device *det : detectorDevices) {
        OSc_Device_SetLogFunc(det, LogOpenScan, this);
    }

    err = OSc_Device_Open(clockDevice, oscLSM_);
    if (err != OSc_OK)
        return AdHocErrorCode(err);
    if (scannerDevice != clockDevice) {
        err = OSc_Device_Open(scannerDevice, oscLSM_);
        if (err != OSc_OK)
            return AdHocErrorCode(err);
    }
    for (OSc_Device *det : detectorDevices) {
        if (det != scannerDevice && det != clockDevice) {
            err = OSc_Device_Open(det, oscLSM_);
            if (err != OSc_OK)
                return AdHocErrorCode(err);
        }
    }

    err = OSc_LSM_SetClockDevice(oscLSM_, clockDevice);
    if (err != OSc_OK)
        return AdHocErrorCode(err);
    err = OSc_LSM_SetScannerDevice(oscLSM_, scannerDevice);
    if (err != OSc_OK)
        return AdHocErrorCode(err);
    for (OSc_Device *det : detectorDevices) {
        err = OSc_LSM_AddDetectorDevice(oscLSM_, det);
        if (err != OSc_OK)
            return AdHocErrorCode(err);
    }

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
    OSc_Device *clockDevice = OSc_LSM_GetClockDevice(oscLSM_);
    OSc_Device *scannerDevice = OSc_LSM_GetScannerDevice(oscLSM_);
    std::vector<OSc_Device *> detectorDevices;
    for (std::size_t i = 0; i < OSc_LSM_GetNumberOfDetectorDevices(oscLSM_);
         ++i) {
        detectorDevices.push_back(OSc_LSM_GetDetectorDevice(oscLSM_, i));
    }

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

    for (OSc_Device *detDev : detectorDevices) {
        if (detDev != scannerDevice && detDev != clockDevice) {
            err = OSc_Device_GetSettings(detDev, &settings, &count);
            if (err != OSc_OK)
                return AdHocErrorCode(err);
            errCode = GenerateProperties(settings, count, detDev);
            if (errCode != DEVICE_OK)
                return errCode;
        }
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

    // Properties that are not OpenScan settings:
    for (std::size_t i = 0; i < detectorDevices.size(); ++i) {
        OSc_Device *detDev = detectorDevices[i];
        const char *devName;
        err = OSc_Device_GetName(detDev, &devName);
        if (err != OSc_OK)
            return AdHocErrorCode(err);
        bool enabled =
            OSc_AcqTemplate_IsDetectorDeviceEnabled(acqTemplate_, i);
        const auto propName = PROPERTY_EnableDetector_Prefix +
                              std::to_string(i) + "-" + std::string(devName);
        CPropertyActionEx *handler = new CPropertyActionEx(
            this, &OpenScan::OnEnableDetectorProperty, long(i));
        errCode = CreateStringProperty(
            propName.c_str(), enabled ? VALUE_Yes : VALUE_No, false, handler);
        if (errCode != DEVICE_OK)
            return errCode;
        errCode = AddAllowedValue(propName.c_str(), VALUE_Yes);
        if (errCode != DEVICE_OK)
            return errCode;
        errCode = AddAllowedValue(propName.c_str(), VALUE_No);
        if (errCode != DEVICE_OK)
            return errCode;
    }

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

    // To work like Multi Camera, we must include the camera channel index. The
    // metadata key for this is (for legacy reasons?) strange: it must include
    // the device label of the camera.
    // We also generate tags without the device label.
    char myLabel[MM::MaxStrLength + 1];
    GetLabel(myLabel);
    std::string deviceTaggedChannelIndex(myLabel);
    deviceTaggedChannelIndex += '-';
    deviceTaggedChannelIndex += MM::g_Keyword_CameraChannelIndex;
    std::string deviceTaggedChannelName(myLabel);
    deviceTaggedChannelName += '-';
    deviceTaggedChannelName += MM::g_Keyword_CameraChannelName;

    char chanName[MM::MaxStrLength + 1]{};
    GetChannelName(chan, chanName);

    Metadata md;
    md.put(deviceTaggedChannelIndex.c_str(), chan);
    md.put(MM::g_Keyword_CameraChannelIndex, chan);
    if (strlen(chanName) > 0) {
        md.put(deviceTaggedChannelName.c_str(), chanName);
        md.put(MM::g_Keyword_CameraChannelName, chanName);
    }

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

int OpenScan::OnEnableDetectorProperty(MM::PropertyBase *pProp,
                                       MM::ActionType eAct, long data) {
    std::size_t i = data;
    if (eAct == MM::BeforeGet) {
        bool enabled =
            OSc_AcqTemplate_IsDetectorDeviceEnabled(acqTemplate_, i);
        pProp->Set(enabled ? VALUE_Yes : VALUE_No);
    } else if (eAct == MM::AfterSet) {
        std::string valueStr;
        pProp->Get(valueStr);
        bool enable = (valueStr == VALUE_Yes);
        OSc_AcqTemplate_SetDetectorDeviceEnabled(acqTemplate_, i, enable);
    }
    return DEVICE_OK;
}

int OpenScan::AdHocErrorCode(OSc_RichError *richError) {
    if (richError == OSc_OK)
        return DEVICE_OK;

    std::string buffer;
    buffer.resize(MM::MaxStrLength);
    // buffer.data() is const until C++17
    OSc_Error_FormatRecursive(richError, &buffer[0], MM::MaxStrLength);
    OSc_Error_Destroy(richError);
    buffer.resize(std::strlen(buffer.data()));
    return AdHocErrorCode(buffer);
}

int OpenScan::AdHocErrorCode(const std::string &message) {
    int ret = nextAdHocErrorCode_++;
    if (nextAdHocErrorCode_ > MAX_ADHOC_ERROR_CODE)
        nextAdHocErrorCode_ = MIN_ADHOC_ERROR_CODE;
    SetErrorText(ret, message.c_str());
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
