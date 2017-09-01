#include "OpenScan.h"

#include "ModuleInterface.h"

#include <stdio.h>
#include <string.h>

const char* const DEVICE_NAME_Camera = "OpenScan";

const char* const PROPERTY_Scanner = "Scanner";
const char* const PROPERTY_Detector = "Detector";

enum
{
	ERR_SCANNER_AND_DETECTOR_REQUIRED = 30000,
};


MODULE_API void
InitializeModuleData()
{
	RegisterDevice(DEVICE_NAME_Camera, MM::CameraDevice, "OpenScan Laser Scanning System");
}


MODULE_API MM::Device*
CreateDevice(const char* name)
{
	if (std::string(name) == DEVICE_NAME_Camera)
		return new OpenScan();
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

		bool flag;
		if (OSc_Device_Has_Scanner(device, &flag) == OSc_Error_OK)
			scannerDevices_[name] = device;
		if (OSc_Device_Has_Detector(device, &flag) == OSc_Error_OK)
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
	OSc_Error err = OSc_LSM_Create(&oscLSM_);
	if (err != OSc_Error_OK)
		return err;

	char scannerName[OSc_MAX_STR_LEN + 1];
	int stat = GetProperty(PROPERTY_Scanner, scannerName);
	if (stat != DEVICE_OK)
		return stat;

	char detectorName[OSc_MAX_STR_LEN + 1];
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
	err = OSc_Device_Open(detectorDevice, oscLSM_);
	if (err != OSc_Error_OK)
		return err;

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

	// TODO Auto-create properties for Osc_Device settings

	// Standard properties Exposure and Binning - not used for LSM
	err = CreateFloatProperty(MM::g_Keyword_Exposure, 0.0, false);
	if (err != DEVICE_OK)
		return err;
	err = AddAllowedValue(MM::g_Keyword_Exposure, "0.0000"); if (err != DEVICE_OK) return err;
	err = CreateIntegerProperty(MM::g_Keyword_Binning, 1, false);
	if (err != DEVICE_OK)
		return err;
	err = AddAllowedValue(MM::g_Keyword_Binning, "1"); if (err != DEVICE_OK) return err;

	return DEVICE_OK;
}


int
OpenScan::Shutdown()
{
	StopSequenceAcquisition();

	OSc_LSM_Destroy(oscLSM_);
	oscLSM_ = 0;

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
	if (IsCapturing() || sequenceAcquisition_)
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
	bool isRunning;
	OSc_Error err = OSc_LSM_Is_Running_Acquisition(oscLSM_, &isRunning);
	if (err != OSc_Error_OK)
		return false;
	return isRunning;
}