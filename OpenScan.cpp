#include "OpenScan.h"
#include "Waveform.h"

#include "ModuleInterface.h"

const char* const DEVICE_NAME_Camera = "OpenScan";

const char* const PROPERTY_NAME_ScanRate = "ScanRate";
const char* const PROPERTY_NAME_Resolution = "Resolution";
const char* const PROPERTY_NAME_Zoom = "Zoom";
const char* const PROPERTY_NAME_GalvoOffsetX = "GalvoOffsetX";
const char* const PROPERTY_NAME_GalvoOffsetY = "GalvoOffsetY";
const char* const PROPERTY_NAME_Channels = "Channels";
const char* const PROPERTY_VALUE_RawImage = "RawImage";
const char* const PROPERTY_VALUE_KalmanAveraged = "KalmanAveraged";
const char* const PROPERTY_VALUE_RawAndKalmanAveraged = "RawAndKalmanAveraged";
const char* const PROPERTY_NAME_KalmanProgressive = "KalmanAveragingProgressive";
const char* const PROPERTY_NAME_KalmanAverageFrames = "KalmanAverageFrames";
const char* const PROPERTY_NAME_KalmanFilterGain = "KalmanAveragingFilterGain";
const char* const PROPERTY_NAME_DebugFPGAState = "Debug-FPGAStateMachineState";
const char* const PROPERTY_NAME_DebugWaveformWrittenToDRAM = "Debug-WaveformWrittenToDRAM";
const char* const PROPERTY_NAME_DebugWaveformOutputFinished = "Debug-WaveformOutputFinished";
const char* const PROPERTY_NAME_DebugFrameAcquisitionFinished = "Debug-FrameAcquisitionFinished";
const char* const PROPERTY_NAME_DebugAveragedImageDisplayed = "Debug-AveragedImageDisplayed";


enum FpgaState
{
	FPGA_STATE_IDLE,
	FPGA_STATE_INIT,
	FPGA_STATE_WRITE,
	FPGA_STATE_SCAN,
	FPGA_STATE_BLANK,
	FPGA_STATE_DONE,
	FPGA_STATE_STOP,
};


enum
{
	ERR_UNEXPECTED_STATE_AFTER_FPGA_RESET = 2000,
	ERR_WAVEFORM_OUT_OF_RANGE,
	ERR_WAVEFORM_MEMORY_SIZE_MISMATCH,
	ERR_DATA_LEFT_IN_FIFO_AFTER_READING_IMAGE,
};


inline uint16_t DoubleToFixed16(double d, int intBits)
{
	int fracBits = 16 - intBits;
	return (uint16_t)round(d * (1 << fracBits));
}


