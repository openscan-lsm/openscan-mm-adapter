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
const char* const DEVICE_NAME_Hub = "OScHub";
const char* const DEVICE_NAME_Camera = "OSc-LSM";
const char* const DEVICE_NAME_Ablation = "OSc-Ablation";
const char* const DEVICE_NAME_Magnifier = "OSc-Magnifier";

const char* const PROPERTY_Clock = "Clock";
const char* const PROPERTY_Scanner = "Scanner";
const char* const PROPERTY_Detector = "Detector";
const char* const PROPERTY_Resolution = "Resolution";
const char* const PROPERTY_Magnification = "Magnification";

const char* const VALUE_Yes = "Yes";
const char* const VALUE_No = "No";

const char* NoHubError = "Parent Hub not defined.";

enum
{
	ERR_SCANNER_AND_DETECTOR_REQUIRED = 30000,
};


MODULE_API void InitializeModuleData()
{
	if (!OSc_CheckVersion()) {
		// Unfortunately we have no way of logging the error here.
		// We could wait until the hub Initialize() is called, but that would
		// require complicating the constructor code with conditionals.
		// Instead, for now we create a dummy device to report the error.
		RegisterDevice("Error", MM::GenericDevice, "Incompatible OpenScanLib version");
		return;
	}

	RegisterDevice(DEVICE_NAME_Camera, MM::CameraDevice, "Laser Scanning Microscope"); // scan and imaging
	//RegisterDevice(DEVICE_NAME_Ablation, MM::SignalIODevice, "OpenScan Photo Ablation"); // DAQ analog out only
	RegisterDevice(DEVICE_NAME_Magnifier, MM::MagnifierDevice, "Pixel Size Magnifier");
	RegisterDevice(DEVICE_NAME_Hub, MM::HubDevice, "OpenScan Laser Scanning System");
}


MODULE_API MM::Device* CreateDevice(const char* deviceName)
{
	if (std::string(deviceName) == DEVICE_NAME_Camera)
		return new OpenScan();
	//else if (std::string(deviceName) == DEVICE_NAME_Ablation)
	//	return new OpenScanAO();
	else if (std::string(deviceName) == DEVICE_NAME_Magnifier)
		return new OpenScanMagnifier();
	else if (std::string(deviceName) == DEVICE_NAME_Hub)
		return new OpenScanHub();
	return 0;
}


MODULE_API void
DeleteDevice(MM::Device* device)
{
	delete device;
}


OpenScan::OpenScan() :
	oscLSM_(0),
	sequenceAcquisition_(0)
{
	// parent ID display
	CreateHubIDProperty();

	char *paths[] = {
		".",
		NULL
	};
	OSc_SetDeviceModuleSearchPaths(paths);

	size_t count;
	if (OSc_GetNumberOfAvailableDevices(&count) != OSc_Error_OK)
		return;
	OSc_Device **devices;
	if (OSc_GetAllDevices(&devices, &count) != OSc_Error_OK)
		return;
	for (size_t i = 0; i < count; ++i)
	{
		OSc_Device *device = devices[i];
		const char *name = NULL;
		if (OSc_Device_GetDisplayName(device, &name) != OSc_Error_OK ||
			!name || !name[0])
			continue;

		bool flag = false;
		if (OSc_Device_HasClock(device, &flag) == OSc_Error_OK && flag)
			clockDevices_[name] = device;
		if (OSc_Device_HasScanner(device, &flag) == OSc_Error_OK && flag)
			scannerDevices_[name] = device;
		if (OSc_Device_HasDetector(device, &flag) == OSc_Error_OK && flag)
			detectorDevices_[name] = device;
	}

	CreateStringProperty(PROPERTY_Clock, "Unselected", false, 0, true);
	AddAllowedValue(PROPERTY_Clock, "Unselected");
	for (std::map<std::string, OSc_Device*>::iterator
		it = clockDevices_.begin(), end = clockDevices_.end(); it != end; ++it)
	{
		AddAllowedValue(PROPERTY_Clock, it->first.c_str());
	}

	CreateStringProperty(PROPERTY_Scanner, "Unselected", false, 0, true);
	AddAllowedValue(PROPERTY_Scanner, "Unselected");
	for (std::map<std::string, OSc_Device*>::iterator
		it = scannerDevices_.begin(), end = scannerDevices_.end(); it != end; ++it)
	{
		AddAllowedValue(PROPERTY_Scanner, it->first.c_str());
	}

	CreateStringProperty(PROPERTY_Detector, "Unselected", false, 0, true);
	AddAllowedValue(PROPERTY_Detector, "Unselected");
	for (std::map<std::string, OSc_Device*>::iterator
		it = detectorDevices_.begin(), end = detectorDevices_.end(); it != end; ++it)
	{
		AddAllowedValue(PROPERTY_Detector, it->first.c_str());
	}
}


