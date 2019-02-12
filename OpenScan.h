#pragma once

#include "DeviceBase.h"
#include "DeviceThreads.h"

#include <OpenScanLib.h>

#include <map>
#include <string>
#include <vector>

class OpenScan;
class OpenScanMagnifier;


class OpenScanHub : public HubBase<OpenScanHub>
{
public:
	typedef int (OpenScanMagnifier::*MagChangeNotifierType)();

private:
	OpenScan* openScanCamera_;
	OpenScanMagnifier* magnifier_;
	MagChangeNotifierType magChangeNotifier_;

public:
	OpenScanHub() :
		openScanCamera_(0),
		magnifier_(0),
		magChangeNotifier_(0)
	{}
	~OpenScanHub() {}

	// Device API
	int Initialize();
	int Shutdown() { return DEVICE_OK; }
	void GetName(char* pName) const;
	bool Busy() { return false; }

	// Hub api
	int DetectInstalledDevices();

public: // Internal interface for peripherals
	void SetCameraDevice(OpenScan* camera);
	void SetMagnificationChangeNotifier(OpenScanMagnifier* magnifier,
		MagChangeNotifierType notifier);

	int GetMagnification(double* mag);
	int OnMagnifierChanged();
};


class OpenScan : public CCameraBase<OpenScan>
{
private:
	OSc_LSM* oscLSM_;

	std::vector< std::pair<size_t, size_t> > resolutions_;

	std::vector<void*> snappedImages_; // Memory manually managed
	OSc_Acquisition* sequenceAcquisition_;
	bool sequenceAcquisitionStopOnOverflow_;

private: // Pre-init config
	std::map<std::string, OSc_Device*> clockDevices_;
	std::map<std::string, OSc_Device*> scannerDevices_;
	std::map<std::string, OSc_Device*> detectorDevices_;

public:
	OpenScan();
	virtual ~OpenScan();

	virtual int Initialize();
	virtual int Shutdown();

	virtual bool Busy();
	virtual void GetName(char* name) const;

	// Camera
	virtual int SnapImage();
	virtual const unsigned char* GetImageBuffer() { return GetImageBuffer(0); }
	virtual const unsigned char* GetImageBuffer(unsigned chan);

	virtual long GetImageBufferSize() const;
	virtual unsigned GetImageWidth() const;
	virtual unsigned GetImageHeight() const;
	virtual unsigned GetImageBytesPerPixel() const;
	virtual unsigned GetNumberOfComponents() const;
	virtual unsigned GetNumberOfChannels() const;
	virtual int GetChannelName(unsigned channel, char* name);
	virtual unsigned GetBitDepth() const;

	virtual int GetBinning() const { return 1; }
	virtual int SetBinning(int) { return DEVICE_OK; }
	virtual double GetExposure() const { return 0.0; }
	virtual void SetExposure(double) { }

	virtual int SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize);
	virtual int GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize);
	virtual int ClearROI();

	virtual int StartSequenceAcquisition(long count, double intervalMs, bool stopOnOverflow);
	virtual int StartSequenceAcquisition(double intervalMs)
	{
		return StartSequenceAcquisition(LONG_MAX, intervalMs, false);
	}
	virtual int StopSequenceAcquisition();
	virtual bool IsCapturing();

	virtual int IsExposureSequenceable(bool& f) const { f = false; return DEVICE_OK; }

private: // Property handlers
	int OnResolution(MM::PropertyBase* pProp, MM::ActionType eAct);

	std::vector<OSc_Setting*> settingIndex_;
	int OnStringProperty(MM::PropertyBase* pProp, MM::ActionType eAct, long data);
	int OnBoolProperty(MM::PropertyBase* pProp, MM::ActionType eAct, long data);
	int OnInt32Property(MM::PropertyBase* pProp, MM::ActionType eAct, long data);
	int OnFloat64Property(MM::PropertyBase* pProp, MM::ActionType eAct, long data);
	int OnEnumProperty(MM::PropertyBase* pProp, MM::ActionType eAct, long data);

public: // Internal functions called from non-class context
	void LogOpenScanMessage(const char *msg, OSc_Log_Level level);
	void StoreSnapImage(OSc_Acquisition* acq, uint32_t chan, void* pixels);
	bool SendSequenceImage(OSc_Acquisition* acq, uint32_t chan, void* pixels);

public: // Internal interface
	int GetMagnification(double *magnification);

private:
	int InitializeResolution(OSc_Device* clockDevice, OSc_Device* scannerDevice, OSc_Device* detectorDevice);
	int GenerateProperties();
	int GenerateProperties(OSc_Setting** settings, size_t count);
	void DiscardPreviouslySnappedImages();
};


class OpenScanAO : public CSignalIOBase<OpenScanAO>
{
public:
	OpenScanAO();
	virtual ~OpenScanAO();

	virtual int Initialize();
	virtual int Shutdown();

	virtual void GetName(char* name) const;
	virtual bool Busy() { return false; }

	virtual int SetGateOpen(bool open);
	virtual int GetGateOpen(bool& open);
	virtual int SetSignal(double volts);
	virtual int GetSignal(double& /* volts */) { return DEVICE_UNSUPPORTED_COMMAND; }
	virtual int GetLimits(double& minVolts, double& maxVolts);
};



// Magnifier for scaling pixel size with respect to resolution and zoom change
class OpenScanMagnifier : public CMagnifierBase<OpenScanMagnifier>
{
public:
	OpenScanMagnifier();
	~OpenScanMagnifier() {};

	int Shutdown();
	void GetName(char* name) const;
	bool Busy() { return false; }
	int Initialize();

	double GetMagnification();

private:
	int HandleMagnificationChange();
};