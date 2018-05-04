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

	size_t count;
	if (OSc_Devices_Get_Count(&count) != OSc_Error_OK)
		return;
	OSc_Device **devices;
	if (OSc_Devices_Get_All(&devices, &count) != OSc_Error_OK)
		return;
	for (size_t i = 0; i < count; ++i)
	{
		OSc_Device *device = devices[i];
		const char *name = NULL;
		if (OSc_Device_Get_Display_Name(device, &name) != OSc_Error_OK ||
			!name || !name[0])
			continue;

		bool flag = false;
		if (OSc_Device_Has_Scanner(device, &flag) == OSc_Error_OK && flag)
			scannerDevices_[name] = device;
		if (OSc_Device_Has_Detector(device, &flag) == OSc_Error_OK && flag)
			detectorDevices_[name] = device;
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
	static void LogOpenScan(const char *msg, OSc_Log_Level level, void *data)
	{
		OpenScan *self = (OpenScan *)data;
		self->LogOpenScanMessage(msg, level);
	}
}


void
OpenScan::LogOpenScanMessage(const char *msg, OSc_Log_Level level)
{
	LogMessage(msg, level <= OSc_Log_Level_Info);
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

	char scannerName[MM::MaxStrLength + 1];
	int stat = GetProperty(PROPERTY_Scanner, scannerName);
	if (stat != DEVICE_OK)
		return stat;

	char detectorName[MM::MaxStrLength + 1];
	stat = GetProperty(PROPERTY_Detector, detectorName);
	if (stat != DEVICE_OK)
		return stat;

	OSc_Device* scannerDevice;
	OSc_Device* detectorDevice;
	try
	{
		scannerDevice = scannerDevices_.at(scannerName);
		detectorDevice = detectorDevices_.at(detectorName);
	}
	catch (const std::exception&)
	{
		return ERR_SCANNER_AND_DETECTOR_REQUIRED;
	}

	OSc_Log_Set_Device_Log_Func(scannerDevice, LogOpenScan, this);

	err = OSc_Device_Open(scannerDevice, oscLSM_);
	if (err != OSc_Error_OK)
		return err;
	if (detectorDevice != scannerDevice)
	{
		err = OSc_Device_Open(detectorDevice, oscLSM_);
		if (err != OSc_Error_OK)
			return err;
	}

	OSc_Scanner* scanner;
	err = OSc_Device_Get_Scanner(scannerDevice, &scanner);
	if (err != OSc_Error_OK)
		return err;

	OSc_Detector* detector;
	err = OSc_Device_Get_Detector(detectorDevice, &detector);

	err = OSc_LSM_Set_Scanner(oscLSM_, scanner);
	if (err != OSc_Error_OK)
		return err;
	err = OSc_LSM_Set_Detector(oscLSM_, detector);
	if (err != OSc_Error_OK)
		return err;

	err = InitializeResolution(scannerDevice, detectorDevice);
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
OpenScan::InitializeResolution(OSc_Device* scannerDevice, OSc_Device* detectorDevice)
{
	std::set< std::pair<size_t, size_t> > scannerResolutions, detectorResolutions;

	size_t* widths;
	size_t* heights;
	size_t nrResolutions;
	OSc_Error err = OSc_Device_Get_Allowed_Resolutions(scannerDevice,
		&widths, &heights, &nrResolutions);
	if (err != OSc_Error_OK)
		return err;
	for (size_t i = 0; i < nrResolutions; ++i)
		scannerResolutions.insert(std::make_pair(widths[i], heights[i]));

	err = OSc_Device_Get_Allowed_Resolutions(detectorDevice,
		&widths, &heights, &nrResolutions);
	if (err != OSc_Error_OK)
		return err;
	for (size_t i = 0; i < nrResolutions; ++i)
		detectorResolutions.insert(std::make_pair(widths[i], heights[i]));

	std::vector< std::pair<size_t, size_t> > resolutions;
	std::set_union(
		scannerResolutions.begin(), scannerResolutions.end(),
		detectorResolutions.begin(), detectorResolutions.end(),
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

	// Set a resolution that both the scanner and detector support.
	// Default to the scanner's default if allowed, otherwise the first allowed.
	if (scannerDevice != detectorDevice)
	{
		size_t width, height;
		err = OSc_Device_Get_Resolution(scannerDevice, &width, &height);
		if (detectorResolutions.count(std::make_pair(width, height)))
		{
			err = OSc_Device_Set_Resolution(detectorDevice, width, height);
			if (err != OSc_Error_OK)
				return err;
		}
		else
		{
			std::pair<size_t, size_t> widthHeight = resolutions_[0];
			err = OSc_Device_Set_Resolution(scannerDevice, widthHeight.first, widthHeight.second);
			if (err != OSc_Error_OK)
				return err;
			err = OSc_Device_Set_Resolution(detectorDevice, widthHeight.first, widthHeight.second);
			if (err != OSc_Error_OK)
				return err;
		}

		OpenScanHub* hub = static_cast<OpenScanHub*>(GetParentHub());
		if (hub)
			hub->OnMagnifierChanged();
	}

	return DEVICE_OK;
}


int
OpenScan::GenerateProperties()
{
	OSc_Error err;
	OSc_Scanner* scanner;
	err = OSc_LSM_Get_Scanner(oscLSM_, &scanner);
	OSc_Detector* detector;
	err = OSc_LSM_Get_Detector(oscLSM_, &detector);
	OSc_Device* scannerDevice;
	err = OSc_Scanner_Get_Device(scanner, &scannerDevice);
	OSc_Device* detectorDevice;
	err = OSc_Detector_Get_Device(detector, &detectorDevice);

	OSc_Setting** settings;
	size_t count;
	err = OSc_Device_Get_Settings(scannerDevice, &settings, &count);
	err = GenerateProperties(settings, count);

	if (detectorDevice != scannerDevice)
	{
		err = OSc_Device_Get_Settings(detectorDevice, &settings, &count);
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
		err = OSc_Setting_Get_Name(setting, name);
		OSc_Value_Type valueType;
		err = OSc_Setting_Get_Value_Type(setting, &valueType);
		bool writable;
		err = OSc_Setting_Is_Writable(setting, &writable);

		switch (valueType)
		{
			case OSc_Value_Type_String:
			{
				char value[OSc_MAX_STR_LEN + 1];
				err = OSc_Setting_Get_String_Value(setting, value);
				CPropertyActionEx* handler = new CPropertyActionEx(this,
					&OpenScan::OnStringProperty, index);
				err = CreateStringProperty(name, value, !writable, handler);
				break;
			}
			case OSc_Value_Type_Bool:
			{
				bool value;
				err = OSc_Setting_Get_Bool_Value(setting, &value);
				CPropertyActionEx* handler = new CPropertyActionEx(this,
					&OpenScan::OnBoolProperty, index);
				err = CreateStringProperty(name, value ? VALUE_Yes : VALUE_No, !writable, handler);
				err = AddAllowedValue(name, VALUE_Yes);
				err = AddAllowedValue(name, VALUE_No);
				break;
			}
			case OSc_Value_Type_Int32:
			{
				int32_t value;
				err = OSc_Setting_Get_Int32_Value(setting, &value);
				CPropertyActionEx* handler = new CPropertyActionEx(this,
					&OpenScan::OnInt32Property, index);
				err = CreateIntegerProperty(name, value, !writable, handler);
				OSc_Value_Constraint constraint;
				err = OSc_Setting_Get_Numeric_Constraint_Type(setting, &constraint);
				switch (constraint)
				{
				case OSc_Value_Constraint_Discrete_Values:
					int32_t* values;
					size_t numValues;
					err = OSc_Setting_Get_Int32_Discrete_Values(setting, &values, &numValues);
					for (int j = 0; j < numValues; ++j)
					{
						char valueStr[OSc_MAX_STR_LEN + 1];
						snprintf(valueStr, OSc_MAX_STR_LEN, "%d", values[j]);
						err = AddAllowedValue(name, valueStr);
					}
					break;
				case OSc_Value_Constraint_Range:
					int32_t min, max;
					err = OSc_Setting_Get_Int32_Range(setting, &min, &max);
					SetPropertyLimits(name, min, max);
					break;
				}
				break;
			}
			case OSc_Value_Type_Float64:
			{
				double value;
				err = OSc_Setting_Get_Float64_Value(setting, &value);
				CPropertyActionEx* handler = new CPropertyActionEx(this,
					&OpenScan::OnFloat64Property, index);
				err = CreateFloatProperty(name, value, !writable, handler);
				OSc_Value_Constraint constraint;
				err = OSc_Setting_Get_Numeric_Constraint_Type(setting, &constraint);
				switch (constraint)
				{
				case OSc_Value_Constraint_Discrete_Values:
					double* values;
					size_t numValues;
					err = OSc_Setting_Get_Float64_Discrete_Values(setting, &values, &numValues);
					for (int j = 0; j < numValues; ++j)
					{
						char valueStr[OSc_MAX_STR_LEN + 1];
						snprintf(valueStr, OSc_MAX_STR_LEN, "%0.4f", values[j]);
						err = AddAllowedValue(name, valueStr);
					}
					break;
				case OSc_Value_Constraint_Range:
					double min, max;
					err = OSc_Setting_Get_Float64_Range(setting, &min, &max);
					SetPropertyLimits(name, min, max);
					break;
				}
				break;
			}
			case OSc_Value_Type_Enum:
			{
				uint32_t value;
				err = OSc_Setting_Get_Enum_Value(setting, &value);
				char valueStr[OSc_MAX_STR_LEN + 1];
				err = OSc_Setting_Get_Enum_Name_For_Value(setting, value, valueStr);
				CPropertyActionEx* handler = new CPropertyActionEx(this,
					&OpenScan::OnEnumProperty, index);
				err = CreateStringProperty(name, valueStr, !writable, handler);
				uint32_t numValues;
				err = OSc_Setting_Get_Enum_Num_Values(setting, &numValues);
				for (uint32_t j = 0; j < numValues; ++j)
				{
					err = OSc_Setting_Get_Enum_Name_For_Value(setting, j, valueStr);
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
	err = OSc_LSM_Get_Scanner(oscLSM_, &scanner);
	if (err)
		return err;
	OSc_Device* scannerDevice;
	err = OSc_Scanner_Get_Device(scanner, &scannerDevice);
	if (err)
		return err;

	err = OSc_Device_Get_Magnification(scannerDevice, magnification);
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

	err = OSc_Acquisition_Set_Data(acq, this);
	err = OSc_Acquisition_Set_Number_Of_Frames(acq, 1);

	// TODO Other trigger sources (or should this be fixed for a given LSM?)
	err = OSc_Acquisition_Set_Trigger_Source(acq, OSc_Trigger_Source_Scanner);

	err = OSc_Acquisition_Set_Frame_Callback(acq, SnapFrameCallback);

	err = OSc_Acquisition_Arm(acq);
	err = OSc_Acquisition_Start(acq);
	err = OSc_Acquisition_Wait(acq);

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
	OSc_LSM_Get_Detector(oscLSM_, &detector);
	uint32_t width, height;
	OSc_Detector_Get_Image_Size(detector, &width, &height);
	return width;
}


unsigned
OpenScan::GetImageHeight() const
{
	OSc_Detector* detector;
	OSc_LSM_Get_Detector(oscLSM_, &detector);
	uint32_t width, height;
	OSc_Detector_Get_Image_Size(detector, &width, &height);
	return height;
}


unsigned
OpenScan::GetImageBytesPerPixel() const
{
	OSc_Detector* detector;
	OSc_LSM_Get_Detector(oscLSM_, &detector);
	uint32_t bps;
	OSc_Detector_Get_Bytes_Per_Sample(detector, &bps);
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
	OSc_LSM_Get_Detector(oscLSM_, &detector);
	uint32_t nChannels;
	OSc_Detector_Get_Number_Of_Channels(detector, &nChannels);
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

	err = OSc_Acquisition_Set_Data(acq, this);
	err = OSc_Acquisition_Set_Number_Of_Frames(acq, count);

	// TODO Other trigger sources (or should this be fixed for a given LSM?)
	err = OSc_Acquisition_Set_Trigger_Source(acq, OSc_Trigger_Source_Scanner);

	err = OSc_Acquisition_Set_Frame_Callback(acq, SequenceFrameCallback);

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
	else
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
	OSc_Error err = OSc_LSM_Is_Running_Acquisition(oscLSM_, &isRunning);
	if (err != OSc_Error_OK)
		return false;
	return isRunning;
}


int
OpenScan::OnResolution(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	OSc_Error err;
	OSc_Scanner* scanner;
	err = OSc_LSM_Get_Scanner(oscLSM_, &scanner);
	if (err)
		return err;
	OSc_Detector* detector;
	err = OSc_LSM_Get_Detector(oscLSM_, &detector);
	if (err)
		return err;
	OSc_Device* scannerDevice;
	err = OSc_Scanner_Get_Device(scanner, &scannerDevice);
	if (err)
		return err;
	OSc_Device* detectorDevice;
	err = OSc_Detector_Get_Device(detector, &detectorDevice);
	if (err)
		return err;

	if (eAct == MM::BeforeGet)
	{
		size_t width, height;
		err = OSc_Device_Get_Resolution(scannerDevice, &width, &height);
		if (err)
			return err;
		if (detectorDevice != scannerDevice)
		{
			size_t dWidth, dHeight;
			err = OSc_Device_Get_Resolution(detectorDevice, &dWidth, &dHeight);
			if (err)
				return err;
			if (dWidth != width || dHeight != height)
			{
				pProp->Set("");
				return DEVICE_OK;
			}
		}

		std::ostringstream oss;
		oss << width << 'x' << height;
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

		err = OSc_Device_Set_Resolution(scannerDevice, width, height);
		if (err)
			return err;
		if (detectorDevice != scannerDevice)
		{
			err = OSc_Device_Set_Resolution(detectorDevice, width, height);
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
		err = OSc_Setting_Get_String_Value(setting, value);
		pProp->Set(value);
	}
	else if (eAct == MM::AfterSet)
	{
		std::string value;
		pProp->Get(value);
		err = OSc_Setting_Set_String_Value(setting, value.c_str());
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
		err = OSc_Setting_Get_Bool_Value(setting, &value);
		pProp->Set(value ? VALUE_Yes : VALUE_No);
	}
	else if (eAct == MM::AfterSet)
	{
		std::string value;
		pProp->Get(value);
		err = OSc_Setting_Set_Bool_Value(setting, value == VALUE_Yes);
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
		err = OSc_Setting_Get_Int32_Value(setting, &value);
		pProp->Set(static_cast<long>(value));
	}
	else if (eAct == MM::AfterSet)
	{
		long value;
		pProp->Get(value);
		err = OSc_Setting_Set_Int32_Value(setting, static_cast<int32_t>(value));
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
		err = OSc_Setting_Get_Float64_Value(setting, &value);
		pProp->Set(value);
	}
	else if (eAct == MM::AfterSet)
	{
		double value;
		pProp->Get(value);
		err = OSc_Setting_Set_Float64_Value(setting, value);
		if (err)
			return err;

		// TEMPORARY: Special handling for Zoom change, which affect magnification.
		// A proper interface should be added to OpenScan C API that allows us to
		// subscribe to setting changes.
		char name[1024];
		err = OSc_Setting_Get_Name(setting, name);
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
		err = OSc_Setting_Get_Enum_Value(setting, &value);
		char valueStr[OSc_MAX_STR_LEN + 1];
		err = OSc_Setting_Get_Enum_Name_For_Value(setting, value, valueStr);
		pProp->Set(valueStr);
	}
	else if (eAct == MM::AfterSet)
	{
		std::string valueStr;
		pProp->Get(valueStr);
		uint32_t value;
		err = OSc_Setting_Get_Enum_Value_For_Name(setting, &value, valueStr.c_str());
		err = OSc_Setting_Set_Enum_Value(setting, value);
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