OpenScan::~OpenScan()
{
}


extern "C"
{
	static void LogOpenScan(const char *msg, OSc_LogLevel level, void *data)
	{
		OpenScan *self = (OpenScan *)data;
		self->LogOpenScanMessage(msg, level);
	}
}


void
OpenScan::LogOpenScanMessage(const char *msg, OSc_LogLevel level)
{
	LogMessage(msg, level <= OSc_LogLevel_Info);
}


int
OpenScan::Initialize()
{
	OpenScanHub* pHub = static_cast<OpenScanHub*>(GetParentHub());
	if (pHub)
	{
		char hubLabel[MM::MaxStrLength];
		pHub->GetLabel(hubLabel);
		SetParentID(hubLabel); // for backward comp.
	}
	else
		LogMessage(NoHubError);

	OSc_Error err = OSc_LSM_Create(&oscLSM_);
	if (err != OSc_Error_OK)
		return err;

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

	OSc_Device* clockDevice;
	OSc_Device* scannerDevice;
	OSc_Device* detectorDevice;
	std::string dummy = "Dummy Detector@Dummy detector device";
	std::string unsel = "Unselected";
	try
	{
		clockDevice = clockDevices_.at(clockName);
		scannerDevice = scannerDevices_.at(scannerName);

		std::string tmp = std::string(detectorName);
		if(unsel.compare(tmp) == 0)
			detectorDevice = detectorDevices_.at(dummy);
		else
			detectorDevice = detectorDevices_.at(detectorName);
	}
	catch (const std::exception&)
	{
		return ERR_SCANNER_AND_DETECTOR_REQUIRED;
	}

	OSc_Device_SetLogFunc(clockDevice, LogOpenScan, this);
	OSc_Device_SetLogFunc(scannerDevice, LogOpenScan, this);
	OSc_Device_SetLogFunc(detectorDevice, LogOpenScan, this);

	err = OSc_Device_Open(clockDevice, oscLSM_);
	if (err != OSc_Error_OK)
		return err;
	if (scannerDevice != clockDevice)
	{
		err = OSc_Device_Open(scannerDevice, oscLSM_);
		if (err != OSc_Error_OK)
			return err;
	}
	if (detectorDevice != scannerDevice &&
		detectorDevice != clockDevice)
	{
		err = OSc_Device_Open(detectorDevice, oscLSM_);
		if (err != OSc_Error_OK)
			return err;
	}

	OSc_Clock* clock;
	err = OSc_Device_GetClock(clockDevice, &clock);
	if (err != OSc_Error_OK)
		return err;

	OSc_Scanner* scanner;
	err = OSc_Device_GetScanner(scannerDevice, &scanner);
	if (err != OSc_Error_OK)
		return err;

	OSc_Detector* detector;
	err = OSc_Device_GetDetector(detectorDevice, &detector);
	if (err != OSc_Error_OK)
		return err;

	err = OSc_LSM_SetClock(oscLSM_, clock);
	if (err != OSc_Error_OK)
		return err;
	err = OSc_LSM_SetScanner(oscLSM_, scanner);
	if (err != OSc_Error_OK)
		return err;
	err = OSc_LSM_SetDetector(oscLSM_, detector);
	if (err != OSc_Error_OK)
		return err;

	err = InitializeResolution(clockDevice, scannerDevice, detectorDevice);
	if (err != DEVICE_OK)
		return err;

	err = GenerateProperties();
	if (err != DEVICE_OK)
		return err;

	// Standard properties Exposure and Binning - not used for LSM
	err = CreateFloatProperty(MM::g_Keyword_Exposure, 0.0, false);
	if (err != DEVICE_OK)
		return err;
	err = AddAllowedValue(MM::g_Keyword_Exposure, "0.0000"); if (err != DEVICE_OK) return err;
	err = CreateIntegerProperty(MM::g_Keyword_Binning, 1, false);
	if (err != DEVICE_OK)
		return err;
	err = AddAllowedValue(MM::g_Keyword_Binning, "1"); if (err != DEVICE_OK) return err;

	pHub->SetCameraDevice(this);

	return DEVICE_OK;
}


int
OpenScan::Shutdown()
{
	if (!oscLSM_)
		return DEVICE_OK;

	StopSequenceAcquisition();

	OpenScanHub* pHub = static_cast<OpenScanHub*>(GetParentHub());
	if (pHub)
		pHub->SetCameraDevice(0);

	OSc_LSM_Destroy(oscLSM_);
	oscLSM_ = 0;

	return DEVICE_OK;
}