MODULE_API void
InitializeModuleData()
{
	RegisterDevice(DEVICE_NAME_Camera, MM::CameraDevice, "OpenScan");
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
	fpgaSession_(0),
	settingsChanged_(true),
	scanRate_(0.2),
	resolution_(512),
	zoom_(6.0),
	galvoOffsetX_(0),
	galvoOffsetY_(0),
	channels_(CHANNELS_RAW_IMAGE),
	kalmanProgressive_(false),
	kalmanFrames_(1),
	filterGain_(0.99),
	imageBuffer_(0),
	kalmanBuffer_(0),
	sequenceThread_(0),
	stopSequenceRequested_(false)
{
	SetErrorText(ERR_UNEXPECTED_STATE_AFTER_FPGA_RESET, "FPGA in incorrect state at startup");
	SetErrorText(ERR_WAVEFORM_OUT_OF_RANGE, "Waveform out of DAC range");
	SetErrorText(ERR_WAVEFORM_MEMORY_SIZE_MISMATCH, "Waveform size does not match FPGA waveform memory size");
	SetErrorText(ERR_DATA_LEFT_IN_FIFO_AFTER_READING_IMAGE, "Data left in FIFO after reading image");
}


OpenScan::~OpenScan()
{
}


int
OpenScan::Initialize()
{
	LogMessage("Initializing FPGA driver...", true);
	NiFpga_Status stat = NiFpga_Initialize();
	if (NiFpga_IsError(stat))
		return stat;

	LogMessage("Opening FPGA...", true);
	stat = NiFpga_Open(NiFpga_OpenScanFPGAHost_Bitfile,
		NiFpga_OpenScanFPGAHost_Signature,
		"PXI1Slot3",
		NiFpga_OpenAttribute_NoRun,
		&fpgaSession_);
	if (NiFpga_IsError(stat))
		return stat;

	int err = StartFPGA();
	if (err != DEVICE_OK)
		return err;

	err = CreateFloatProperty(PROPERTY_NAME_ScanRate, scanRate_, false,
		new CPropertyAction(this, &OpenScan::OnScanRate));
	if (err != DEVICE_OK)
		return err;
	err = AddAllowedValue(PROPERTY_NAME_ScanRate, "0.0500"); if (err != DEVICE_OK) return err;
	err = AddAllowedValue(PROPERTY_NAME_ScanRate, "0.1000"); if (err != DEVICE_OK) return err;
	err = AddAllowedValue(PROPERTY_NAME_ScanRate, "0.1500"); if (err != DEVICE_OK) return err;
	err = AddAllowedValue(PROPERTY_NAME_ScanRate, "0.2000"); if (err != DEVICE_OK) return err;
	err = AddAllowedValue(PROPERTY_NAME_ScanRate, "0.2500"); if (err != DEVICE_OK) return err;
	err = AddAllowedValue(PROPERTY_NAME_ScanRate, "0.3000"); if (err != DEVICE_OK) return err;
	err = AddAllowedValue(PROPERTY_NAME_ScanRate, "0.4000"); if (err != DEVICE_OK) return err;
	err = AddAllowedValue(PROPERTY_NAME_ScanRate, "0.5000"); if (err != DEVICE_OK) return err;

	err = CreateIntegerProperty(PROPERTY_NAME_Resolution, resolution_, false,
		new CPropertyAction(this, &OpenScan::OnResolution));
	if (err != DEVICE_OK)
		return err;
	err = AddAllowedValue(PROPERTY_NAME_Resolution, "256"); if (err != DEVICE_OK) return err;
	err = AddAllowedValue(PROPERTY_NAME_Resolution, "512"); if (err != DEVICE_OK) return err;
	err = AddAllowedValue(PROPERTY_NAME_Resolution, "1024"); if (err != DEVICE_OK) return err;
	err = AddAllowedValue(PROPERTY_NAME_Resolution, "2048"); if (err != DEVICE_OK) return err;

	err = CreateFloatProperty(PROPERTY_NAME_Zoom, zoom_, false,
		new CPropertyAction(this, &OpenScan::OnZoom));
	if (err != DEVICE_OK)
		return err;
	err = SetPropertyLimits(PROPERTY_NAME_Zoom, 1.0, 40.0);
	if (err != DEVICE_OK)
		return err;

	/*The galvoOffsetX and galvoOffsetY variables are expressed  in optical degrees
	This is a rough correspondence - it likely needs to be calibrated to the actual
	sensitivity of the galvos*/
	err = CreateFloatProperty(PROPERTY_NAME_GalvoOffsetX, galvoOffsetX_, false,
		new CPropertyAction(this, &OpenScan::OnGalvoOffsetX));
	if (err!= DEVICE_OK)
		return err;
	err = SetPropertyLimits(PROPERTY_NAME_GalvoOffsetX, -10.0, 10.0);
	if (err != DEVICE_OK)
		return err;

	err = CreateFloatProperty(PROPERTY_NAME_GalvoOffsetY, galvoOffsetY_, false,
		new CPropertyAction(this, &OpenScan::OnGalvoOffsetY));
	if (err != DEVICE_OK)
		return err;
	err = SetPropertyLimits(PROPERTY_NAME_GalvoOffsetY, -10.0, 10.0);
	if (err != DEVICE_OK)
		return err;

	err = CreateStringProperty(PROPERTY_NAME_Channels, PROPERTY_VALUE_RawImage, false,
		new CPropertyAction(this, &OpenScan::OnChannels));
	if (err != DEVICE_OK)
		return err;
	err = AddAllowedValue(PROPERTY_NAME_Channels, PROPERTY_VALUE_RawImage); if (err != DEVICE_OK) return err;
	err = AddAllowedValue(PROPERTY_NAME_Channels, PROPERTY_VALUE_KalmanAveraged); if (err != DEVICE_OK) return err;
	err = AddAllowedValue(PROPERTY_NAME_Channels, PROPERTY_VALUE_RawAndKalmanAveraged); if (err != DEVICE_OK) return err;

	err = CreateStringProperty(PROPERTY_NAME_KalmanProgressive,
		(kalmanProgressive_ ? "Yes" : "No"), false,
		new CPropertyAction(this, &OpenScan::OnKalmanProgressive));
	if (err != DEVICE_OK)
		return err;
	err = AddAllowedValue(PROPERTY_NAME_KalmanProgressive, "Yes"); if (err != DEVICE_OK) return err;
	err = AddAllowedValue(PROPERTY_NAME_KalmanProgressive, "No"); if (err != DEVICE_OK) return err;

	err = CreateIntegerProperty(PROPERTY_NAME_KalmanAverageFrames, kalmanFrames_, false,
		new CPropertyAction(this, &OpenScan::OnKalmanAverageFrames));
	if (err != DEVICE_OK)
		return err;
	err = SetPropertyLimits(PROPERTY_NAME_KalmanAverageFrames, 1, 100);
	if (err != DEVICE_OK)
		return err;

	err = CreateFloatProperty(PROPERTY_NAME_KalmanFilterGain, filterGain_, false,
		new CPropertyAction(this, &OpenScan::OnKalmanFilterGain));
	if (err != DEVICE_OK)
		return err;
	err = SetPropertyLimits(PROPERTY_NAME_KalmanFilterGain, 0.0, 1.0);
	if (err != DEVICE_OK)
		return err;

	err = CreateStringProperty(PROPERTY_NAME_DebugFPGAState, "No", true,
		new CPropertyAction(this, &OpenScan::OnDebugFPGAState));
	if (err != DEVICE_OK)
		return err;
	err = CreateStringProperty(PROPERTY_NAME_DebugWaveformWrittenToDRAM, "No", true,
		new CPropertyAction(this, &OpenScan::OnDebugWaveformWrittenToDRAM));
	if (err != DEVICE_OK)
		return err;
	err = CreateStringProperty(PROPERTY_NAME_DebugWaveformOutputFinished, "No", true,
		new CPropertyAction(this, &OpenScan::OnDebugWaveformOutputFinished));
	if (err != DEVICE_OK)
		return err;
	err = CreateStringProperty(PROPERTY_NAME_DebugFrameAcquisitionFinished, "No", true,
		new CPropertyAction(this, &OpenScan::OnDebugFrameAcquisitionFinished));
	if (err != DEVICE_OK)
		return err;
	err = CreateStringProperty(PROPERTY_NAME_DebugAveragedImageDisplayed, "No", true,
		new CPropertyAction(this, &OpenScan::OnDebugAveragedImageDisplayed));
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

	return DEVICE_OK;
}


int
OpenScan::Shutdown()
{
	StopSequenceAcquisition();

	NiFpga_Status stat;

	if (fpgaSession_)
	{
		// Reset FPGA to close shutter (temporary workaround)
		StartFPGA();

		stat = NiFpga_WriteU16(fpgaSession_,
			NiFpga_OpenScanFPGAHost_ControlU16_Current, FPGA_STATE_STOP);
		if (NiFpga_IsError(stat))
			return stat;

		stat = NiFpga_Close(fpgaSession_, 0);
		if (NiFpga_IsError(stat))
			return stat;
		fpgaSession_ = 0;
	}

	stat = NiFpga_Finalize();
	if (NiFpga_IsError(stat))
		return stat;
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


int
OpenScan::SnapImage()
{
	if (IsCapturing())
		return DEVICE_CAMERA_BUSY_ACQUIRING;

	int err;

	if (settingsChanged_)
	{
		err = ReloadWaveform();
		if (err != DEVICE_OK)
			return err;
		settingsChanged_ = false;
	}

	err = SetScanParameters();
	if (err != DEVICE_OK)
		return err;

	for (unsigned i = 0; i < kalmanFrames_; ++i)
	{
		LogMessage("Starting scan...", true);
		err = StartScan();
		if (err != DEVICE_OK)
			return err;

		LogMessage("Waiting for scan to finish...", true);
		NiFpga_Bool finished = false;
		while (!finished)
		{
			NiFpga_Status stat = NiFpga_ReadBool(fpgaSession_,
				NiFpga_OpenScanFPGAHost_IndicatorBool_Frameacquisitionfinish, &finished);
			if (NiFpga_IsError(stat))
				return stat;
			Sleep(10);
		}

		LogMessage("Reading image...", true);
		err = ReadImage();
		if (err != DEVICE_OK)
			return err;
		LogMessage("Finished reading image", true);

		if (i < kalmanFrames_ - 1)
		{
			NiFpga_Status stat = NiFpga_WriteI32(fpgaSession_, NiFpga_OpenScanFPGAHost_ControlI32_Framenumber, i + 1);
			if (NiFpga_IsError(stat))
				return stat;
			err = SetKalmanGain(1.0 / (i + 2));
			if (err != DEVICE_OK)
				return err;
		}
	}

	return DEVICE_OK;
}


int
OpenScan::ReloadWaveform()
{
	int err;

	LogMessage("Sending parameters...", true);
	err = SendParameters();
	if (err != DEVICE_OK)
		return err;

	LogMessage("Setting resolution...", true);
	err = SetResolution(resolution_);
	if (err != DEVICE_OK)
		return err;

	LogMessage("Setting up scan...", true);
	err = InitScan();
	if (err != DEVICE_OK)
		return err;

	uint16_t firstX, firstY;
	LogMessage("Writing waveform...", true);
	err = WriteWaveforms(&firstX, &firstY);
	if (err != DEVICE_OK)
		return err;

	LogMessage("Moving galvos to start position...", true);
	err = MoveGalvosTo(firstX, firstY);
	if (err != DEVICE_OK)
		return err;

	return DEVICE_OK;
}


const unsigned char*
OpenScan::GetImageBuffer(unsigned chan)
{
	if (chan >= GetNumberOfChannels())
		return 0;

	uint16_t* buffer = 0;
	switch (channels_)
	{
	case CHANNELS_RAW_IMAGE:
		buffer = imageBuffer_;
		break;
	case CHANNELS_KALMAN_AVERAGED:
		buffer = kalmanBuffer_;
		break;
	case CHANNELS_RAW_AND_KALMAN:
		switch (chan)
		{
		case 0:
			buffer = imageBuffer_;
			break;
		case 1:
			buffer = kalmanBuffer_;
			break;
		}
		break;
	}
	return reinterpret_cast<unsigned char*>(buffer);
}


long
OpenScan::GetImageBufferSize() const
{
	return GetImageWidth() * GetImageHeight() * GetImageBytesPerPixel();
}


unsigned
OpenScan::GetImageWidth() const
{
	return resolution_;
}


unsigned
OpenScan::GetImageHeight() const
{
	return resolution_;
}


unsigned
OpenScan::GetImageBytesPerPixel() const
{
	return 2;
}


unsigned
OpenScan::GetNumberOfComponents() const
{
	return 1;
}


unsigned
OpenScan::GetNumberOfChannels() const
{
	switch (channels_)
	{
	case CHANNELS_RAW_IMAGE:
	case CHANNELS_KALMAN_AVERAGED:
		return 1;
	case CHANNELS_RAW_AND_KALMAN:
		return 2;
	}
	return 0;
}


int
OpenScan::GetChannelName(unsigned channel, char* name)
{
	switch (channels_)
	{
	case CHANNELS_RAW_IMAGE:
	case CHANNELS_KALMAN_AVERAGED:
		GetName(name);
		return DEVICE_OK;
	case CHANNELS_RAW_AND_KALMAN:
		switch (channel)
		{
		case 0:
			GetName(name);
			strcat(name, "-Unaveraged");
			break;
		case 1:
			GetName(name);
			strcat(name, "-Kalman");
			break;
		}
		return DEVICE_OK;
	}
	return DEVICE_ERR;
}


unsigned
OpenScan::GetBitDepth() const
{
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
	xSize = resolution_;
	ySize = resolution_;
	return DEVICE_OK;
}


int
OpenScan::ClearROI()
{
	if (IsCapturing())
		return DEVICE_CAMERA_BUSY_ACQUIRING;

	return DEVICE_OK;
}


int
OpenScan::StartSequenceAcquisition(long count, double, bool stopOnOverflow)
{
	MMThreadGuard g(sequenceThreadMutex_);

	if (sequenceThread_)
		return DEVICE_CAMERA_BUSY_ACQUIRING;
	if (count < 1)
		return DEVICE_OK;

	stopSequenceRequested_ = false;
	sequenceThread_ = new SequenceThread(this,
		count, kalmanFrames_, stopOnOverflow);
	sequenceThread_->Start();
	return DEVICE_OK;
}


int
OpenScan::StopSequenceAcquisitionImpl(bool wait)
{
	SequenceThread* thread;
	{
		MMThreadGuard g(sequenceThreadMutex_);

		if (!sequenceThread_)
			return DEVICE_OK;

		LogMessage("Stopping sequence acquisition", true);
		stopSequenceRequested_ = true;
		thread = sequenceThread_;
	}

	LogMessage("Stopping scanning", true);
	int err = StopScan();

	if (wait)
	{
		LogMessage("Waiting for sequence thread to exit", true);
		thread->wait();
		LogMessage("Sequence thread exited", true);
	}

	{
		MMThreadGuard g(sequenceThreadMutex_);
		delete sequenceThread_;
		sequenceThread_ = 0;
	}

	return err;
}


// Called on sequence thread
int
OpenScan::SendSequenceImages(bool stopOnOverflow)
{
	uint16_t* chan0Buffer = 0;
	uint16_t* chan1Buffer = 0;
	switch (channels_)
	{
	case CHANNELS_RAW_IMAGE:
		chan0Buffer = imageBuffer_;
		break;
	case CHANNELS_KALMAN_AVERAGED:
		chan0Buffer = kalmanBuffer_;
		break;
	case CHANNELS_RAW_AND_KALMAN:
		chan0Buffer = imageBuffer_;
		chan1Buffer = kalmanBuffer_;
		break;
	}
	int err;

	err = SendSequenceImage(stopOnOverflow, 0,
		reinterpret_cast<unsigned char*>(chan0Buffer));
	if (err != DEVICE_OK)
		return err;

	if (chan1Buffer)
	{
		err = SendSequenceImage(stopOnOverflow, 1,
			reinterpret_cast<unsigned char*>(chan1Buffer));
		if (err != DEVICE_OK)
			return err;
	}

	return DEVICE_OK;
}


int
OpenScan::SendSequenceImage(bool stopOnOverflow, int chan, const unsigned char* pixels)
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
	int err = GetCoreCallback()->InsertImage(this, pixels, width, height, bytesPerPixel, md.Serialize().c_str());
	if (!stopOnOverflow && err == DEVICE_BUFFER_OVERFLOW)
	{
		GetCoreCallback()->ClearImageBuffer(this);
		return GetCoreCallback()->InsertImage(this, pixels, width, height, bytesPerPixel, md.Serialize().c_str(), false);
	}
	else
	{
		return err;
	}
	return DEVICE_OK;
}


bool
OpenScan::IsCapturing()
{
	MMThreadGuard g(sequenceThreadMutex_);
	return sequenceThread_ != 0;
}


int
OpenScan::OnScanRate(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct == MM::BeforeGet)
	{
		pProp->Set(scanRate_);
	}
	else if (eAct == MM::AfterSet)
	{
		if (IsCapturing())
			return DEVICE_CAMERA_BUSY_ACQUIRING;
		double v;
		pProp->Get(v);
		scanRate_ = v;
	}
	return DEVICE_OK;
}


