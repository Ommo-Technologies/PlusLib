/*=Plus=header=begin======================================================
Program: Plus
Copyright (c) Laboratory for Percutaneous Surgery. All rights reserved.
See License.txt for details.
=========================================================Plus=header=end*/

// Local includes
#include "PlusConfigure.h"
#include "vtkPlusOmmoSource.h"
#include "vtkPlusDataSource.h"
#include "vtkPlusChannel.h"

// VTK includes
#include <vtkMatrix4x4.h>
#include <vtkXMLDataElement.h>
#include <vtkMath.h>
#include <vtkSmartPointer.h>
#include <vtkPlusLogger.h>
#include "igsioCommon.h"

// STL includes
#include <sstream>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <iomanip>

// Required ommo::api classes
#include "sdk_utils.h"

// gRPC channel state constants
// The Ommo SDK callback returns int values matching grpc::channelz::v1::ChannelConnectivityState_State enum
// We define constants here to avoid including the heavy protobuf header
namespace {
  // These match grpc::channelz::v1::ChannelConnectivityState_State enum values:
  // UNKNOWN=0, IDLE=1, CONNECTING=2, READY=3, TRANSIENT_FAILURE=4, SHUTDOWN=5
  constexpr int GRPC_CHANNEL_STATE_UNKNOWN = 0;
  constexpr int GRPC_CHANNEL_STATE_IDLE = 1;
  constexpr int GRPC_CHANNEL_STATE_CONNECTING = 2;
  constexpr int GRPC_CHANNEL_STATE_READY = 3;
  constexpr int GRPC_CHANNEL_STATE_TRANSIENT_FAILURE = 4;
  constexpr int GRPC_CHANNEL_STATE_SHUTDOWN = 5;
}

//----------------------------------------------------------------------------

vtkStandardNewMacro(vtkPlusOmmoSource);

//----------------------------------------------------------------------------
vtkPlusOmmoSource::vtkPlusOmmoSource()
  : vtkPlusDevice()
  , GrpcAddress("localhost:50051")
  , BadDataThreshold(0.5)
  , DeviceWaitTimeoutSec(30.0)
  , ChannelReadyTimeoutSec(30.0)
  , ConnectionDelaySec(10.0)
  , BaseStationStartupDelaySec(10.0)
  , PositionScaleFactor(1.0)
  , ClientContext(nullptr)
  , DevicesRequestTag(0)
  , DataPacketCount(0)
  , FrameNumber(0)
  , IsConnected(false)
  , ChannelState(0)  // UNKNOWN
{
  this->StartThreadForInternalUpdates = true;
  this->FrameNumber = 0;
}

//----------------------------------------------------------------------------
vtkPlusOmmoSource::~vtkPlusOmmoSource()
{
  if (this->IsConnected && this->ClientContext)
  {
    if (this->DevicesRequestTag != 0)
    {
      this->ClientContext->CloseRequest(this->DevicesRequestTag);
    }
    this->ClientContext->Shutdown();
    delete this->ClientContext;
    this->ClientContext = nullptr;
  }
}