int
OpenScan::InitializeResolution(OSc_Device* clockDevice, OSc_Device* scannerDevice, OSc_Device* detectorDevice)
{
	// TODO This logic for determining the intersection of resolutions allowed
	// by clock, scanner, and detector should probably live in OpenScanLib.

	std::set< std::pair<size_t, size_t> > clockResolutions, scannerResolutions, detectorResolutions;

	size_t* widths;
	size_t* heights;
	size_t nrResolutions;
	OSc_Error err;

	err = OSc_Device_GetAllowedResolutions(clockDevice,
		&widths, &heights, &nrResolutions);
	if (err != OSc_Error_OK)
		return err;
	for (size_t i = 0; i < nrResolutions; ++i)
		clockResolutions.insert(std::make_pair(widths[i], heights[i]));

	err = OSc_Device_GetAllowedResolutions(scannerDevice,
		&widths, &heights, &nrResolutions);
	if (err != OSc_Error_OK)
		return err;
	for (size_t i = 0; i < nrResolutions; ++i)
		scannerResolutions.insert(std::make_pair(widths[i], heights[i]));

	err = OSc_Device_GetAllowedResolutions(detectorDevice,
		&widths, &heights, &nrResolutions);
	if (err != OSc_Error_OK)
		return err;
	for (size_t i = 0; i < nrResolutions; ++i)
		detectorResolutions.insert(std::make_pair(widths[i], heights[i]));

	std::vector< std::pair<size_t, size_t> > sdResolutions, resolutions;
	std::set_union(
		scannerResolutions.begin(), scannerResolutions.end(),
		detectorResolutions.begin(), detectorResolutions.end(),
		std::back_inserter(sdResolutions));
	std::set_union(
		sdResolutions.begin(), sdResolutions.end(),
		clockResolutions.begin(), clockResolutions.end(),
		std::back_inserter(resolutions));
	if (resolutions.empty())
		return DEVICE_ERR; // TODO No compatible resolutions!
	std::sort(resolutions.begin(), resolutions.end());
	resolutions_ = resolutions;

	CreateStringProperty(PROPERTY_Resolution, "", false,
		new CPropertyAction(this, &OpenScan::OnResolution));

	std::vector< std::pair<size_t, size_t> >::const_iterator it, end;
	for (it = resolutions_.begin(), end = resolutions_.end(); it != end; ++it)
	{
		std::ostringstream oss;
		oss << it->first << 'x' << it->second;
		AddAllowedValue(PROPERTY_Resolution, oss.str().c_str());
	}

	// Set an initial resolution that all devices support (the first available one).
	std::pair<size_t, size_t> widthHeight = resolutions_[0];
	err = OSc_Device_SetResolution(clockDevice, widthHeight.first, widthHeight.second);
	if (err != OSc_Error_OK)
		return err;
	err = OSc_Device_SetResolution(scannerDevice, widthHeight.first, widthHeight.second);
	if (err != OSc_Error_OK)
		return err;
	err = OSc_Device_SetResolution(detectorDevice, widthHeight.first, widthHeight.second);
	if (err != OSc_Error_OK)
		return err;

	OpenScanHub* hub = static_cast<OpenScanHub*>(GetParentHub());
	if (hub)
		hub->OnMagnifierChanged();

	return DEVICE_OK;
}


int
OpenScan::GenerateProperties()
{
	OSc_Error err;
	OSc_Clock* clock;
	err = OSc_LSM_GetClock(oscLSM_, &clock);
	OSc_Scanner* scanner;
	err = OSc_LSM_GetScanner(oscLSM_, &scanner);
	OSc_Detector* detector;
	err = OSc_LSM_GetDetector(oscLSM_, &detector);
	OSc_Device* clockDevice;
	err = OSc_Clock_GetDevice(clock, &clockDevice);
	OSc_Device* scannerDevice;
	err = OSc_Scanner_GetDevice(scanner, &scannerDevice);
	OSc_Device* detectorDevice;
	err = OSc_Detector_GetDevice(detector, &detectorDevice);

	OSc_Setting** settings;
	size_t count;

	err = OSc_Device_GetSettings(clockDevice, &settings, &count);
	err = GenerateProperties(settings, count);

	if (scannerDevice != clockDevice)
	{
		err = OSc_Device_GetSettings(scannerDevice, &settings, &count);
		err = GenerateProperties(settings, count);
	}

	if (detectorDevice != scannerDevice &&
		detectorDevice != clockDevice)
	{
		err = OSc_Device_GetSettings(detectorDevice, &settings, &count);
		err = GenerateProperties(settings, count);
	}

	return DEVICE_OK;
}