int
OpenScan::OnResolution(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct == MM::BeforeGet)
	{
		pProp->Set(static_cast<long>(resolution_));
	}
	else if (eAct == MM::AfterSet)
	{
		if (IsCapturing())
			return DEVICE_CAMERA_BUSY_ACQUIRING;
		long v;
		pProp->Get(v);
		resolution_ = v;
		settingsChanged_ = true;
	}
	return DEVICE_OK;
}


int
OpenScan::OnZoom(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct == MM::BeforeGet)
	{
		pProp->Set(zoom_);
	}
	else if (eAct == MM::AfterSet)
	{
		if (IsCapturing())
			return DEVICE_CAMERA_BUSY_ACQUIRING;
		double v;
		pProp->Get(v);
		zoom_ = v;
		settingsChanged_ = true;
	}
	return DEVICE_OK;
}

int
OpenScan::OnGalvoOffsetX(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct == MM::BeforeGet)
	{
		pProp->Set(galvoOffsetX_);
	}
	else if (eAct == MM::AfterSet)
	{
		if (IsCapturing())
			return DEVICE_CAMERA_BUSY_ACQUIRING;
		double v;
		pProp->Get(v);
		galvoOffsetX_ = v;
		settingsChanged_ = true;
	}
	return DEVICE_OK;
}

