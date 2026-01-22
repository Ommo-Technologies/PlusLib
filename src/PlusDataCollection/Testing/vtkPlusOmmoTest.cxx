/*=Plus=header=begin======================================================
  Program: Plus
  Copyright (c) Laboratory for Percutaneous Surgery. All rights reserved.
  See License.txt for details.
=========================================================Plus=header=end*/

// Local includes
#include "PlusConfigure.h"
#include "vtkCallbackCommand.h"
#include "vtkCommand.h"
#include "vtkPlusDataSource.h"
#include "vtkSmartPointer.h"
#include "vtkPlusOmmoSource.h"
#include "vtkXMLUtilities.h"
#include "vtksys/CommandLineArguments.hxx"
#include "vtksys/SystemTools.hxx"
#include "igtlOSUtil.h" // for igtl::Sleep

// STL includes
#include <iostream>

void PrintLogsCallback(vtkObject* obj, unsigned long eid, void* clientdata, void* calldata);

int main(int argc, char** argv)
{
  bool printHelp(false);
  std::string inputConfigFileName = "Testing/PlusDeviceSet_DataCollectionOnly_Ommo.xml";

  vtksys::CommandLineArguments args;
  args.Initialize(argc, argv);

  int verboseLevel = vtkPlusLogger::LOG_LEVEL_UNDEFINED;
  bool renderingOff(false);

  args.AddArgument("--help", vtksys::CommandLineArguments::NO_ARGUMENT, &printHelp, "Print this help.");
  args.AddArgument("--verbose", vtksys::CommandLineArguments::EQUAL_ARGUMENT, &verboseLevel, "Verbose level (1=error only, 2=warning, 3=info, 4=debug, 5=trace)");
  args.AddArgument("--config-file", vtksys::CommandLineArguments::EQUAL_ARGUMENT, &inputConfigFileName, "Config file containing the device configuration.");
  args.AddArgument("--rendering-off", vtksys::CommandLineArguments::NO_ARGUMENT, &renderingOff, "Run test without rendering.");

  if (!args.Parse())
  {
    std::cerr << "Problem parsing arguments" << std::endl;
    std::cout << "\n\nvtkPlusOmmoTest help:" << args.GetHelp() << std::endl;
    exit(EXIT_FAILURE);
  }

  vtkPlusLogger::Instance()->SetLogLevel(verboseLevel);

  if (printHelp)
  {
    std::cout << "\n\nvtkPlusOmmoTest help:" << args.GetHelp() << std::endl;
    exit(EXIT_SUCCESS);
  }

  vtkNew<vtkPlusOmmoSource> ommoDevice;
  ommoDevice->SetDeviceId("TrackerDevice");

  std::cout << "Checking config file: " << inputConfigFileName << std::endl;
  if (!vtksys::SystemTools::FileExists(inputConfigFileName))
  {
    LOG_ERROR("Bad configuration file: " << inputConfigFileName);
    exit(EXIT_FAILURE);
  }

  LOG_DEBUG("Reading config file: " << inputConfigFileName);
  auto* configRead = vtkXMLUtilities::ReadElementFromFile(inputConfigFileName.c_str());

  if (!configRead)
  {
    std::cerr << "ERROR: Failed to read configuration file" << std::endl;
    LOG_ERROR("Failed to read configuration file");
    exit(EXIT_FAILURE);
  }

  std::cout << "Parsing configuration..." << std::endl;
  LOG_TRACE("Config file: " << *configRead);
  if (ommoDevice->ReadConfiguration(configRead) != PLUS_SUCCESS)
  {
    std::cerr << "ERROR: Failed to read configuration" << std::endl;
    LOG_ERROR("Failed to read configuration");
    exit(EXIT_FAILURE);
  }

  std::cout << "Validating configuration..." << std::endl;
  if (ommoDevice->NotifyConfigured() != PLUS_SUCCESS)
  {
    std::cerr << "ERROR: Invalid configuration" << std::endl;
    LOG_ERROR("Invalid configuration");
    exit(EXIT_FAILURE);
  }

  std::cout << "Device configuration:" << std::endl;
  vtkIndent indent;
  ommoDevice->PrintSelf(std::cout, indent);
  std::cout << std::endl;

  // Add an observer to warning and error events for redirecting it to the stdout
  vtkNew<vtkCallbackCommand> callbackCommand;
  callbackCommand->SetCallback(PrintLogsCallback);
  ommoDevice->AddObserver("WarningEvent", callbackCommand);
  ommoDevice->AddObserver("ErrorEvent", callbackCommand);

  std::cout << "Connecting to device..." << std::endl;
  if (ommoDevice->Connect() != PLUS_SUCCESS)
  {
    std::cerr << "ERROR: Failed to connect" << std::endl;
    LOG_ERROR("Failed to connect");
    exit(EXIT_FAILURE);
  }
  std::cout << "Connected successfully." << std::endl;

  // Give the connection time to establish (gRPC connection is asynchronous)
  // InternalStartRecording() will also wait for the channel to be READY, but
  // a small delay here ensures the connection process has started
  LOG_INFO("Waiting for connection to establish...");
  igtl::Sleep(500);  // 500ms should be enough for connection to start

  if (ommoDevice->StartRecording() != PLUS_SUCCESS)
  {
    LOG_ERROR("Failed to start recording - channel may not be ready or devices not connected");
    exit(EXIT_FAILURE);
  }
  
  LOG_INFO("Recording started successfully");

  // Wait for some data
  igtl::Sleep(2000);

  if (ommoDevice->StopRecording() != PLUS_SUCCESS)
  {
    LOG_INFO("Failed to stop recording");
    exit(EXIT_FAILURE);
  }

  if (ommoDevice->Disconnect() != PLUS_SUCCESS)
  {
    LOG_ERROR("Failed to disconnect");
    exit(EXIT_FAILURE);
  }

  LOG_INFO("Exit successfully");
  exit(EXIT_SUCCESS);
}

// Callback function for error and warning redirects
void PrintLogsCallback(vtkObject* obj, unsigned long eid, void* clientdata, void* calldata)
{
  if (eid == vtkCommand::GetEventIdFromString("WarningEvent"))
  {
    LOG_WARNING((const char*)calldata);
  }
  else if (eid == vtkCommand::GetEventIdFromString("ErrorEvent"))
  {
    LOG_ERROR((const char*)calldata);
  }
}