int
OpenScan::GenerateProperties(OSc_Setting** settings, size_t count)
{
	OSc_Error err;
	for (size_t i = 0; i < count; ++i)
	{
		OSc_Setting* setting = settings[i];

		long index = static_cast<long>(settingIndex_.size());
		settingIndex_.push_back(setting);

		char name[OSc_MAX_STR_LEN + 1];
		err = OSc_Setting_GetName(setting, name);
		OSc_ValueType valueType;
		err = OSc_Setting_GetValueType(setting, &valueType);
		bool writable;
		err = OSc_Setting_IsWritable(setting, &writable);

		switch (valueType)
		{
			case OSc_ValueType_String:
			{
				char value[OSc_MAX_STR_LEN + 1];
				err = OSc_Setting_GetStringValue(setting, value);
				CPropertyActionEx* handler = new CPropertyActionEx(this,
					&OpenScan::OnStringProperty, index);
				err = CreateStringProperty(name, value, !writable, handler);
				break;
			}
			case OSc_ValueType_Bool:
			{
				bool value;
				err = OSc_Setting_GetBoolValue(setting, &value);
				CPropertyActionEx* handler = new CPropertyActionEx(this,
					&OpenScan::OnBoolProperty, index);
				err = CreateStringProperty(name, value ? VALUE_Yes : VALUE_No, !writable, handler);
				err = AddAllowedValue(name, VALUE_Yes);
				err = AddAllowedValue(name, VALUE_No);
				break;
			}
			case OSc_ValueType_Int32:
			{
				int32_t value;
				err = OSc_Setting_GetInt32Value(setting, &value);
				CPropertyActionEx* handler = new CPropertyActionEx(this,
					&OpenScan::OnInt32Property, index);
				err = CreateIntegerProperty(name, value, !writable, handler);
				OSc_ValueConstraint constraint;
				err = OSc_Setting_GetNumericConstraintType(setting, &constraint);
				switch (constraint)
				{
				case OSc_ValueConstraint_Discrete:
					int32_t* values;
					size_t numValues;
					err = OSc_Setting_GetInt32DiscreteValues(setting, &values, &numValues);
					for (int j = 0; j < numValues; ++j)
					{
						char valueStr[OSc_MAX_STR_LEN + 1];
						snprintf(valueStr, OSc_MAX_STR_LEN, "%d", values[j]);
						err = AddAllowedValue(name, valueStr);
					}
					break;
				case OSc_ValueConstraint_Continuous:
					int32_t min, max;
					err = OSc_Setting_GetInt32ContinuousRange(setting, &min, &max);
					SetPropertyLimits(name, min, max);
					break;
				}
				break;
			}
			case OSc_ValueType_Float64:
			{
				double value;
				err = OSc_Setting_GetFloat64Value(setting, &value);
				CPropertyActionEx* handler = new CPropertyActionEx(this,
					&OpenScan::OnFloat64Property, index);
				err = CreateFloatProperty(name, value, !writable, handler);
				OSc_ValueConstraint constraint;
				err = OSc_Setting_GetNumericConstraintType(setting, &constraint);
				switch (constraint)
				{
				case OSc_ValueConstraint_Discrete:
					double* values;
					size_t numValues;
					err = OSc_Setting_GetFloat64DiscreteValues(setting, &values, &numValues);
					for (int j = 0; j < numValues; ++j)
					{
						char valueStr[OSc_MAX_STR_LEN + 1];
						snprintf(valueStr, OSc_MAX_STR_LEN, "%0.4f", values[j]);
						err = AddAllowedValue(name, valueStr);
					}
					break;
				case OSc_ValueConstraint_Continuous:
					double min, max;
					err = OSc_Setting_GetFloat64ContinuousRange(setting, &min, &max);
					SetPropertyLimits(name, min, max);
					break;
				}
				break;
			}
			case OSc_ValueType_Enum:
			{
				uint32_t value;
				err = OSc_Setting_GetEnumValue(setting, &value);
				char valueStr[OSc_MAX_STR_LEN + 1];
				err = OSc_Setting_GetEnumNameForValue(setting, value, valueStr);
				CPropertyActionEx* handler = new CPropertyActionEx(this,
					&OpenScan::OnEnumProperty, index);
				err = CreateStringProperty(name, valueStr, !writable, handler);
				uint32_t numValues;
				err = OSc_Setting_GetEnumNumValues(setting, &numValues);
				for (uint32_t j = 0; j < numValues; ++j)
				{
					err = OSc_Setting_GetEnumNameForValue(setting, j, valueStr);
					err = AddAllowedValue(name, valueStr);
				}
				break;
			}
		}
	}
	return DEVICE_OK;
}