int
OpenScan::OnGalvoOffsetY(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct == MM::BeforeGet)
	{
		pProp->Set(galvoOffsetY_);
	}
	else if (eAct == MM::AfterSet)
	{
		if (IsCapturing())
			return DEVICE_CAMERA_BUSY_ACQUIRING;
		double v;
		pProp->Get(v);
		galvoOffsetY_ = v;
		settingsChanged_ = true;
	}
	return DEVICE_OK;
}

int
OpenScan::OnChannels(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct == MM::BeforeGet)
	{
		const char* v = "";
		switch (channels_)
		{
		case CHANNELS_RAW_IMAGE:
			v = PROPERTY_VALUE_RawImage;
			break;
		case CHANNELS_KALMAN_AVERAGED:
			v = PROPERTY_VALUE_KalmanAveraged;
			break;
		case CHANNELS_RAW_AND_KALMAN:
			v = PROPERTY_VALUE_RawAndKalmanAveraged;
			break;
		}
		pProp->Set(v);
	}
	else if (eAct == MM::AfterSet)
	{
		if (IsCapturing())
			return DEVICE_CAMERA_BUSY_ACQUIRING;
		std::string v;
		pProp->Get(v);
		if (v == PROPERTY_VALUE_RawImage)
			channels_ = CHANNELS_RAW_IMAGE;
		else if (v == PROPERTY_VALUE_KalmanAveraged)
			channels_ = CHANNELS_KALMAN_AVERAGED;
		else if (v == PROPERTY_VALUE_RawAndKalmanAveraged)
			channels_ = CHANNELS_RAW_AND_KALMAN;
	}
	return DEVICE_OK;
}


