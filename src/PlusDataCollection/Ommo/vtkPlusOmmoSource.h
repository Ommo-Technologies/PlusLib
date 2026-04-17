/*=Plus=header=begin======================================================
Program: Plus
Copyright (c) Laboratory for Percutaneous Surgery. All rights reserved.
See License.txt for details.
=========================================================Plus=header=end*/

#ifndef __vtkPlusOmmoSource_h
#define __vtkPlusOmmoSource_h

#include "vtkPlusDataCollectionExport.h"
#include "vtkPlusDevice.h"

// Required ommo::api classes
#include "client_context.h"

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <condition_variable>

class vtkPlusDataSource;
class vtkXMLDataElement;

class vtkPlusDataCollectionExport vtkPlusOmmoSource : public vtkPlusDevice
{
public:
  static vtkPlusOmmoSource* New();
  vtkTypeMacro(vtkPlusOmmoSource, vtkPlusDevice);
  virtual void PrintSelf(ostream& os, vtkIndent indent) VTK_OVERRIDE;

  virtual PlusStatus ReadConfiguration(vtkXMLDataElement* rootConfigElement) VTK_OVERRIDE;

  virtual PlusStatus WriteConfiguration(vtkXMLDataElement* rootConfigElement) VTK_OVERRIDE;

  virtual PlusStatus Probe() VTK_OVERRIDE;
  virtual std::string GetSdkVersion() VTK_OVERRIDE;

  virtual bool IsTracker() const { return true; }

  /*! Called after all devices have been configured / inputs & outputs are connected / collection of data not started*/
  virtual PlusStatus NotifyConfigured() VTK_OVERRIDE;

protected:
  vtkPlusOmmoSource();
  ~vtkPlusOmmoSource();

  virtual PlusStatus InternalConnect() VTK_OVERRIDE;
  virtual PlusStatus InternalDisconnect() VTK_OVERRIDE;
  virtual PlusStatus InternalStartRecording() VTK_OVERRIDE;
  virtual PlusStatus InternalStopRecording() VTK_OVERRIDE;
  virtual PlusStatus InternalUpdate() VTK_OVERRIDE;


private:
  vtkPlusOmmoSource(const vtkPlusOmmoSource&);
  void operator=(const vtkPlusOmmoSource&);
  
  void ChannelStateHandler(int channel_state);
  void DeviceEventHandler(const ommo::api::TrackingDeviceEvent& device_event);
  void TrackingDeviceDataHandler(const ommo::api::TrackingDeviceData& device_data);
  
  void HandleDeviceConnect(const ommo::api::DeviceDescriptor& device);
  void HandleDeviceDisconnect(const ommo::api::DeviceDescriptor& device);

  /*! Poll tracking until each connected tool has at least one buffer sample (or timeout). Avoids empty-channel errors at startup. */
  void WaitForFirstToolSamplesAfterDataRequest();

  std::string GrpcAddress;
  std::vector<uint32_t> RequiredDeviceUuids;  // Parsed UUIDs from RequiredDeviceIds config (validated in ReadConfiguration)
  double BadDataThreshold;  // Threshold for bad_data_indicator above which tool is marked invalid (default 0.5)
  double DeviceWaitTimeoutSec;  // Timeout in seconds to wait for required devices to connect (default 30.0)
  double ChannelReadyTimeoutSec;  // Timeout in seconds to wait for gRPC channel to become READY (default 30.0)
  double ConnectionDelaySec;  // Delay in seconds when no RequiredDeviceIds (after channel READY, default 10.0)
  double BaseStationStartupDelaySec;  // Delay in seconds to wait for base station motor to spin up (default 10.0)
  double PositionScaleFactor;  // Scale position to mm: 1.0=mm, 10.0=cm, 1000.0=meters (default 1.0)
  
  ommo::api::ClientContext* ClientContext;
  uint32_t DevicesRequestTag;
  std::atomic_uint32_t DataPacketCount;
  
  // DevicePortKey uniquely identifies a device by SIU UUID and port ID
  // This is needed because the same SIU can have multiple ports, each with its own sensors
  struct DevicePortKey
  {
    uint32_t siu_uuid;
    uint32_t port_id;
    
    bool operator<(const DevicePortKey& other) const
    {
      if (siu_uuid != other.siu_uuid) return siu_uuid < other.siu_uuid;
      return port_id < other.port_id;
    }
    
    bool operator==(const DevicePortKey& other) const
    {
      return siu_uuid == other.siu_uuid && port_id == other.port_id;
    }
  };
  
  // Map from (SIU UUID, port ID) to device info (which contains tools, one per sensor on that port)
  struct DeviceToolInfo
  {
    std::vector<vtkPlusDataSource*> tools;  // One tool per sensor unit on this port
    uint32_t packet_start_index;  // Track packet start index for GetDataSinceIndex
  };
  std::map<DevicePortKey, DeviceToolInfo> DeviceToolMap;
  std::mutex DeviceToolMapMutex;
  
  unsigned long FrameNumber;
  
  bool IsConnected;
  int ChannelState;  // Track gRPC channel state (see GRPC_CHANNEL_STATE_* constants in .cxx file)
  std::mutex ChannelStateMutex;
  std::condition_variable ChannelReadyCondition;   // Signaled when gRPC channel becomes READY
  std::condition_variable DevicesReadyCondition;   // Signaled when required devices connect
};

#endif