int OpenScan::GetMagnification(double *magnification)
{
	OSc_Error err;

	// for now only consider scanner as pixel size is determined by scan waveform in LSM
	OSc_Scanner* scanner;
	err = OSc_LSM_GetScanner(oscLSM_, &scanner);
	if (err)
		return err;
	OSc_Device* scannerDevice;
	err = OSc_Scanner_GetDevice(scanner, &scannerDevice);
	if (err)
		return err;

	err = OSc_Device_GetMagnification(scannerDevice, magnification);
	if (err)
		return err;
	char msg[OSc_MAX_STR_LEN + 1];
	snprintf(msg, OSc_MAX_STR_LEN, "Updated magnification is: %6.2f", *magnification);
	LogMessage(msg, true);
	//LogMessage(("Updated magnification: " + boost::lexical_cast<std::string>(*magnification) + "x").c_str(), true);

	return DEVICE_OK;
}


bool
OpenScan::Busy()
{
	return false;
}


void
OpenScan::GetName(char* name) const
{
	CDeviceUtils::CopyLimitedString(name, DEVICE_NAME_Camera);
}


extern "C"
{
	static bool
	SnapFrameCallback(OSc_Acquisition* acq, uint32_t chan, void* pixels, void* data)
	{
		OpenScan* self = static_cast<OpenScan*>(data);
		self->StoreSnapImage(acq, chan, pixels);
		return true;
	}
}


int
OpenScan::SnapImage()
{
	if (IsCapturing())
		return DEVICE_CAMERA_BUSY_ACQUIRING;

	DiscardPreviouslySnappedImages();

	OSc_Acquisition* acq;
	OSc_Error err = OSc_Acquisition_Create(&acq, oscLSM_);

	err = OSc_Acquisition_SetData(acq, this);
	err = OSc_Acquisition_SetNumberOfFrames(acq, 1);

	err = OSc_Acquisition_SetFrameCallback(acq, SnapFrameCallback);

	err = OSc_Acquisition_Arm(acq);
	err = OSc_Acquisition_Start(acq);
	err = OSc_Acquisition_Wait(acq);
	err = OSc_Acquisition_Destroy(acq);

	return DEVICE_OK;
}


void
OpenScan::StoreSnapImage(OSc_Acquisition*, uint32_t chan, void* pixels)
{
	size_t bufSize = GetImageBufferSize();
	void* buffer = malloc(bufSize);
	memcpy(buffer, pixels, bufSize);

	if (snappedImages_.size() < chan + 1)
		snappedImages_.resize(chan + 1, 0);
	if (snappedImages_[chan])
		free(snappedImages_[chan]);
	snappedImages_[chan] = buffer;
}


void
OpenScan::DiscardPreviouslySnappedImages()
{
	for (std::vector<void*>::iterator
		it = snappedImages_.begin(), end = snappedImages_.end(); it != end; ++it)
	{
		if (*it)
			free(*it);
	}
	snappedImages_.clear();
}


const unsigned char*
OpenScan::GetImageBuffer(unsigned chan)
{
	if (chan >= GetNumberOfChannels() || chan >= snappedImages_.size())
		return 0;
	return reinterpret_cast<unsigned char*>(snappedImages_[chan]);
}


long
OpenScan::GetImageBufferSize() const
{
	return GetImageWidth() * GetImageHeight() * GetImageBytesPerPixel();
}


unsigned
OpenScan::GetImageWidth() const
{
	OSc_Detector* detector;
	OSc_LSM_GetDetector(oscLSM_, &detector);
	uint32_t width, height;
	OSc_Detector_GetImageSize(detector, &width, &height);
	return width;
}


unsigned
OpenScan::GetImageHeight() const
{
	OSc_Detector* detector;
	OSc_LSM_GetDetector(oscLSM_, &detector);
	uint32_t width, height;
	OSc_Detector_GetImageSize(detector, &width, &height);
	return height;
}


unsigned
OpenScan::GetImageBytesPerPixel() const
{
	OSc_Detector* detector;
	OSc_LSM_GetDetector(oscLSM_, &detector);
	uint32_t bps;
	OSc_Detector_GetBytesPerSample(detector, &bps);
	return bps;
}


unsigned
OpenScan::GetNumberOfComponents() const
{
	return 1;
}


unsigned
OpenScan::GetNumberOfChannels() const
{
	OSc_Detector* detector;
	OSc_LSM_GetDetector(oscLSM_, &detector);
	uint32_t nChannels;
	OSc_Detector_GetNumberOfChannels(detector, &nChannels);
	return nChannels;
}


int
OpenScan::GetChannelName(unsigned channel, char* name)
{
	snprintf(name, MM::MaxStrLength, "OpenScanChannel-%u", channel);
	return DEVICE_OK;
}


unsigned
OpenScan::GetBitDepth() const
{
	// TODO Get from OpenScan
	return 16;
}


