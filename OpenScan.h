#pragma once

#include "DeviceBase.h"
#include "DeviceThreads.h"

#include <OpenScanLib.h>

#include <map>
#include <string>


class OpenScan : public CCameraBase<OpenScan>
{
private:
	OSc_LSM* oscLSM_;

	std::vector<void*> snappedImages_; // Memory manually managed
	OSc_Acquisition* sequenceAcquisition_;
	bool sequenceAcquisitionStopOnOverflow_;

private: // Pre-init config
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

public:
	void LogOpenScanMessage(const char *msg, OSc_Log_Level level);
	void StoreSnapImage(OSc_Acquisition* acq, uint32_t chan, void* pixels);
	bool SendSequenceImage(OSc_Acquisition* acq, uint32_t chan, void* pixels);

private:
	void DiscardPreviouslySnappedImages();
};