int
OpenScan::OnKalmanProgressive(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct == MM::BeforeGet)
	{
		pProp->Set(kalmanProgressive_ ? "Yes" : "No");
	}
	else if (eAct == MM::AfterSet)
	{
		std::string v;
		pProp->Get(v);
		kalmanProgressive_ = (v == "Yes");
	}
	return DEVICE_OK;
}


int
OpenScan::OnKalmanAverageFrames(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct == MM::BeforeGet)
	{
		pProp->Set(static_cast<long>(kalmanFrames_));
	}
	else if (eAct == MM::AfterSet)
	{
		if (IsCapturing())
			return DEVICE_CAMERA_BUSY_ACQUIRING;
		long v;
		pProp->Get(v);
		kalmanFrames_ = v;
	}
	return DEVICE_OK;
}


int
OpenScan::OnKalmanFilterGain(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct == MM::BeforeGet)
	{
		pProp->Set(filterGain_);
	}
	else if (eAct == MM::AfterSet)
	{
		if (IsCapturing())
			return DEVICE_CAMERA_BUSY_ACQUIRING;
		double v;
		pProp->Get(v);
		filterGain_ = v;
	}
	return DEVICE_OK;
}


int
OpenScan::OnDebugFPGAState(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct != MM::BeforeGet)
		return DEVICE_OK;
	uint16_t stateNumber;
	NiFpga_Status stat = NiFpga_ReadU16(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlU16_Current, &stateNumber);
	if (NiFpga_IsError(stat))
		return stat;
	const char* stateName = "Unknown";
	switch (stateNumber)
	{
	case FPGA_STATE_IDLE:
		stateName = "Idle";
		break;
	case FPGA_STATE_INIT:
		stateName = "Init";
		break;
	case FPGA_STATE_WRITE:
		stateName = "Write";
		break;
	case FPGA_STATE_SCAN:
		stateName = "Scan";
		break;
	case FPGA_STATE_BLANK:
		stateName = "Blank";
		break;
	case FPGA_STATE_DONE:
		stateName = "Done";
		break;
	case FPGA_STATE_STOP:
		stateName = "Stop";
		break;
	}
	pProp->Set(stateName);
	return DEVICE_OK;
}


int
OpenScan::OnDebugWaveformWrittenToDRAM(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct != MM::BeforeGet)
		return DEVICE_OK;
	NiFpga_Bool flag;
	NiFpga_Status stat = NiFpga_ReadBool(fpgaSession_,
		NiFpga_OpenScanFPGAHost_IndicatorBool_WriteDRAMdone, &flag);
	if (NiFpga_IsError(stat))
		return stat;
	pProp->Set(flag ? "Yes" : "No");
	return DEVICE_OK;
}


int
OpenScan::OnDebugWaveformOutputFinished(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct != MM::BeforeGet)
		return DEVICE_OK;
	NiFpga_Bool flag;
	NiFpga_Status stat = NiFpga_ReadBool(fpgaSession_,
		NiFpga_OpenScanFPGAHost_IndicatorBool_Framewaveformoutputfinish, &flag);
	if (NiFpga_IsError(stat))
		return stat;
	pProp->Set(flag ? "Yes" : "No");
	return DEVICE_OK;
}


int
OpenScan::OnDebugFrameAcquisitionFinished(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct != MM::BeforeGet)
		return DEVICE_OK;
	NiFpga_Bool flag;
	NiFpga_Status stat = NiFpga_ReadBool(fpgaSession_,
		NiFpga_OpenScanFPGAHost_IndicatorBool_Frameacquisitionfinish, &flag);
	if (NiFpga_IsError(stat))
		return stat;
	pProp->Set(flag ? "Yes" : "No");
	return DEVICE_OK;
}


int
OpenScan::OnDebugAveragedImageDisplayed(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct != MM::BeforeGet)
		return DEVICE_OK;
	NiFpga_Bool flag;
	NiFpga_Status stat = NiFpga_ReadBool(fpgaSession_,
		NiFpga_OpenScanFPGAHost_IndicatorBool_Averagedimagedisplayed, &flag);
	if (NiFpga_IsError(stat))
		return stat;
	pProp->Set(flag ? "Yes" : "No");
	return DEVICE_OK;
}


int
OpenScan::StartFPGA()
{
	NiFpga_Status stat;
	LogMessage("Resetting FPGA...", true);
	stat = NiFpga_Reset(fpgaSession_);
	if (NiFpga_IsError(stat))
		return stat;
	LogMessage("Starting FPGA...", true);
	stat = NiFpga_Run(fpgaSession_, 0);
	if (NiFpga_IsError(stat))
		return stat;

	uint16_t currentState;
	stat = NiFpga_ReadU16(fpgaSession_, NiFpga_OpenScanFPGAHost_ControlU16_Current,
		&currentState);
	if (NiFpga_IsError(stat))
		return stat;
	if (currentState != FPGA_STATE_IDLE)
	{
		return ERR_UNEXPECTED_STATE_AFTER_FPGA_RESET;
	}

	return DEVICE_OK;
}