int
OpenScan::SetROI(unsigned, unsigned, unsigned, unsigned)
{
	if (IsCapturing())
		return DEVICE_CAMERA_BUSY_ACQUIRING;

	return DEVICE_OK;
}


int
OpenScan::GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize)
{
	x = y = 0;
	xSize = GetImageWidth();
	ySize = GetImageHeight();
	return DEVICE_OK;
}


int
OpenScan::ClearROI()
{
	if (IsCapturing())
		return DEVICE_CAMERA_BUSY_ACQUIRING;

	return DEVICE_OK;
}


extern "C"
{
	static bool
	SequenceFrameCallback(OSc_Acquisition* acq, uint32_t chan, void* pixels, void* data)
	{
		OpenScan* self = static_cast<OpenScan*>(data);
		return self->SendSequenceImage(acq, chan, pixels);
	}
}


int
OpenScan::StartSequenceAcquisition(long count, double, bool stopOnOverflow)
{
	// I cannot think of a reasonable situation 
	// when IsCapturing is false while sequenceAcquisition_ is true.
	// possibly it means previous live mode is not stopped properly
	// anyway remove sequenceAcquisition_ from if ocndition for now
	// TODO: need to fully test whether this change is valid?
	if (IsCapturing())
		return DEVICE_CAMERA_BUSY_ACQUIRING;

	if (count < 1)
		return DEVICE_OK;

	OSc_Acquisition* acq;
	OSc_Error err = OSc_Acquisition_Create(&acq, oscLSM_);

	err = OSc_Acquisition_SetData(acq, this);
	err = OSc_Acquisition_SetNumberOfFrames(acq, count);

	err = OSc_Acquisition_SetFrameCallback(acq, SequenceFrameCallback);

	err = OSc_Acquisition_Arm(acq);
	GetCoreCallback()->PrepareForAcq(this);
	err = OSc_Acquisition_Start(acq);

	sequenceAcquisition_ = acq;
	sequenceAcquisitionStopOnOverflow_ = stopOnOverflow;

	return DEVICE_OK;
}


int
OpenScan::StopSequenceAcquisition()
{
	if (!oscLSM_)
		return DEVICE_OK;

	if (!IsCapturing() || !sequenceAcquisition_)
		return DEVICE_OK;

	OSc_Error err = OSc_Acquisition_Stop(sequenceAcquisition_);
	GetCoreCallback()->AcqFinished(this, DEVICE_OK);
	err = OSc_Acquisition_Destroy(sequenceAcquisition_);
	sequenceAcquisition_ = 0;

	return DEVICE_OK;
}


bool
OpenScan::SendSequenceImage(OSc_Acquisition*, uint32_t chan, void* pixels)
{
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
	unsigned char* p = static_cast<unsigned char*>(pixels);
	int err = GetCoreCallback()->InsertImage(this, p, width, height, bytesPerPixel, md.Serialize().c_str());
	if (!sequenceAcquisitionStopOnOverflow_ && err == DEVICE_BUFFER_OVERFLOW)
	{
		GetCoreCallback()->ClearImageBuffer(this);
		err = GetCoreCallback()->InsertImage(this, p, width, height, bytesPerPixel, md.Serialize().c_str(), false);
		return err == DEVICE_OK;
	}
	else if (err != DEVICE_OK)
	{
		return false;
	}
	return true;
}


bool
OpenScan::IsCapturing()
{
	if (!oscLSM_)
		return false;

	bool isRunning;
	OSc_Error err = OSc_LSM_IsRunningAcquisition(oscLSM_, &isRunning);
	if (err != OSc_Error_OK)
		return false;
	return isRunning;
}