//----------------------------------------------------------------------------
void vtkPlusOmmoSource::PrintSelf(ostream& os, vtkIndent indent)
{
  vtkPlusDevice::PrintSelf(os, indent);
  os << indent << "gRPC Address: " << this->GrpcAddress << "\n";
  os << indent << "Required Device UUIDs: ";
  for (const auto& uuid : this->RequiredDeviceUuids)
  {
    os << "0x" << std::hex << uuid << std::dec << " ";
  }
  os << "\n";
  os << indent << "Connected Devices: " << this->DeviceToolMap.size() << "\n";
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOmmoSource::ReadConfiguration(vtkXMLDataElement* rootConfigElement)
{
  XML_FIND_DEVICE_ELEMENT_REQUIRED_FOR_READING(deviceConfig, rootConfigElement);
  // Read required parameters
  //XML_READ_SCALAR_ATTRIBUTE_REQUIRED(int, DeviceParameter, deviceConfig);
  // Read optional parameters
  //XML_READ_BOOL_ATTRIBUTE_OPTIONAL(EnableSomeFeature, deviceConfig);

  // Read gRPC address
  const char* grpcAddress = deviceConfig->GetAttribute("GrpcAddress");
  if (grpcAddress != nullptr)
  {
    this->GrpcAddress = grpcAddress;
  }
  LOG_INFO("Ommo gRPC Address: " << this->GrpcAddress);

  // Read bad data threshold (optional, default 0.5)
  // Poses with bad_data_indicator above this threshold will be marked as invalid/out-of-view
  double badDataThreshold = 0.5;
  if (deviceConfig->GetScalarAttribute("BadDataThreshold", badDataThreshold))
  {
    this->BadDataThreshold = badDataThreshold;
  }
  LOG_INFO("Ommo BadDataThreshold: " << this->BadDataThreshold);

  // Read device wait timeout (optional, default 30 seconds)
  // Time to wait for required devices to connect before failing
  double deviceWaitTimeoutSec = 30.0;
  if (deviceConfig->GetScalarAttribute("DeviceWaitTimeoutSec", deviceWaitTimeoutSec))
  {
    this->DeviceWaitTimeoutSec = deviceWaitTimeoutSec;
  }
  LOG_INFO("Ommo DeviceWaitTimeoutSec: " << this->DeviceWaitTimeoutSec);

  // Read channel ready timeout (optional, default 30 seconds)
  double channelReadyTimeoutSec = 30.0;
  if (deviceConfig->GetScalarAttribute("ChannelReadyTimeoutSec", channelReadyTimeoutSec))
  {
    this->ChannelReadyTimeoutSec = channelReadyTimeoutSec;
  }
  LOG_INFO("Ommo ChannelReadyTimeoutSec: " << this->ChannelReadyTimeoutSec);

  // Read connection delay when no RequiredDeviceIds (optional, default 10 seconds)
  // Applied after channel is READY, before starting motor
  double connectionDelaySec = 10.0;
  if (deviceConfig->GetScalarAttribute("ConnectionDelaySec", connectionDelaySec))
  {
    this->ConnectionDelaySec = connectionDelaySec;
  }
  LOG_INFO("Ommo ConnectionDelaySec: " << this->ConnectionDelaySec);

  // Read base station startup delay (optional, default 10 seconds)
  // Time to wait for the base station motor to spin up after starting
  double baseStationStartupDelaySec = 10.0;
  if (deviceConfig->GetScalarAttribute("BaseStationStartupDelaySec", baseStationStartupDelaySec))
  {
    this->BaseStationStartupDelaySec = baseStationStartupDelaySec;
  }
  LOG_INFO("Ommo BaseStationStartupDelaySec: " << this->BaseStationStartupDelaySec);

  // Read position scale factor (optional, default 1.0)
  // Ommo SDK position units: 1.0=mm, 10.0=cm-to-mm, 1000.0=meters-to-mm
  double positionScaleFactor = 1.0;
  if (deviceConfig->GetScalarAttribute("PositionScaleFactor", positionScaleFactor))
  {
    this->PositionScaleFactor = positionScaleFactor;
  }
  LOG_INFO("Ommo PositionScaleFactor: " << this->PositionScaleFactor);

  // Read and parse required device IDs (optional)
  const char* requiredDeviceIds = deviceConfig->GetAttribute("RequiredDeviceIds");
  if (requiredDeviceIds != nullptr)
  {
    std::string idsString(requiredDeviceIds);
    this->RequiredDeviceUuids.clear();
    
    // Parse comma-separated list and convert to UUIDs immediately
    std::stringstream ss(idsString);
    std::string id;
    while (std::getline(ss, id, ','))
    {
      // Trim whitespace
      id.erase(0, id.find_first_not_of(" \t"));
      id.erase(id.find_last_not_of(" \t") + 1);
      if (!id.empty())
      {
        try
        {
          // Parse as decimal or hex (with 0x prefix)
          uint32_t uuid = static_cast<uint32_t>(std::stoul(id, nullptr, 10));
          this->RequiredDeviceUuids.push_back(uuid);
          LOG_DEBUG("Parsed required device ID: " << id << " -> " << uuid);
        }
        catch (const std::exception& e)
        {
          LOG_ERROR("Failed to parse required device ID as UUID: " << id << " (" << e.what() << ")");
          return PLUS_FAIL;
        }
        catch (...)
        {
          LOG_ERROR("Failed to parse required device ID as UUID: " << id);
          return PLUS_FAIL;
        }
      }
    }
    LOG_INFO("Required Device IDs: " << idsString << " (" << this->RequiredDeviceUuids.size() << " UUID(s) parsed)");
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOmmoSource::WriteConfiguration(vtkXMLDataElement* rootConfigElement)
{
  // Configuration values (GrpcAddress, RequiredDeviceUuids) are user-provided
  // and already in the XML file, so no need to write them back.
  // Just call the base class to handle DataSources (tools, etc.)
  return vtkPlusDevice::WriteConfiguration(rootConfigElement);
}


//----------------------------------------------------------------------------
PlusStatus vtkPlusOmmoSource::Probe()
{
  LOG_TRACE("Probing for Ommo tracking system");

  // Try to connect to the Ommo service
  try
  {
    ommo::api::ClientContext testContext(this->GrpcAddress.c_str());
    testContext.Start();
    
    // If we can start the context, the service is available
    testContext.Shutdown();
    LOG_TRACE("Ommo service found at: " << this->GrpcAddress);
    return PLUS_SUCCESS;
  }
  catch (...)
  {
    LOG_WARNING("Ommo service not available at: " << this->GrpcAddress);
    return PLUS_FAIL;
  }
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOmmoSource::InternalConnect()
{
  try
  {
    // Create ClientContext with configured address
    this->ClientContext = new ommo::api::ClientContext(this->GrpcAddress.c_str());
    
    // Start the client context
    this->ClientContext->Start();
    
    // Register callbacks using lambdas to capture 'this'
    this->ClientContext->RegisterChannelStateCallback(
      [this](int state) { this->ChannelStateHandler(state); });
    this->ClientContext->RegisterDeviceEventCallback(
      [this](const ommo::api::TrackingDeviceEvent& event) { this->DeviceEventHandler(event); });
   
    // Initialize channel state
    {
      std::lock_guard<std::mutex> lock(this->ChannelStateMutex);
      this->ChannelState = GRPC_CHANNEL_STATE_UNKNOWN;  // Will be updated by callback
    }
    
    LOG_INFO("Attempting to connect to Ommo service at: " << this->GrpcAddress);
    LOG_INFO("Note: Connection will be established asynchronously. Channel state will be reported via callback.");
    
    return PLUS_SUCCESS;
  }
  catch (const std::exception& e)
  {
    LOG_ERROR("Failed to connect to Ommo service: " << e.what());
    if (this->ClientContext)
    {
      delete this->ClientContext;
      this->ClientContext = nullptr;
    }
    return PLUS_FAIL;
  }
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOmmoSource::InternalDisconnect()
{
  if (this->ClientContext)
  {
    this->ClientContext->Shutdown();
    delete this->ClientContext;
    this->ClientContext = nullptr;
  }

  {
    std::lock_guard<std::mutex> lock(this->DeviceToolMapMutex);
    this->DeviceToolMap.clear();
  }

  this->IsConnected = false;
  LOG_INFO("Disconnected from Ommo service");
  
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOmmoSource::InternalStartRecording()
{
  // Step 1: Wait for gRPC channel to become READY (notified by ChannelStateHandler)
  {
    std::unique_lock<std::mutex> channelLock(this->ChannelStateMutex);
    LOG_INFO("Waiting for gRPC channel to become READY/CONNECTING (timeout: " << this->ChannelReadyTimeoutSec << "s)...");
    bool channelReady = this->ChannelReadyCondition.wait_for(
      channelLock,
      std::chrono::milliseconds(static_cast<int>(this->ChannelReadyTimeoutSec * 1000)),
      [this] {
        return this->ChannelState == GRPC_CHANNEL_STATE_READY ||
               this->ChannelState == GRPC_CHANNEL_STATE_CONNECTING;
      }
    );
    if (!channelReady)
    {
      LOG_ERROR("Cannot start recording: gRPC channel did not reach READY/CONNECTING within "
                << this->ChannelReadyTimeoutSec << " seconds");
      return PLUS_FAIL;
    }
    LOG_INFO("gRPC channel state is now READY/CONNECTING - proceeding with startup");
  }

  // Step 2: Start motor so devices can be discovered, then wait for devices or delay
  // Start motor before waiting for devices - devices only appear when motor is running
  if (!this->ClientContext->SetBaseStationMotorRunning(true))
  {
    LOG_WARNING("Failed to start base station motor - tracking may not work");
  }
  else
  {
    LOG_INFO("Base station motor started, waiting " << this->BaseStationStartupDelaySec << " seconds for spin-up...");
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(this->BaseStationStartupDelaySec * 1000)));
    LOG_INFO("Base station spin-up delay complete");
  }

  // Step 3: Wait for required devices (if specified) or fixed delay (if not)
  {
    std::unique_lock<std::mutex> deviceLock(this->DeviceToolMapMutex);

    if (!this->RequiredDeviceUuids.empty())
    {
      LOG_INFO("Waiting for required devices to connect: " << this->RequiredDeviceUuids.size()
               << " device(s), timeout: " << this->DeviceWaitTimeoutSec << "s");
      bool allDevicesConnected = this->DevicesReadyCondition.wait_for(
        deviceLock,
        std::chrono::milliseconds(static_cast<int>(this->DeviceWaitTimeoutSec * 1000)),
        [this] {
          for (uint32_t requiredUuid : this->RequiredDeviceUuids)
          {
            bool found = false;
            for (const auto& pair : this->DeviceToolMap)
            {
              if (pair.first.siu_uuid == requiredUuid)
              {
                found = true;
                break;
              }
            }
            if (!found) return false;
          }
          return true;
        }
      );
      if (!allDevicesConnected)
      {
        size_t connectedCount = this->DeviceToolMap.size();
        deviceLock.unlock();
        LOG_ERROR("Cannot start recording: Required devices did not connect within "
                  << this->DeviceWaitTimeoutSec << " seconds. Connected: " << connectedCount
                  << ", Required: " << this->RequiredDeviceUuids.size());
        return PLUS_FAIL;
      }
      LOG_INFO("All required devices are connected (" << this->RequiredDeviceUuids.size() << " device(s))");
    }
    else
    {
      deviceLock.unlock();
      LOG_INFO("No RequiredDeviceIds specified, waiting " << this->ConnectionDelaySec << " seconds for device discovery...");
      std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(this->ConnectionDelaySec * 1000)));
      LOG_INFO("Device discovery delay complete");
    }
  }

  // Start data acquisition
  // Create a default data request for all devices
  ommo::api::DataRequestUPtr req_ptr(ommo::api::CreateDefaultDataRequest());
  this->DevicesRequestTag = this->ClientContext->RequestDeviceData(*req_ptr);
   
  // Register data callback (optional - we can also poll in InternalUpdate)
  this->ClientContext->RegisterTrackingDeviceDataCallback(this->DevicesRequestTag,
    [this](const ommo::api::TrackingDeviceData& data) { this->TrackingDeviceDataHandler(data); });

  this->IsConnected = true;
  this->WaitForFirstToolSamplesAfterDataRequest();
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
void vtkPlusOmmoSource::WaitForFirstToolSamplesAfterDataRequest()
{
  constexpr double kMaxWaitSec = 15.0;
  constexpr int kPollIntervalMs = 20;

  bool needWait = false;
  {
    std::lock_guard<std::mutex> lock(this->DeviceToolMapMutex);
    for (const auto& pair : this->DeviceToolMap)
    {
      if (!pair.second.tools.empty())
      {
        needWait = true;
        break;
      }
    }
  }
  if (!needWait)
  {
    LOG_WARNING("Ommo: No tracker tools registered; skipping wait for first sample.");
    return;
  }

  double waitedSec = 0.0;
  while (waitedSec < kMaxWaitSec)
  {
    this->InternalUpdate();

    bool allHaveSample = true;
    {
      std::lock_guard<std::mutex> lock(this->DeviceToolMapMutex);
      for (const auto& pair : this->DeviceToolMap)
      {
        for (vtkPlusDataSource* tool : pair.second.tools)
        {
          if (tool == nullptr)
          {
            continue;
          }
          double ts = 0.0;
          if (tool->GetLatestTimeStamp(ts) != ITEM_OK)
          {
            allHaveSample = false;
            break;
          }
        }
        if (!allHaveSample)
        {
          break;
        }
      }
    }

    if (allHaveSample)
    {
      LOG_INFO("Ommo: First tracking samples received for all connected tools.");
      return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    waitedSec += kPollIntervalMs / 1000.0;
  }

  LOG_WARNING("Ommo: No tracking samples within " << kMaxWaitSec
              << "s after starting data request (tool out of range or stream delay). "
              << "Downstream devices may log errors until data arrives.");
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOmmoSource::InternalStopRecording()
{
  // Stop data acquisition
  if (this->DevicesRequestTag != 0)
  {
    this->ClientContext->ResetTrackingDeviceDataCallback(this->DevicesRequestTag);
    this->ClientContext->CloseRequest(this->DevicesRequestTag);
    this->DevicesRequestTag = 0;
  }
  
  // Stop the base station motor
  if (this->ClientContext)
  {
    if (!this->ClientContext->SetBaseStationMotorRunning(false))
    {
      LOG_WARNING("Failed to stop base station motor");
    }
    else
    {
      LOG_INFO("Base station motor stopped");
    }
  }
  
  this->IsConnected = false;
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOmmoSource::InternalUpdate()
{
  if (!this->IsConnected || !this->ClientContext)
  {
    return PLUS_FAIL;
  }

  std::lock_guard<std::mutex> lock(this->DeviceToolMapMutex);
  
  if (this->DeviceToolMap.empty())
  {
    return PLUS_SUCCESS;  // Not an error, just no devices yet
  }

  // Poll for device data packets from each port
  for (auto& pair : this->DeviceToolMap)
  {
    const DevicePortKey& key = pair.first;
    DeviceToolInfo& info = pair.second;  // Non-const so we can update packet_start_index
    
    if (info.tools.empty())
    {
      continue;  // No tools created for this port yet
    }

    // Create DeviceID for this port
    ommo::api::DeviceID deviceId;
    deviceId.siu_uuid = key.siu_uuid;
    deviceId.port_id = key.port_id;
    
    // Request only the data that has been received since the last check for this specific device
    // This ensures that only new data is returned
    int32_t startIndex = static_cast<int32_t>(info.packet_start_index);
    
    // Wrap response in unique_ptr for automatic cleanup
    ommo::api::DataResponseUPtr response(this->ClientContext->GetDataSinceIndex(this->DevicesRequestTag, deviceId, startIndex));
    
    if (!response || response->state == ommo::api::DataResponseState::kNoData)
    {
      continue;
    }

    // Update the index AFTER processing, matching the simple_client example
    // Process all packets and all poses within each packet
    for (uint32_t i = 0; i < response->packet_count; ++i)
    {
      const ommo::api::DevicePacket* packet = &response->packets[i];
      const ommo::api::TrackingDeviceData& deviceData = packet->device_data;
      // Process all poses in this packet - each pose corresponds to a sensor
      // Dynamically create tools if we see more poses than we have tools
      if (deviceData.pose_count > info.tools.size())
      {
        LOG_INFO("Port " << key.siu_uuid << ":" << key.port_id << " has " << deviceData.pose_count 
                 << " poses but only " << info.tools.size() << " tools. Creating additional tools.");
        
        vtkPlusChannel* outputChannel = nullptr;
        if (this->GetFirstOutputChannel(outputChannel) == PLUS_SUCCESS)
        {
          // Create missing tools
          for (uint32_t sensor_index = info.tools.size(); sensor_index < deviceData.pose_count; sensor_index++)
          {
            std::stringstream toolId;
            toolId << "Ommo_" << key.siu_uuid << "_P" << key.port_id << "_S" << sensor_index;
            
            // Create transform name for the tool
            igsioTransformName toolTransformName(toolId.str(), this->ToolReferenceFrameName);
            std::string toolTransformNameStr = toolTransformName.GetTransformName();
            
            // Check if tool already exists
            vtkPlusDataSource* tool = nullptr;
            if (outputChannel->GetTool(tool, toolTransformNameStr) != PLUS_SUCCESS)
            {
              tool = vtkPlusDataSource::New();
              tool->SetId(toolTransformNameStr);
              tool->SetType(DATA_SOURCE_TYPE_TOOL);
              tool->SetReferenceCoordinateFrameName(this->ToolReferenceFrameName);
              
              // Add tool to the channel
              if (outputChannel->AddTool(tool) != PLUS_SUCCESS)
              {
                LOG_ERROR("Failed to add tool to channel: " << toolTransformNameStr);
                tool->Delete();
                continue;
              }
              
              // Also add tool to the device's Tools container
              if (this->AddTool(tool) != PLUS_SUCCESS)
              {
                LOG_ERROR("Failed to add tool to device: " << toolTransformNameStr);
                outputChannel->RemoveTool(toolTransformNameStr);
                tool->Delete();
                continue;
              }
            }
            
            info.tools.push_back(tool);
            LOG_INFO("Created tool for sensor " << sensor_index << ": " << toolTransformNameStr);
          }
        }
      }
      
      for (uint32_t j = 0; j < deviceData.pose_count; j++)
      {
        // Get the tool for this sensor (pose index j maps to sensor index j)
        if (j >= info.tools.size())
        {
          LOG_WARNING("Pose index " << j << " exceeds number of tools (" << info.tools.size() 
                      << ") for port " << key.siu_uuid << ":" << key.port_id << ". Skipping pose.");
          continue;
        }
        
        vtkPlusDataSource* tool = info.tools[j];
        if (!tool)
        {
          continue;
        }
        
        const ommo::api::PoseData& pose = deviceData.poses[j];
        // Convert quaternion + position to 4x4 transformation matrix
        vtkNew<vtkMatrix4x4> matrix;
        matrix->Identity();
        
        // Extract quaternion components (w, x, y, z)
        double qw = pose.quaternion.w;
        double qx = pose.quaternion.x;
        double qy = pose.quaternion.y;
        double qz = pose.quaternion.z;
        
        // Normalize quaternion to ensure it represents a valid rotation
        double qnorm = sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
        if (qnorm > 1e-6)  // Avoid division by zero
        {
          qw /= qnorm;
          qx /= qnorm;
          qy /= qnorm;
          qz /= qnorm;
        }
        else
        {
          // Default to identity quaternion if invalid
          qw = 1.0;
          qx = qy = qz = 0.0;
        }
        
        // Convert quaternion to rotation matrix
        // VTK uses row-major format, so we build the matrix row by row
        matrix->Element[0][0] = 1 - 2 * (qy * qy + qz * qz);
        matrix->Element[0][1] = 2 * (qx * qy - qz * qw);
        matrix->Element[0][2] = 2 * (qx * qz + qy * qw);
        matrix->Element[1][0] = 2 * (qx * qy + qz * qw);
        matrix->Element[1][1] = 1 - 2 * (qx * qx + qz * qz);
        matrix->Element[1][2] = 2 * (qy * qz - qx * qw);
        matrix->Element[2][0] = 2 * (qx * qz - qy * qw);
        matrix->Element[2][1] = 2 * (qy * qz + qx * qw);
        matrix->Element[2][2] = 1 - 2 * (qx * qx + qy * qy);
        
        // Position: scale to mm for Plus (PositionScaleFactor: 1=mm, 10=cm, 1000=meters)
        matrix->Element[0][3] = pose.position.x * this->PositionScaleFactor;
        matrix->Element[1][3] = pose.position.y * this->PositionScaleFactor;
        matrix->Element[2][3] = pose.position.z * this->PositionScaleFactor;
        
        // Get timestamp from latency_timestamps if available (contains epoch milliseconds).
        // If the SDK timestamp is missing or implausible (far from local clock), fall back to
        // system time to keep Plus buffers time-consistent.
        double timestamp = vtkIGSIOAccurateTimer::GetSystemTime();
        if (deviceData.latency_timestamps != nullptr && deviceData.latency_timestamp_count > 0)
        {
          uint64_t epochMs = deviceData.latency_timestamps[0].system_timestamp_milliseconds;
          if (epochMs > 0)
          {
            double sdkTimestampSec = static_cast<double>(epochMs) / 1000.0;
            double nowSec = vtkIGSIOAccurateTimer::GetSystemTime();
            constexpr double maxAllowedClockSkewSec = 10.0;
            if (fabs(sdkTimestampSec - nowSec) <= maxAllowedClockSkewSec)
            {
              timestamp = sdkTimestampSec;
            }
          }
        }
        
        // Determine tool status based on pose quality indicators
        // Default to TOOL_OK - only mark as invalid if bad_data_indicator exceeds threshold
        ToolStatus status = TOOL_OK;
        if (pose.bad_data_indicator > this->BadDataThreshold)
        {
          // High bad_data_indicator suggests invalid or poor quality data (e.g., out of tracking range)
          status = TOOL_OUT_OF_VIEW;
        }
        // Note: motion_indicator=0 doesn't necessarily mean out of view in Ommo SDK
        // It may just indicate no motion detected, which is normal for stationary tools
        // We'll only mark as OUT_OF_VIEW if we have explicit indication of tracking issues
        // For now, rely on bad_data_indicator for status determination
        
        // Update tool with transform for this sensor's pose
        PlusStatus updateStatus = this->ToolTimeStampedUpdate(
          tool->GetId(),
          matrix,
          status,
          this->FrameNumber++,
          timestamp
        );
      }
    }
    
    // Update the index for next request based on what we just received
    // This matches the simple_client example: update AFTER processing
    if (response->packet_count > 0)
    {
      uint32_t new_index = response->packets[response->packet_count - 1].packet_idx + 1;
      info.packet_start_index = new_index;
    }

  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
std::string vtkPlusOmmoSource::GetSdkVersion()
{
  return "Ommo SDK v1.0";
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOmmoSource::NotifyConfigured()
{
  vtkPlusChannel* outputChannel = nullptr;
  if (this->GetFirstOutputChannel(outputChannel) != PLUS_SUCCESS)
  {
    LOG_ERROR("No output channel defined for Ommo device");
    return PLUS_FAIL;
  }

  // RequiredDeviceUuids are already parsed and validated in ReadConfiguration()
  if (!this->RequiredDeviceUuids.empty())
  {
    LOG_INFO("Required device UUIDs configured: " << this->RequiredDeviceUuids.size() << " device(s)");
  }

  // Tools will be created dynamically when devices connect
  // No pre-configured tools needed
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
void vtkPlusOmmoSource::ChannelStateHandler(int channel_state)
{
  {
    std::lock_guard<std::mutex> lock(this->ChannelStateMutex);
    this->ChannelState = channel_state;
  }
  
  // Map state values to names (matching protobuf ChannelConnectivityState_State enum)
  const char* stateNames[] = {"UNKNOWN", "IDLE", "CONNECTING", "READY", "TRANSIENT_FAILURE", "SHUTDOWN"};
  const char* stateName = (channel_state >= GRPC_CHANNEL_STATE_UNKNOWN && channel_state <= GRPC_CHANNEL_STATE_SHUTDOWN) 
    ? stateNames[channel_state] : "INVALID";
  
  LOG_INFO("Ommo service channel state changed: " << channel_state << " (" << stateName << ")");
  
  // Update IsConnected based on channel state
  // Only consider connected when channel is READY
  if (channel_state == GRPC_CHANNEL_STATE_READY)
  {
    if (!this->IsConnected)
    {
      LOG_INFO("Ommo service channel is now READY - connection established");
      this->IsConnected = true;
    }
    this->ChannelReadyCondition.notify_all();
  }
  else
  {
    if (this->IsConnected && (channel_state == GRPC_CHANNEL_STATE_TRANSIENT_FAILURE || channel_state == GRPC_CHANNEL_STATE_SHUTDOWN))
    {
      LOG_WARNING("Ommo service channel state indicates connection lost (state: " << stateName << ")");
      this->IsConnected = false;
    }
  }
}

//----------------------------------------------------------------------------
void vtkPlusOmmoSource::DeviceEventHandler(const ommo::api::TrackingDeviceEvent& device_event)
{
  if (device_event.connected)
  {
    this->HandleDeviceConnect(device_event.device);
  }
  else
  {
    this->HandleDeviceDisconnect(device_event.device);
  }
}

//----------------------------------------------------------------------------
void vtkPlusOmmoSource::HandleDeviceConnect(const ommo::api::DeviceDescriptor& device)
{
  std::lock_guard<std::mutex> lock(this->DeviceToolMapMutex);
  
  // If RequiredDeviceIds is specified, only accept devices in that list
  if (!this->RequiredDeviceUuids.empty())
  {
    bool isRequired = false;
    for (uint32_t requiredUuid : this->RequiredDeviceUuids)
    {
      if (device.siu_uuid == requiredUuid)
      {
        isRequired = true;
        break;
      }
    }
    if (!isRequired)
    {
      LOG_INFO("Ignoring device " << device.siu_uuid << ":" << device.port_id 
               << " - not in RequiredDeviceIds list");
      return;
    }
  }

  // Ports with no sensor units (e.g. SIUS hub or non-sensing port 0) still appear as connected
  // devices in the SDK but never produce poses. Do not create tools or map entries — empty
  // tracker buffers break channels that expect every tool to have timestamps.
  if (device.sensor_unit_descriptor_count == 0)
  {
    LOG_INFO("Skipping port " << device.siu_uuid << ":" << device.port_id
             << " - no sensor units (non-sensor / hub port)");
    return;
  }
  
  // Create key from siu_uuid and port_id - each port is tracked separately
  DevicePortKey key{device.siu_uuid, device.port_id};
  
  // Check if this specific port already exists
  if (this->DeviceToolMap.find(key) != this->DeviceToolMap.end())
  {
    LOG_WARNING("Port " << device.siu_uuid << ":" << device.port_id << " already connected");
    return;
  }

  // Get the first output channel
  vtkPlusChannel* outputChannel = nullptr;
  if (this->GetFirstOutputChannel(outputChannel) != PLUS_SUCCESS)
  {
    LOG_ERROR("No output channel available for port " << device.siu_uuid << ":" << device.port_id);
    return;
  }

  // Each port corresponds to one physical device.
  // - sensor_unit_descriptor_count = number of physical mag sensors on the device (hardware config)
  // - pose_count in data = number of tracking outputs (depends on fusion mode)
  // These may differ: e.g., FULL_FUSION can combine multiple sensors into fewer poses.
  // We create tools based on pose_count (what we actually receive), not sensor count.
  // Start with 1 tool per device - additional tools created dynamically in InternalUpdate if pose_count > 1.
  uint32_t sensor_count = 1;
  
  LOG_INFO("Port " << device.siu_uuid << ":" << device.port_id << " connected (device has " 
           << device.sensor_unit_descriptor_count << " sensor unit(s), initially creating " << sensor_count << " tool)");
  
  DeviceToolInfo info;
  info.packet_start_index = 0;  // Initialize packet start index to 0
  info.tools.clear();
  
  // Create one tool per sensor unit on this port
  for (uint32_t sensor_index = 0; sensor_index < sensor_count; sensor_index++)
  {
    // Create tool name: Ommo_<UUID>_P<port> or Ommo_<UUID>_P<port>_S<sensor>
    // Use compact format so the first 20 chars are unique (OpenIGTLink IGTL_TDATA_LEN_NAME=20)
    std::stringstream toolId;
    toolId << "Ommo_" << device.siu_uuid << "_P" << device.port_id;
    if (sensor_count > 1)
    {
      toolId << "_S" << sensor_index;
    }
    
    std::string toolIdStr = toolId.str();
    LOG_DEBUG("Creating tool for device " << device.siu_uuid << ", sensor " << sensor_index 
             << ", toolId base: " << toolIdStr);
    
    // Create transform name for the tool (matches XML configuration behavior)
    igsioTransformName toolTransformName(toolIdStr, this->ToolReferenceFrameName);
    std::string toolTransformNameStr = toolTransformName.GetTransformName();
    
    // Check if tool already exists
    vtkPlusDataSource* tool = nullptr;
    if (outputChannel->GetTool(tool, toolTransformNameStr) != PLUS_SUCCESS)
    {
      tool = vtkPlusDataSource::New();
      // Set ID to transform name (e.g., "OmmoDevice_1001_Sensor_0ToTracker") to match XML configuration behavior
      tool->SetId(toolTransformNameStr);
      tool->SetType(DATA_SOURCE_TYPE_TOOL);
      tool->SetReferenceCoordinateFrameName(this->ToolReferenceFrameName);
      
      // Add tool to the channel
      if (outputChannel->AddTool(tool) != PLUS_SUCCESS)
      {
        LOG_ERROR("Failed to add tool to channel: " << toolTransformNameStr);
        tool->Delete();
        continue;  // Continue with next sensor
      }
      
      // Also add tool to the device's Tools container so GetTool() can find it
      if (this->AddTool(tool) != PLUS_SUCCESS)
      {
        LOG_ERROR("Failed to add tool to device: " << toolTransformNameStr);
        outputChannel->RemoveTool(toolTransformNameStr);
        tool->Delete();
        continue;  // Continue with next sensor
      }
    }
    
    info.tools.push_back(tool);
    LOG_INFO("Ommo sensor connected - SIU UUID: " << device.siu_uuid 
             << ", Port ID: " << device.port_id 
             << ", Sensor Index: " << sensor_index
             << ", Tool ID: " << toolTransformNameStr);
  }
  
  this->DeviceToolMap[key] = info;
  
  LOG_INFO("Ommo port connected - SIU UUID: " << device.siu_uuid 
           << ", Port ID: " << device.port_id 
           << ", Sensor Count: " << sensor_count
           << ", Tools Created: " << info.tools.size());
  
  // Check if all required devices are now connected, and notify only if so
  if (!this->RequiredDeviceUuids.empty())
  {
    bool allRequiredConnected = true;
    for (uint32_t requiredUuid : this->RequiredDeviceUuids)
    {
      // Check if at least one port with this UUID is connected
      bool found = false;
      for (const auto& pair : this->DeviceToolMap)
      {
        if (pair.first.siu_uuid == requiredUuid)
        {
          found = true;
          break;
        }
      }
      if (!found)
      {
        allRequiredConnected = false;
        break;
      }
    }
    
    if (allRequiredConnected)
    {
      LOG_DEBUG("All required devices are now connected - notifying waiting threads");
      this->DevicesReadyCondition.notify_all();
    }
  }
  else
  {
    // If no required devices specified, notify on any device connection
    // (though the wait condition will return immediately anyway)
    this->DevicesReadyCondition.notify_all();
  }
}

//----------------------------------------------------------------------------
void vtkPlusOmmoSource::HandleDeviceDisconnect(const ommo::api::DeviceDescriptor& device)
{
  std::lock_guard<std::mutex> lock(this->DeviceToolMapMutex);
  
  DevicePortKey key{device.siu_uuid, device.port_id};
  auto it = this->DeviceToolMap.find(key);
  if (it != this->DeviceToolMap.end())
  {
    DeviceToolInfo& info = it->second;
    
    LOG_INFO("Ommo port disconnected - SIU UUID: " << device.siu_uuid 
             << ", Port ID: " << device.port_id
             << ", Removing " << info.tools.size() << " sensor tool(s)");
    
    // Remove all tools for this device (one per sensor)
    vtkPlusChannel* outputChannel = nullptr;
    if (this->GetFirstOutputChannel(outputChannel) == PLUS_SUCCESS)
    {
      for (vtkPlusDataSource* tool : info.tools)
      {
        if (tool)
        {
          std::string toolId = tool->GetId();
          
          // Remove tool from device's Tools container
          auto toolIt = this->Tools.find(toolId);
          if (toolIt != this->Tools.end())
          {
            this->Tools.erase(toolIt);
          }
          
          // Remove tool from channel
          outputChannel->RemoveTool(toolId);
        }
      }
    }
    
    // Remove from our internal map
    this->DeviceToolMap.erase(it);
  }
}

//----------------------------------------------------------------------------
void vtkPlusOmmoSource::TrackingDeviceDataHandler(const ommo::api::TrackingDeviceData& device_data)
{
  // This callback is called for every tracking packet
  // Note: Keep processing minimal here as it's called from the gRPC thread
  this->DataPacketCount++;
}