int
OpenScan::SendParameters()
{
	int err = StartFPGA();
	if (err != DEVICE_OK)
		return err;

	NiFpga_Status stat;
	stat = NiFpga_WriteI32(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlI32_Numofundershoot, X_UNDERSHOOT);
	if (NiFpga_IsError(stat))
		return stat;
	stat = NiFpga_WriteI32(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlI32_Pixelpulse_initialdelay, 1);
	if (NiFpga_IsError(stat))
		return stat;
	stat = NiFpga_WriteU32(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlU32_Frameretracetime, 100);
	if (NiFpga_IsError(stat))
		return stat;

	return DEVICE_OK;
}


int
OpenScan::SetScanRate(double scanRate)
{
	double pixelTime = 40.0 / scanRate;
	int32_t pixelTimeTicks = (int32_t) round(pixelTime);
	NiFpga_Status stat = NiFpga_WriteI32(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlI32_Pixeltimetick, pixelTimeTicks);
	if (NiFpga_IsError(stat))
		return stat;
	int32_t pulseWidthTicks = pixelTimeTicks - 21;
	stat = NiFpga_WriteI32(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlI32_Pixelclock_pulsewidthtick, pulseWidthTicks);
	if (NiFpga_IsError(stat))
		return stat;

	return DEVICE_OK;
}


int
OpenScan::SetResolution(uint32_t resolution)
{
	int32_t elementsPerLine = X_UNDERSHOOT + resolution + X_RETRACE_LEN;

	NiFpga_Status stat = NiFpga_WriteI32(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlI32_Resolution, resolution);
	if (NiFpga_IsError(stat))
		return stat;
	stat = NiFpga_WriteI32(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlI32_Elementsperline, elementsPerLine);
	if (NiFpga_IsError(stat))
		return stat;
	stat = NiFpga_WriteU32(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlU32_maxaddr, (uint32_t) elementsPerLine);
	if (NiFpga_IsError(stat))
		return stat;

	uint32_t totalElements = elementsPerLine * resolution;
	stat = NiFpga_WriteU32(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlU32_Totalelements, totalElements);
	if (NiFpga_IsError(stat))
		return stat;
	stat = NiFpga_WriteU32(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlU32_Numofelements, totalElements);
	if (NiFpga_IsError(stat))
		return stat;

	uint32_t totalPixels = resolution * resolution;
	stat = NiFpga_WriteU32(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlU32_Samplesperframecontrol, totalPixels);
	if (NiFpga_IsError(stat))
		return stat;
	stat = NiFpga_WriteU32(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlU32_MaxDRAMaddress, totalPixels / 16);
	if (NiFpga_IsError(stat))
		return stat;

	return DEVICE_OK;
}


int
OpenScan::InitScan()
{
	NiFpga_Status stat;
	stat = NiFpga_WriteBool(fpgaSession_, NiFpga_OpenScanFPGAHost_IndicatorBool_Imageaveragingdone, false);
	if (NiFpga_IsError(stat))
		return stat;
	stat = NiFpga_WriteBool(fpgaSession_, NiFpga_OpenScanFPGAHost_IndicatorBool_Averagedimagedisplayed, false);
	if (NiFpga_IsError(stat))
		return stat;
	stat = NiFpga_WriteBool(fpgaSession_, NiFpga_OpenScanFPGAHost_IndicatorBool_Frameimagingdataaveraged, false);
	if (NiFpga_IsError(stat))
		return stat;
	stat = NiFpga_WriteBool(fpgaSession_, NiFpga_OpenScanFPGAHost_ControlBool_WriteFrameGalvosignal, false);
	if (NiFpga_IsError(stat))
		return stat;
	stat = NiFpga_WriteBool(fpgaSession_, NiFpga_OpenScanFPGAHost_ControlBool_WriteDRAMenable, false);
	if (NiFpga_IsError(stat))
		return stat;
	stat = NiFpga_WriteBool(fpgaSession_, NiFpga_OpenScanFPGAHost_IndicatorBool_FrameGalvosignalwritedone, false);
	if (NiFpga_IsError(stat))
		return stat;
	stat = NiFpga_WriteBool(fpgaSession_, NiFpga_OpenScanFPGAHost_IndicatorBool_Framewaveformoutputfinish, false);
	if (NiFpga_IsError(stat))
		return stat;
	stat = NiFpga_WriteBool(fpgaSession_, NiFpga_OpenScanFPGAHost_IndicatorBool_Frameacquisitionfinish, false);
	if (NiFpga_IsError(stat))
		return stat;
	stat = NiFpga_WriteBool(fpgaSession_, NiFpga_OpenScanFPGAHost_ControlBool_Done, false);
	if (NiFpga_IsError(stat))
		return stat;

	stat = NiFpga_WriteU16(fpgaSession_, NiFpga_OpenScanFPGAHost_ControlU16_Current, FPGA_STATE_INIT);
	if (NiFpga_IsError(stat))
		return stat;

	return DEVICE_OK;
}


int
OpenScan::SetScanParameters()
{
	NiFpga_Status stat = NiFpga_WriteI32(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlI32_Numberofframes, 1);
	if (NiFpga_IsError(stat))
		return stat;
	stat = NiFpga_WriteI32(fpgaSession_, NiFpga_OpenScanFPGAHost_ControlI32_Framenumber, 0);
	if (NiFpga_IsError(stat))
		return stat;

	stat = NiFpga_WriteU16(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlU16_Filtergain, DoubleToFixed16(filterGain_, 0));
	if (NiFpga_IsError(stat))
		return stat;
	int err = SetKalmanGain(1.0);
	if (err != DEVICE_OK)
		return err;

	return DEVICE_OK;
}