int
OpenScan::OnResolution(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	// TODO The logic for getting and setting resolution for all devices
	// should be moved into OpenScanLib

	OSc_Error err;

	OSc_Clock* clock;
	err = OSc_LSM_GetClock(oscLSM_, &clock);
	if (err)
		return err;
	OSc_Scanner* scanner;
	err = OSc_LSM_GetScanner(oscLSM_, &scanner);
	if (err)
		return err;
	OSc_Detector* detector;
	err = OSc_LSM_GetDetector(oscLSM_, &detector);
	if (err)
		return err;
	OSc_Device* clockDevice;
	err = OSc_Clock_GetDevice(clock, &clockDevice);
	if (err)
		return err;
	OSc_Device* scannerDevice;
	err = OSc_Scanner_GetDevice(scanner, &scannerDevice);
	if (err)
		return err;
	OSc_Device* detectorDevice;
	err = OSc_Detector_GetDevice(detector, &detectorDevice);
	if (err)
		return err;

	if (eAct == MM::BeforeGet)
	{
		size_t cWidth, cHeight;
		err = OSc_Device_GetResolution(clockDevice, &cWidth, &cHeight);
		if (err)
			return err;

		size_t sWidth, sHeight;
		if (scannerDevice == clockDevice)
		{
			sWidth = cWidth;
			sHeight = cHeight;
		}
		else
		{
			err = OSc_Device_GetResolution(scannerDevice, &sWidth, &sHeight);
			if (err)
				return err;
		}

		size_t dWidth, dHeight;
		if (detectorDevice == clockDevice)
		{
			dWidth = cWidth;
			dHeight = cHeight;
		}
		else if (detectorDevice == scannerDevice)
		{
			dWidth = sWidth;
			dHeight = sHeight;
		}
		else
		{
			err = OSc_Device_GetResolution(detectorDevice, &dWidth, &dHeight);
			if (err)
				return err;
		}

		if (cWidth != sWidth || cWidth != dWidth ||
			cHeight != sHeight || cHeight != dHeight)
		{
			// For some reason, resolutions aren't matched.
			pProp->Set("");
			return DEVICE_OK;
		}

		std::ostringstream oss;
		oss << cWidth << 'x' << cHeight;
		pProp->Set(oss.str().c_str());
	}
	else if (eAct == MM::AfterSet)
	{
		std::string s;
		pProp->Get(s);

		size_t width, height;
		std::string::size_type xPosition = s.find('x');
		std::istringstream wiss(s.substr(0, xPosition));
		wiss >> width;
		std::istringstream hiss(s.substr(xPosition + 1));
		hiss >> height;

		err = OSc_Device_SetResolution(clockDevice, width, height);
		if (err)
			return err;
		if (scannerDevice != clockDevice)
		{
			err = OSc_Device_SetResolution(scannerDevice, width, height);
			if (err)
				return err;
		}
		if (detectorDevice != scannerDevice &&
			detectorDevice != clockDevice)
		{
			err = OSc_Device_SetResolution(detectorDevice, width, height);
			if (err)
				return err;
		}

		OpenScanHub* hub = static_cast<OpenScanHub*>(GetParentHub());
		if (hub)
			hub->OnMagnifierChanged();
	}
	return DEVICE_OK;
}


int
OpenScan::OnStringProperty(MM::PropertyBase* pProp, MM::ActionType eAct, long data)
{
	OSc_Error err;
	OSc_Setting* setting = settingIndex_[data];
	if (eAct == MM::BeforeGet)
	{
		char value[OSc_MAX_STR_LEN + 1];
		err = OSc_Setting_GetStringValue(setting, value);
		pProp->Set(value);
	}
	else if (eAct == MM::AfterSet)
	{
		std::string value;
		pProp->Get(value);
		err = OSc_Setting_SetStringValue(setting, value.c_str());
	}
	return DEVICE_OK;
}


int
OpenScan::OnBoolProperty(MM::PropertyBase* pProp, MM::ActionType eAct, long data)
{
	OSc_Error err;
	OSc_Setting* setting = settingIndex_[data];
	if (eAct == MM::BeforeGet)
	{
		bool value;
		err = OSc_Setting_GetBoolValue(setting, &value);
		pProp->Set(value ? VALUE_Yes : VALUE_No);
	}
	else if (eAct == MM::AfterSet)
	{
		std::string value;
		pProp->Get(value);
		err = OSc_Setting_SetBoolValue(setting, value == VALUE_Yes);
	}
	return DEVICE_OK;
}


int
OpenScan::OnInt32Property(MM::PropertyBase* pProp, MM::ActionType eAct, long data)
{
	OSc_Error err;
	OSc_Setting* setting = settingIndex_[data];
	if (eAct == MM::BeforeGet)
	{
		int32_t value;
		err = OSc_Setting_GetInt32Value(setting, &value);
		pProp->Set(static_cast<long>(value));
	}
	else if (eAct == MM::AfterSet)
	{
		long value;
		pProp->Get(value);
		err = OSc_Setting_SetInt32Value(setting, static_cast<int32_t>(value));
	}
	return DEVICE_OK;
}


int
OpenScan::OnFloat64Property(MM::PropertyBase* pProp, MM::ActionType eAct, long data)
{
	OSc_Error err;
	OSc_Setting* setting = settingIndex_[data];
	if (eAct == MM::BeforeGet)
	{
		double value;
		err = OSc_Setting_GetFloat64Value(setting, &value);
		pProp->Set(value);
	}
	else if (eAct == MM::AfterSet)
	{
		double value;
		pProp->Get(value);
		err = OSc_Setting_SetFloat64Value(setting, value);
		if (err)
			return err;

		// TEMPORARY: Special handling for Zoom change, which affect magnification.
		// A proper interface should be added to OpenScan C API that allows us to
		// subscribe to setting changes.
		char name[1024];
		err = OSc_Setting_GetName(setting, name);
		if (err)
			return err;
		if (strcmp(name, "Zoom") == 0)
		{
			OpenScanHub* hub = static_cast<OpenScanHub*>(GetParentHub());
			if (hub)
				hub->OnMagnifierChanged();
		}
	}
	return DEVICE_OK;
}


