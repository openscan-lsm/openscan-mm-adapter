#pragma once

#include "NiFpga_OpenScanFPGAHost.h"

#include "DeviceBase.h"
#include "DeviceThreads.h"

class SequenceThread;


class OpenScan : public CCameraBase<OpenScan>
{
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
	virtual int StopSequenceAcquisition() { return StopSequenceAcquisitionImpl(true); }
	virtual bool IsCapturing();

	virtual int IsExposureSequenceable(bool& f) const { f = false; return DEVICE_OK; }

private: // Property handlers
	int OnScanRate(MM::PropertyBase* pProp, MM::ActionType eAct);
	int OnResolution(MM::PropertyBase* pProp, MM::ActionType eAct);
	int OnZoom(MM::PropertyBase* pProp, MM::ActionType eAct);

	int OnChannels(MM::PropertyBase* pProp, MM::ActionType eAct);

	int OnKalmanProgressive(MM::PropertyBase* pProp, MM::ActionType eAct);
	int OnKalmanAverageFrames(MM::PropertyBase* pProp, MM::ActionType eAct);
	int OnKalmanFilterGain(MM::PropertyBase* pProp, MM::ActionType eAct);

	int OnDebugFPGAState(MM::PropertyBase* pProp, MM::ActionType eAct);
	int OnDebugWaveformWrittenToDRAM(MM::PropertyBase* pProp, MM::ActionType eAct);
	int OnDebugWaveformOutputFinished(MM::PropertyBase* pProp, MM::ActionType eAct);
	int OnDebugFrameAcquisitionFinished(MM::PropertyBase* pProp, MM::ActionType eAct);
	int OnDebugAveragedImageDisplayed(MM::PropertyBase* pProp, MM::ActionType eAct);

private: // Internal functions
	int SetScanRate(double scanRate);
	int SetResolution(uint32_t resolution);

	int StartFPGA();
	int ReloadWaveform();
	int SendParameters();
	int InitScan();
	int SetScanParameters();
	int SetKalmanGain(double kg);
	int WriteWaveforms(uint16_t *firstX, uint16_t *firstY);
	int MoveGalvosTo(uint16_t x, uint16_t y);
	int StartScan();
	int StopScan();
	int ReadImage();

	int SendSequenceImages(bool stopOnOverflow);
	int SendSequenceImage(bool stopOnOverflow, int chan, const unsigned char* pixels);
	int StopSequenceAcquisitionImpl(bool wait);

private:
	NiFpga_Session fpgaSession_;

	// True if settings have changed since last acquisition,
	// indicating we need to reset the FPGA
	bool settingsChanged_;

	double scanRate_;
	uint32_t resolution_;
	double zoom_;

	enum {
		CHANNELS_RAW_IMAGE,
		CHANNELS_KALMAN_AVERAGED,
		CHANNELS_RAW_AND_KALMAN,
	} channels_;

	bool kalmanProgressive_;
	double filterGain_;
	uint32_t kalmanFrames_;

	uint16_t* imageBuffer_;
	uint16_t* kalmanBuffer_;

	friend class SequenceThread;
	MMThreadLock sequenceThreadMutex_;
	SequenceThread* sequenceThread_;
	bool stopSequenceRequested_;
};


class SequenceThread : public MMDeviceThreadBase
{
public:
	SequenceThread(OpenScan* camera, int32_t numberOfFrames,
		uint32_t kalmanFrames, bool stopOnOverflow);
	int AcquireFrame(unsigned kalmanCounter);
	int Start();

private:
	OpenScan* camera_;
	int32_t numberOfFrames_;
	uint32_t kalmanFrames_;
	bool stopOnOverflow_;

	int svc();
};