int
OpenScan::SetKalmanGain(double kg)
{
	NiFpga_Status stat = NiFpga_WriteU16(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlU16_Kalmangain, DoubleToFixed16(kg, 1));
	if (NiFpga_IsError(stat))
		return stat;
	return DEVICE_OK;
}


int
OpenScan::WriteWaveforms(uint16_t *firstX, uint16_t *firstY)
{
	uint32_t elementsPerLine =
		X_UNDERSHOOT + resolution_ + X_RETRACE_LEN;
	uint16_t *xScaled = (uint16_t *)malloc(sizeof(uint16_t) * elementsPerLine);
	uint16_t *yScaled = (uint16_t *)malloc(sizeof(uint16_t) * resolution_);

	int err = GenerateScaledWaveforms(resolution_, 0.25 * zoom_, xScaled, yScaled,
		galvoOffsetX_, galvoOffsetY_);
	if (err != 0)
		return ERR_WAVEFORM_OUT_OF_RANGE;

	NiFpga_Status stat;

	stat = NiFpga_WriteU16(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlU16_Current, FPGA_STATE_WRITE);
	if (NiFpga_IsError(stat))
		goto error;

	stat = NiFpga_WriteBool(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlBool_WriteDRAMenable, true);
	if (NiFpga_IsError(stat))
		goto error;
	stat = NiFpga_WriteBool(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlBool_WriteFrameGalvosignal, true);
	if (NiFpga_IsError(stat))
		goto error;

	size_t fifoSize = 0;
	stat = NiFpga_WriteFifoU32(fpgaSession_,
		NiFpga_OpenScanFPGAHost_HostToTargetFifoU32_HosttotargetFIFO,
		0, 0, 10000, &fifoSize);

	uint32_t *xy = (uint32_t *)malloc(sizeof(uint32_t) * elementsPerLine);
	for (unsigned j = 0; j < resolution_; ++j)
	{
		for (unsigned i = 0; i < elementsPerLine; ++i)
		{
			xy[i] = ((uint32_t) xScaled[i] << 16) | yScaled[j];
		}

		NiFpga_Bool dramFull = false;
		stat = NiFpga_ReadBool(fpgaSession_, NiFpga_OpenScanFPGAHost_IndicatorBool_WriteDRAMdone, &dramFull);
		if (NiFpga_IsError(stat))
			goto error;
		if (dramFull)
		{
			std::ostringstream oss;
			oss << "FPGA DRAM full; wrote " << j << " lines of " << resolution_;
			LogMessage(oss.str().c_str(), true);
			stat = ERR_WAVEFORM_MEMORY_SIZE_MISMATCH;
			goto error;
		}

		size_t remaining;
		stat = NiFpga_WriteFifoU32(fpgaSession_,
			NiFpga_OpenScanFPGAHost_HostToTargetFifoU32_HosttotargetFIFO,
			xy, elementsPerLine, 10000, &remaining);
		if (NiFpga_IsError(stat))
			goto error;

		bool done = false;
		while (!done)
		{
			stat = NiFpga_WriteFifoU32(fpgaSession_,
				NiFpga_OpenScanFPGAHost_HostToTargetFifoU32_HosttotargetFIFO,
				0, 0, 10000, &remaining);
			if (remaining == fifoSize)
				done = true;
		}
	}

	Sleep(10);

	NiFpga_Bool dramFull = false;
	stat = NiFpga_ReadBool(fpgaSession_, NiFpga_OpenScanFPGAHost_IndicatorBool_WriteDRAMdone, &dramFull);
	if (NiFpga_IsError(stat))
		goto error;
	if (!dramFull)
	{
		stat = ERR_WAVEFORM_MEMORY_SIZE_MISMATCH;
		LogMessage("FPGA DRAM not full after writing all lines");
		goto error;
	}

	*firstX = xScaled[0];
	*firstY = yScaled[0];

	free(xScaled);
	free(yScaled);

	return DEVICE_OK;

error:
	free(xScaled);
	free(yScaled);
	return stat;
}


int
OpenScan::MoveGalvosTo(uint16_t x, uint16_t y)
{
	uint32_t xy = (uint32_t)x << 16 | y;
	NiFpga_Status stat = NiFpga_WriteU32(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlU32_Galvosignal, xy);
	if (NiFpga_IsError(stat))
		return stat;
	return DEVICE_OK;
}


int
OpenScan::StartScan()
{
	int err = SetScanRate(scanRate_);
	if (err != DEVICE_OK)
		return err;

	// Workaround: Set ReadytoScan to false to acquire only one image
	NiFpga_Status stat = NiFpga_WriteBool(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlBool_ReadytoScan, false);
	if (NiFpga_IsError(stat))
		return stat;

	stat = NiFpga_WriteU16(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlU16_Current, FPGA_STATE_SCAN);
	if (NiFpga_IsError(stat))
		return stat;

	return DEVICE_OK;
}


int
OpenScan::StopScan()
{
	NiFpga_Status stat = NiFpga_WriteBool(fpgaSession_,
		NiFpga_OpenScanFPGAHost_ControlBool_ReadytoScan, false);
	if (NiFpga_IsError(stat))
		return stat;

	return DEVICE_OK;
}