int
OpenScan::OnEnumProperty(MM::PropertyBase* pProp, MM::ActionType eAct, long data)
{
	OSc_Error err;
	OSc_Setting* setting = settingIndex_[data];
	if (eAct == MM::BeforeGet)
	{
		uint32_t value;
		err = OSc_Setting_GetEnumValue(setting, &value);
		char valueStr[OSc_MAX_STR_LEN + 1];
		err = OSc_Setting_GetEnumNameForValue(setting, value, valueStr);
		pProp->Set(valueStr);
	}
	else if (eAct == MM::AfterSet)
	{
		std::string valueStr;
		pProp->Get(valueStr);
		uint32_t value;
		err = OSc_Setting_GetEnumValueForName(setting, &value, valueStr.c_str());
		err = OSc_Setting_SetEnumValue(setting, value);
	}
	return DEVICE_OK;
}


int OpenScanHub::Initialize()
{
	return DEVICE_OK;
}


void OpenScanHub::GetName(char * pName) const
{
	CDeviceUtils::CopyLimitedString(pName, DEVICE_NAME_Hub);
}


int OpenScanHub::DetectInstalledDevices()
{
	MM::Device* camera = CreateDevice(DEVICE_NAME_Camera);
	if (camera)
		AddInstalledDevice(camera);
	MM::Device* magnifier = CreateDevice(DEVICE_NAME_Magnifier);
	if (magnifier)
		AddInstalledDevice(magnifier);
	return DEVICE_OK;
}

void OpenScanHub::SetCameraDevice(OpenScan* camera)
{
	openScanCamera_ = camera;
}

void OpenScanHub::SetMagnificationChangeNotifier(OpenScanMagnifier* magnifier,
	MagChangeNotifierType notifier)
{
	magnifier_ = magnifier;
	magChangeNotifier_ = notifier;
}

int OpenScanHub::GetMagnification(double* mag)
{
	if (!openScanCamera_)
		return DEVICE_ERR;
	return openScanCamera_->GetMagnification(mag);
}

int OpenScanHub::OnMagnifierChanged()
{
	if (!magChangeNotifier_ || !magnifier_) {
		return DEVICE_OK;
	}

	int err = (magnifier_->*magChangeNotifier_)();
	if (err != DEVICE_OK)
		return err;

	return DEVICE_OK;
}


////////////////////////////////////////////////////////////
// TODO: SingleIO device to control a second pair of galvos
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
OpenScanAO::OpenScanAO()
{
}

OpenScanAO::~OpenScanAO()
{
}

int OpenScanAO::Initialize()
{
	return 0;
}

int OpenScanAO::Shutdown()
{
	return 0;
}

void OpenScanAO::GetName(char * name) const
{
}

int OpenScanAO::SetGateOpen(bool open)
{
	return 0;
}

int OpenScanAO::GetGateOpen(bool & open)
{
	return 0;
}

int OpenScanAO::SetSignal(double volts)
{
	return 0;
}

int OpenScanAO::GetLimits(double & minVolts, double & maxVolts)
{
	return 0;
}


OpenScanMagnifier::OpenScanMagnifier()
{
}

void OpenScanMagnifier::GetName(char* name) const
{
	CDeviceUtils::CopyLimitedString(name, DEVICE_NAME_Magnifier);
}

int OpenScanMagnifier::Initialize()
{
	OpenScanHub* pHub = static_cast<OpenScanHub*>(GetParentHub());
	if (pHub)
	{
		char hubLabel[MM::MaxStrLength];
		pHub->GetLabel(hubLabel);
		SetParentID(hubLabel); // for backward comp.
	}
	else
		LogMessage(NoHubError);

	pHub->SetMagnificationChangeNotifier(this, &OpenScanMagnifier::HandleMagnificationChange);

	return DEVICE_OK;
}

int OpenScanMagnifier::Shutdown()
{
	OpenScanHub* pHub = static_cast<OpenScanHub*>(GetParentHub());
	if (pHub)
		pHub->SetMagnificationChangeNotifier(0, 0);
	return DEVICE_OK;
}

double OpenScanMagnifier::GetMagnification() {
	OpenScanHub* pHub = static_cast<OpenScanHub*>(GetParentHub());
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