int
OpenScan::ReadImage()
{
	size_t nPixels = GetImageWidth() * GetImageHeight();
	delete[] imageBuffer_;
	delete[] kalmanBuffer_;
	imageBuffer_ = new uint16_t[nPixels];
	kalmanBuffer_ = new uint16_t[nPixels];

	NiFpga_Status stat = NiFpga_StartFifo(fpgaSession_,
		NiFpga_OpenScanFPGAHost_TargetToHostFifoU32_TargettohostFIFO);
	if (NiFpga_IsError(stat))
		return stat;

	uint32_t* rawAndAveraged = new uint32_t[nPixels];
	size_t readSoFar = 0;
	int32_t prevPercentRead = -1;
	size_t available = 0;
	size_t remaining = 0;
	while (readSoFar < nPixels)
	{
		int32_t percentRead = (int32_t) (readSoFar * 100 / nPixels);
		if (percentRead > prevPercentRead)
		{
			if (percentRead % 1 == 0)
			{
				std::ostringstream oss;
				oss << "Read " << percentRead << " %";
				LogMessage(oss.str().c_str(), true);
			}
			prevPercentRead = percentRead;
		}

		stat = NiFpga_ReadFifoU32(fpgaSession_,
			NiFpga_OpenScanFPGAHost_TargetToHostFifoU32_TargettohostFIFO,
			rawAndAveraged + readSoFar, 0, 3000, &available);
		if (NiFpga_IsError(stat))
			return stat;

		stat = NiFpga_ReadFifoU32(fpgaSession_,
			NiFpga_OpenScanFPGAHost_TargetToHostFifoU32_TargettohostFIFO,
			rawAndAveraged + readSoFar, available, 3000, &remaining);
		if (NiFpga_IsError(stat))
			return stat;

		readSoFar += available;

		Sleep(5);
	}

	Sleep(10);
	if (remaining > 0)
	{
		return ERR_DATA_LEFT_IN_FIFO_AFTER_READING_IMAGE;
	}

	stat = NiFpga_StopFifo(fpgaSession_,
		NiFpga_OpenScanFPGAHost_TargetToHostFifoU32_TargettohostFIFO);
	if (NiFpga_IsError(stat))
		return stat;

	for (size_t i = 0; i < nPixels; ++i) {
		imageBuffer_[i] = (uint16_t)(rawAndAveraged[i]);
		kalmanBuffer_[i] = (uint16_t)(rawAndAveraged[i] >> 16);
	}

	return DEVICE_OK;
}


SequenceThread::SequenceThread(OpenScan* camera, int32_t numberOfFrames,
	uint32_t kalmanFrames, bool stopOnOverflow) :
	camera_(camera),
	numberOfFrames_(numberOfFrames),
	kalmanFrames_(kalmanFrames),
	stopOnOverflow_(stopOnOverflow)
{
}


int
SequenceThread::Start()
{
	if (camera_->settingsChanged_)
	{
		int err = camera_->ReloadWaveform();
		if (err != DEVICE_OK)
			return err;

		camera_->settingsChanged_ = false;
	}

	int err = camera_->SetScanParameters();
	if (err != DEVICE_OK)
		return err;

	activate();

	return DEVICE_OK;
}


int
SequenceThread::AcquireFrame(unsigned kalmanCounter)
{
	int err;

	camera_->LogMessage("Starting scan...", true);
	err = camera_->StartScan();
	if (err != DEVICE_OK)
		return err;

	camera_->LogMessage("Waiting for scan to finish...", true);
	NiFpga_Bool finished = false;
	while (!finished)
	{
		NiFpga_Status stat = NiFpga_ReadBool(camera_->fpgaSession_,
			NiFpga_OpenScanFPGAHost_IndicatorBool_Frameacquisitionfinish, &finished);
		if (NiFpga_IsError(stat))
			return stat;
		Sleep(10);
	}

	camera_->LogMessage("Reading image...", true);
	err = camera_->ReadImage();
	if (err != DEVICE_OK)
		return err;

	err = camera_->SetKalmanGain(1.0 / (kalmanCounter + 2));
	if (err != DEVICE_OK)
		return err;

	camera_->LogMessage("Finished reading image", true);

	if (camera_->kalmanProgressive_ || kalmanCounter + 1 == kalmanFrames_)
	{
		return camera_->SendSequenceImages(stopOnOverflow_);
	}
	return DEVICE_OK;
}


int
SequenceThread::svc()
{
	camera_->LogMessage("Sequence thread started", true);

	NiFpga_Status stat = NiFpga_WriteI32(camera_->fpgaSession_, NiFpga_OpenScanFPGAHost_ControlI32_Framenumber, 0);
	if (NiFpga_IsError(stat))
		return stat;

	int totalFrames;
	if (numberOfFrames_ == INT32_MAX)
		totalFrames = INT32_MAX;
	else if (camera_->kalmanProgressive_)
		totalFrames = numberOfFrames_;
	else
		totalFrames = numberOfFrames_ * kalmanFrames_;

	for (int frame = 0; frame < totalFrames; ++frame)
	{
		{
			MMThreadGuard g(camera_->sequenceThreadMutex_);

			if (camera_->stopSequenceRequested_)
				break;
		}

		int err = AcquireFrame(frame % kalmanFrames_);
		if (err != DEVICE_OK)
		{
			std::ostringstream oss;
			oss << "Error during sequence acquisition: " << err;
			camera_->LogMessage(oss.str().c_str(), true);
			camera_->StopSequenceAcquisition();
			return 0;
		}

		NiFpga_Status stat = NiFpga_WriteI32(camera_->fpgaSession_,
			NiFpga_OpenScanFPGAHost_ControlI32_Framenumber, frame + 1);
		if (NiFpga_IsError(stat))
			return stat;
	}

	camera_->StopSequenceAcquisitionImpl(false);
	return 0;
}