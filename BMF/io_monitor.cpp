/**
* This file is part of Batman "Fix".
*
* Batman "Fix" is free software : you can redistribute it and / or modify
* it under the terms of the GNU General Public License as published by
* The Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* Batman "Fix" is distributed in the hope that it will be useful,
* But WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with Batman "Fix". If not, see <http://www.gnu.org/licenses/>.
**/

#define _CRT_SECURE_NO_WARNINGS

#include "io_monitor.h"
#include "log.h"

#include "framerate.h"

void
BMF_CountIO (io_perf_t& ioc, const double update)
{
  static HANDLE hProc = GetCurrentProcess ();

  if (ioc.init == false) {
    memset (&ioc, 0, sizeof (io_perf_t));
    ioc.init = true;
  }

  SYSTEMTIME     update_time;
  FILETIME       update_ftime;
  ULARGE_INTEGER update_ul;

  IO_COUNTERS current_io;

  GetProcessIoCounters (hProc, &current_io);
  GetSystemTime        (&update_time);
  SystemTimeToFileTime (&update_time, &update_ftime);

  update_ul.HighPart = update_ftime.dwHighDateTime;
  update_ul.LowPart  = update_ftime.dwLowDateTime;

  ioc.dt += update_ul.QuadPart - ioc.last_update.QuadPart;

  ioc.accum.ReadTransferCount +=
    current_io.ReadTransferCount - ioc.last_counter.ReadTransferCount;
  ioc.accum.WriteTransferCount +=
    current_io.WriteTransferCount - ioc.last_counter.WriteTransferCount;
  ioc.accum.OtherTransferCount +=
    current_io.OtherTransferCount - ioc.last_counter.OtherTransferCount;

  ioc.accum.ReadOperationCount +=
    current_io.ReadOperationCount - ioc.last_counter.ReadOperationCount;
  ioc.accum.WriteOperationCount +=
    current_io.WriteOperationCount - ioc.last_counter.WriteOperationCount;
  ioc.accum.OtherOperationCount +=
    current_io.OtherOperationCount - ioc.last_counter.OtherOperationCount;

  double dRB = (double)ioc.accum.ReadTransferCount;
  double dWB = (double)ioc.accum.WriteTransferCount;
  double dOB = (double)ioc.accum.OtherTransferCount;

  double dRC = (double)ioc.accum.ReadOperationCount;
  double dWC = (double)ioc.accum.WriteOperationCount;
  double dOC = (double)ioc.accum.OtherOperationCount;

  double& read_mb_sec  = ioc.read_mb_sec;
  double& write_mb_sec = ioc.write_mb_sec;
  double& other_mb_sec = ioc.other_mb_sec;

  double& read_iop_sec  = ioc.read_iop_sec;
  double& write_iop_sec = ioc.write_iop_sec;
  double& other_iop_sec = ioc.other_iop_sec;

  if (ioc.dt >= update) {
    read_mb_sec  = (
      read_mb_sec + ((dRB / 1048576.0) / (1.0e-7 * (double)ioc.dt))
                   ) / 2.0;
    write_mb_sec = (
      write_mb_sec + ((dWB / 1048576.0) / (1.0e-7 * (double)ioc.dt))
                   ) / 2.0;
    other_mb_sec = (
      other_mb_sec + ((dOB / 1048576.0) / (1.0e-7 * (double)ioc.dt))
                   ) / 2.0;

    read_iop_sec  = (read_iop_sec  + (dRC / (1.0e-7 * (double)ioc.dt))) / 2.0;
    write_iop_sec = (write_iop_sec + (dWC / (1.0e-7 * (double)ioc.dt))) / 2.0;
    other_iop_sec = (other_iop_sec + (dOC / (1.0e-7 * (double)ioc.dt))) / 2.0;

    ioc.accum.ReadTransferCount   = 0;
    ioc.accum.WriteTransferCount  = 0;
    ioc.accum.OtherTransferCount  = 0;

    ioc.accum.ReadOperationCount  = 0;
    ioc.accum.WriteOperationCount = 0;
    ioc.accum.OtherOperationCount = 0;

    ioc.dt = 0;
  }

  ioc.last_update.QuadPart = update_ul.QuadPart;
  memcpy (&ioc.last_counter, &current_io, sizeof (IO_COUNTERS));
}


#pragma comment (lib, "wbemuuid.lib")

bool           com_init   = false;
IWbemServices* pNameSpace = nullptr;

CRITICAL_SECTION com_cs;

bool
BMF_InitCOMLocks (void)
{
  InitializeCriticalSectionAndSpinCount (&com_cs, 5000);

  return true;
}

bool
BMF_InitCOM (void)
{
  IWbemLocator*  pWbemLocator  = nullptr;
  BSTR           bstrNameSpace = nullptr;

  HRESULT hr;

  // Every thread that uses COM has to do this, but the other stuff ...
  //   that only needs to be done once.
  if (FAILED (hr = CoInitializeEx (NULL, COINIT_MULTITHREADED)))
  {
    dll_log.Log (L"[COM] Failure to initialize COM for the calling thread "
                 L"(%s:%d) -- 0x%X",
      __FILEW__, __LINE__, hr);
    return false;
    //goto COM_CLEANUP;
  }

  if (com_init)
    return true;

  BMF_InitCOMLocks ();

  if (FAILED (hr = CoInitializeSecurity (
                     NULL,
                     -1,
                     NULL,
                     NULL,
                     RPC_C_AUTHN_LEVEL_NONE,
                     RPC_C_IMP_LEVEL_IMPERSONATE,
                     NULL, EOAC_NONE, 0 )
             )
     )
  {
    // It's possible that the application already did this, in which case
    //   it is immutable and we should try to deal with whatever the app
    //     initialized it to.
    if (hr != RPC_E_TOO_LATE) {
      dll_log.Log (L"[COM] Failure to initialize COM Security (%s:%d) -- 0x%X",
        __FILEW__, __LINE__, hr);
      goto COM_CLEANUP;
    }
  }

  if (FAILED (hr = CoCreateInstance (
                     CLSID_WbemLocator, 
                     NULL,
                     CLSCTX_INPROC_SERVER,
                     IID_IWbemLocator,
                     (void**) &pWbemLocator )
             )
     )
  {
    dll_log.Log (L"[COM] Failed to create Wbem Locator (%s:%d) -- 0x%X",
      __FILEW__, __LINE__, hr);
    goto COM_CLEANUP;
  }

  // Connect to the desired namespace.
  bstrNameSpace = SysAllocString (L"\\\\.\\Root\\CIMv2");
  if (bstrNameSpace == nullptr)
  {
    dll_log.Log (L"[COM] Out of Memory (%s:%d)",
      __FILEW__, __LINE__);
    hr = E_OUTOFMEMORY;
    goto COM_CLEANUP;
  }

  if (FAILED (hr = pWbemLocator->ConnectServer (
                     bstrNameSpace,
                     NULL, // User name
                     NULL, // Password
                     NULL, // Locale
                     0L,   // Security flags
                     NULL, // Authority
                     NULL, // Wbem context
                     &pNameSpace )
             )
     )
  {
    dll_log.Log (L"[COM] Failure to Connect to Wbem Server (%s:%d) -- 0x%X",
      __FILEW__, __LINE__, hr);
    goto COM_CLEANUP;
  }

#if 0
  pWbemLocator->Release ();
  pWbemLocator = nullptr;

  SysFreeString (bstrNameSpace);
  bstrNameSpace = nullptr;
#endif

  com_init = true;

  return true;

COM_CLEANUP:
  if (bstrNameSpace != nullptr)
  {
    SysFreeString (bstrNameSpace);
    bstrNameSpace = nullptr;
  }

  if (pWbemLocator != nullptr)
  {
    pWbemLocator->Release ();
    pWbemLocator = nullptr;
  }

  if (pNameSpace != nullptr)
  {
    pNameSpace->Release ();
    pNameSpace = nullptr;
  }

  CoUninitialize ();

  com_init = false;

  return false;
}

//
// TODO - Use reference counting for the number of times init was called...
//
void
BMF_ShutdownCOM (void)
{
  if (com_init)
  {
    if (pNameSpace != nullptr)
    {
      pNameSpace->Release ();
      pNameSpace = nullptr;
    }

    CoUninitialize ();
    com_init = false;
  }
}


cpu_perf_t cpu_stats;

#include "config.h"

DWORD
WINAPI
BMF_MonitorCPU (LPVOID user_param)
{
  EnterCriticalSection (&com_cs);

  BMF_InitCOM ();

  cpu_perf_t&  cpu    = cpu_stats;
  const double update = config.cpu.interval;

  HRESULT hr;

  if (FAILED (hr = CoCreateInstance (
                     CLSID_WbemRefresher,
                     NULL,
                     CLSCTX_INPROC_SERVER,
                     IID_IWbemRefresher, 
                     (void**) &cpu.pRefresher )
             )
     )
  {
    dll_log.Log (L" [WMI]: Failed to create Refresher Instance (%s:%d)",
      __FILEW__, __LINE__);
    goto CPU_CLEANUP;
  }

  if (FAILED (hr = cpu.pRefresher->QueryInterface (
                        IID_IWbemConfigureRefresher,
                        (void **)&cpu.pConfig )
             )
     )
  {
    dll_log.Log (L" [WMI]: Failed to Query Refresher Interface (%s:%d)",
      __FILEW__, __LINE__);
    goto CPU_CLEANUP;
  }

  // Add an enumerator to the refresher.
  if (FAILED (hr = cpu.pConfig->AddEnum (
                     pNameSpace,
                     L"Win32_PerfFormattedData_PerfOS_Processor",
                     0,
                     NULL,
                     &cpu.pEnum,
                     &cpu.lID )
             )
     )
  {
    dll_log.Log (L" [WMI]: Failed to Add Enumerator (%s:%d) - %04X",
      __FILEW__, __LINE__, hr);
    goto CPU_CLEANUP;
  }

  cpu.pConfig->Release ();
  cpu.pConfig = nullptr;

  int iter = 0;

  cpu.dwNumReturned = 0;
  cpu.dwNumObjects  = 0;

  LeaveCriticalSection (&com_cs);

  while (cpu_stats.lID != 0)
  {
    // Sleep for 500 ms.
    Sleep (DWORD (update * 1000.0));

    // Only poll WMI while the data view is visible
    if (! config.cpu.show)
      continue;

    cpu.dwNumReturned = 0;

    EnterCriticalSection (&com_cs);

    if (FAILED (hr = cpu.pRefresher->Refresh (0L)))
    {
      dll_log.Log (L" [WMI]: Failed to Refresh CPU (%s:%d)",
        __FILEW__, __LINE__);
      goto CPU_CLEANUP;
    }

    hr = cpu.pEnum->GetObjects ( 0L,
                                 cpu.dwNumObjects,
                                 cpu.apEnumAccess,
                                 &cpu.dwNumReturned );

    // If the buffer was not big enough,
    // allocate a bigger buffer and retry.
    if (hr == WBEM_E_BUFFER_TOO_SMALL 
        && cpu.dwNumReturned > cpu.dwNumObjects)
    {
      cpu.apEnumAccess = new IWbemObjectAccess* [cpu.dwNumReturned];
      if (cpu.apEnumAccess == nullptr)
      {
        dll_log.Log (L" [WMI]: Out of Memory (%s:%d)",
          __FILEW__, __LINE__);
        hr = E_OUTOFMEMORY;
        goto CPU_CLEANUP;
      }
      
      SecureZeroMemory (cpu.apEnumAccess,
                        cpu.dwNumReturned * sizeof(IWbemObjectAccess *));

      cpu.dwNumObjects = cpu.dwNumReturned;

      if (FAILED (hr = cpu.pEnum->GetObjects ( 0L,
                                               cpu.dwNumObjects,
                                               cpu.apEnumAccess,
                                               &cpu.dwNumReturned )
                 )
         )
      {
        dll_log.Log (L" [WMI]: Failed to get CPU Objects (%s:%d)",
          __FILEW__, __LINE__);

        goto CPU_CLEANUP;
      }
    }
    else
    {
      if (hr != WBEM_S_NO_ERROR)
      {
        dll_log.Log (L" [WMI]: UNKNOWN ERROR (%s:%d)",
          __FILEW__, __LINE__);
        hr = WBEM_E_NOT_FOUND;
        goto CPU_CLEANUP;
      }
    }

    // First time through, get the handles.
    if (iter == 0)
    {
      CIMTYPE PercentInterruptTimeType;
      CIMTYPE PercentPrivilegedTimeType;
      CIMTYPE PercentUserTimeType;
      CIMTYPE PercentProcessorTimeType;
      CIMTYPE PercentIdleTimeType;

      if (FAILED (hr = cpu.apEnumAccess [0]->GetPropertyHandle (
                            L"PercentInterruptTime",
                            &PercentInterruptTimeType,
                            &cpu.lPercentInterruptTimeHandle )
                 )
         )
      {
        dll_log.Log (L" [WMI]: Failed to acquire property handle (%s:%d)",
          __FILEW__, __LINE__);
        goto CPU_CLEANUP;
      }

      if (FAILED (hr = cpu.apEnumAccess [0]->GetPropertyHandle (
                            L"PercentPrivilegedTime",
                            &PercentPrivilegedTimeType,
                            &cpu.lPercentPrivilegedTimeHandle )
                 )
         )
      {
        dll_log.Log (L" [WMI]: Failed to acquire property handle (%s:%d)",
          __FILEW__, __LINE__);
        goto CPU_CLEANUP;
      }

      if (FAILED (hr = cpu.apEnumAccess [0]->GetPropertyHandle (
                            L"PercentUserTime",
                            &PercentUserTimeType,
                            &cpu.lPercentUserTimeHandle )
                 )
         )
      {
        dll_log.Log (L" [WMI]: Failed to acquire property handle (%s:%d)",
          __FILEW__, __LINE__);
        goto CPU_CLEANUP;
      }

      if (FAILED (hr = cpu.apEnumAccess [0]->GetPropertyHandle (
                            L"PercentProcessorTime",
                            &PercentProcessorTimeType,
                            &cpu.lPercentProcessorTimeHandle )
                 )
         )
      {
        dll_log.Log (L" [WMI]: Failed to acquire property handle (%s:%d)",
          __FILEW__, __LINE__);
        goto CPU_CLEANUP;
      }

      if (FAILED (hr = cpu.apEnumAccess [0]->GetPropertyHandle (
                            L"PercentIdleTime",
                            &PercentIdleTimeType,
                            &cpu.lPercentIdleTimeHandle )
                 )
         )
      {
        dll_log.Log (L" [WMI]: Failed to acquire property handle (%s:%d)",
          __FILEW__, __LINE__);
        goto CPU_CLEANUP;
      }
    }

    for (unsigned int i = 0; i < cpu.dwNumReturned; i++)
    {
      uint64_t interrupt;
      uint64_t kernel;
      uint64_t user;
      uint64_t load;
      uint64_t idle;

      if (FAILED (hr = cpu.apEnumAccess [i]->ReadQWORD (
                             cpu.lPercentInterruptTimeHandle,
                             &interrupt )
                 )
         )
      {
        dll_log.Log (L" [WMI]: Failed to read Quad-Word Property (%s:%d)",
          __FILEW__, __LINE__);
        goto CPU_CLEANUP;
      }

      if (FAILED (hr = cpu.apEnumAccess [i]->ReadQWORD (
                             cpu.lPercentPrivilegedTimeHandle,
                             &kernel )
                 )
         )
      {
        dll_log.Log (L" [WMI]: Failed to read Quad-Word Property (%s:%d)",
          __FILEW__, __LINE__);
        goto CPU_CLEANUP;
      }

      if (FAILED (hr = cpu.apEnumAccess [i]->ReadQWORD (
                             cpu.lPercentUserTimeHandle,
                             &user )
                 )
         )
      {
        dll_log.Log (L" [WMI]: Failed to read Quad-Word Property (%s:%d)",
          __FILEW__, __LINE__);
        goto CPU_CLEANUP;
      }

      if (FAILED (hr = cpu.apEnumAccess [i]->ReadQWORD (
                             cpu.lPercentProcessorTimeHandle,
                             &load )
                 )
         )
      {
        dll_log.Log (L" [WMI]: Failed to read Quad-Word Property (%s:%d)",
          __FILEW__, __LINE__);
        goto CPU_CLEANUP;
      }

      if (FAILED (hr = cpu.apEnumAccess [i]->ReadQWORD (
                             cpu.lPercentIdleTimeHandle,
                             &idle )
                 )
         )
      {
        dll_log.Log (L" [WMI]: Failed to read Quad-Word Property (%s:%d)",
          __FILEW__, __LINE__);
        goto CPU_CLEANUP;
      }

      cpu.cpus [i].percent_idle   = (cpu.cpus [i].percent_idle   + idle)   / 2;
      cpu.cpus [i].percent_load   = (cpu.cpus [i].percent_load   + load)   / 2;
      cpu.cpus [i].percent_user   = (cpu.cpus [i].percent_user   + user)   / 2;
      cpu.cpus [i].percent_kernel = (cpu.cpus [i].percent_kernel + kernel) / 2;
      cpu.cpus [i].percent_interrupt
                                  = ( cpu.cpus [i].percent_interrupt + 
                                      interrupt ) / 2;

      // Done with the object
      cpu.apEnumAccess [i]->Release ();
      cpu.apEnumAccess [i] = nullptr;
    }

    cpu.num_cpus = cpu.dwNumReturned;

    ++iter;

    LeaveCriticalSection (&com_cs);
  }

  EnterCriticalSection (&com_cs);

CPU_CLEANUP:
  //dll_log.Log (L" >> CPU_CLEANUP");

  if (cpu.apEnumAccess != nullptr)
  {
    for (unsigned int i = 0; i < cpu.dwNumReturned; i++)
    {
      if (cpu.apEnumAccess [i] != nullptr)
      {
        cpu.apEnumAccess [i]->Release ();
        cpu.apEnumAccess [i] = nullptr;
      }
    }
    delete [] cpu.apEnumAccess;
  }

  if (cpu.pEnum)
  {
    cpu.pEnum->Release ();
    cpu.pEnum = nullptr;
  }

  if (cpu.pConfig != nullptr)
  {
    cpu.pConfig->Release ();
    cpu.pConfig = nullptr;
  }

  if (cpu.pRefresher != nullptr)
  {
    cpu.pRefresher->Release ();
    cpu.pRefresher = nullptr;
  }

  CoUninitialize ();

  LeaveCriticalSection (&com_cs);

  return 0;
}

disk_perf_t disk_stats;

DWORD
WINAPI
BMF_MonitorDisk (LPVOID user)
{
  EnterCriticalSection (&com_cs);

  BMF_InitCOM ();

  //Win32_PerfFormattedData_PerfDisk_LogicalDisk

  disk_perf_t&  disk  = disk_stats;
  const double update = config.disk.interval;

  HRESULT hr;

  if (FAILED (hr = CoCreateInstance (
                     CLSID_WbemRefresher,
                     NULL,
                     CLSCTX_INPROC_SERVER,
                     IID_IWbemRefresher, 
                     (void**) &disk.pRefresher )
             )
     )
  {
    goto DISK_CLEANUP;
  }

  if (FAILED (hr = disk.pRefresher->QueryInterface (
                        IID_IWbemConfigureRefresher,
                        (void **)&disk.pConfig )
             )
     )
  {
    goto DISK_CLEANUP;
  }

  // Add an enumerator to the refresher.
  if (FAILED (hr = disk.pConfig->AddEnum (
                     pNameSpace,
                     config.disk.type == 1 ? 
                     L"Win32_PerfFormattedData_PerfDisk_LogicalDisk" :
                     L"Win32_PerfFormattedData_PerfDisk_PhysicalDisk",
                     0,
                     NULL,
                     &disk.pEnum,
                     &disk.lID )
             )
     )
  {
    goto DISK_CLEANUP;
  }

  disk.pConfig->Release ();
  disk.pConfig = nullptr;

  int iter = 0;

  disk.dwNumReturned = 0;
  disk.dwNumObjects  = 0;

  LeaveCriticalSection (&com_cs);

  while (disk_stats.lID != 0)
  {
    // Sleep for 500 ms.
    Sleep (DWORD (update * 1000.0));

    // Only poll WMI while the data view is visible
    if (! config.disk.show)
      continue;

    extern LARGE_INTEGER BMF_QueryPerf (void);
     LARGE_INTEGER now = BMF_QueryPerf ();

    disk.dwNumReturned = 0;

    EnterCriticalSection (&com_cs);

    if (FAILED (hr = disk.pRefresher->Refresh (0L)))
    {
      goto DISK_CLEANUP;
    }

    hr = disk.pEnum->GetObjects ( 0L,
                                  disk.dwNumObjects,
                                  disk.apEnumAccess,
                                 &disk.dwNumReturned );

    // If the buffer was not big enough,
    // allocate a bigger buffer and retry.
    if (hr == WBEM_E_BUFFER_TOO_SMALL 
        && disk.dwNumReturned > disk.dwNumObjects)
    {
      disk.apEnumAccess = new IWbemObjectAccess* [disk.dwNumReturned];
      if (disk.apEnumAccess == nullptr)
      {
        hr = E_OUTOFMEMORY;
        goto DISK_CLEANUP;
      }

      SecureZeroMemory (disk.apEnumAccess,
                        disk.dwNumReturned * sizeof (IWbemObjectAccess *));

      disk.dwNumObjects = disk.dwNumReturned;

      if (FAILED (hr = disk.pEnum->GetObjects ( 0L,
                                               disk.dwNumObjects,
                                               disk.apEnumAccess,
                                               &disk.dwNumReturned )
                 )
         )
      {
        goto DISK_CLEANUP;
      }
    }
    else
    {
      if (hr != WBEM_S_NO_ERROR)
      {
        hr = WBEM_E_NOT_FOUND;
        goto DISK_CLEANUP;
      }
    }

    // First time through, get the handles.
    if (iter == 0)
    {
      CIMTYPE NameType;

      CIMTYPE DiskBytesPerSecType;
      CIMTYPE DiskReadBytesPerSecType;
      CIMTYPE DiskWriteBytesPerSecType;

      CIMTYPE PercentDiskTimeType;
      CIMTYPE PercentDiskReadTimeType;
      CIMTYPE PercentDiskWriteTimeType;
      CIMTYPE PercentIdleTimeType;

      if (FAILED (hr = disk.apEnumAccess [0]->GetPropertyHandle (
                            L"Name",
                            &NameType,
                            &disk.lNameHandle )
                 )
         )
      {
        goto DISK_CLEANUP;
      }

      if (FAILED (hr = disk.apEnumAccess [0]->GetPropertyHandle (
                            L"DiskBytesPerSec",
                            &DiskBytesPerSecType,
                            &disk.lDiskBytesPerSecHandle )
                 )
         )
      {
        goto DISK_CLEANUP;
      }

      if (FAILED (hr = disk.apEnumAccess [0]->GetPropertyHandle (
                            L"DiskReadBytesPerSec",
                            &DiskReadBytesPerSecType,
                            &disk.lDiskReadBytesPerSecHandle )
                 )
         )
      {
        goto DISK_CLEANUP;
      }

      if (FAILED (hr = disk.apEnumAccess [0]->GetPropertyHandle (
                            L"DiskWriteBytesPerSec",
                            &DiskWriteBytesPerSecType,
                            &disk.lDiskWriteBytesPerSecHandle )
                 )
         )
      {
        goto DISK_CLEANUP;
      }

      if (FAILED (hr = disk.apEnumAccess [0]->GetPropertyHandle (
                            L"PercentDiskTime",
                            &PercentDiskTimeType,
                            &disk.lPercentDiskTimeHandle )
                 )
         )
      {
        goto DISK_CLEANUP;
      }

      if (FAILED (hr = disk.apEnumAccess [0]->GetPropertyHandle (
                            L"PercentDiskReadTime",
                            &PercentDiskReadTimeType,
                            &disk.lPercentDiskReadTimeHandle )
                 )
         )
      {
        goto DISK_CLEANUP;
      }

      if (FAILED (hr = disk.apEnumAccess [0]->GetPropertyHandle (
                            L"PercentDiskWriteTime",
                            &PercentDiskWriteTimeType,
                            &disk.lPercentDiskWriteTimeHandle )
                 )
         )
      {
        goto DISK_CLEANUP;
      }

      if (FAILED (hr = disk.apEnumAccess [0]->GetPropertyHandle (
                            L"PercentIdleTime",
                            &PercentIdleTimeType,
                            &disk.lPercentIdleTimeHandle )
                 )
         )
      {
        goto DISK_CLEANUP;
      }
    }

    for (unsigned int i = 0; i < disk.dwNumReturned; i++)
    {
      uint64_t percent_read;
      uint64_t percent_write;
      uint64_t percent_load;
      uint64_t percent_idle;

      uint64_t bytes_sec;
      uint64_t bytes_read_sec;
      uint64_t bytes_write_sec;

      if (FAILED (hr = disk.apEnumAccess [i]->ReadQWORD (
                             disk.lPercentDiskReadTimeHandle,
                            &percent_read )
                 )
         )
      {
        goto DISK_CLEANUP;
      }

      if (FAILED (hr = disk.apEnumAccess [i]->ReadQWORD (
                             disk.lPercentDiskWriteTimeHandle,
                            &percent_write )
                 )
         )
      {
        goto DISK_CLEANUP;
      }

      if (FAILED (hr = disk.apEnumAccess [i]->ReadQWORD (
                             disk.lPercentDiskTimeHandle,
                            &percent_load )
                 )
         )
      {
        goto DISK_CLEANUP;
      }

      if (FAILED (hr = disk.apEnumAccess [i]->ReadQWORD (
                             disk.lPercentIdleTimeHandle,
                            &percent_idle )
                 )
         )
      {
        goto DISK_CLEANUP;
      }

      if (FAILED (hr = disk.apEnumAccess [i]->ReadQWORD (
                             disk.lDiskBytesPerSecHandle,
                            &bytes_sec )
                 )
         )
      {
        goto DISK_CLEANUP;
      }

      if (FAILED (hr = disk.apEnumAccess [i]->ReadQWORD (
                             disk.lDiskWriteBytesPerSecHandle,
                            &bytes_write_sec )
                 )
         )
      {
        goto DISK_CLEANUP;
      }

      if (FAILED (hr = disk.apEnumAccess [i]->ReadQWORD (
                             disk.lDiskReadBytesPerSecHandle,
                            &bytes_read_sec )
                 )
         )
      {
        goto DISK_CLEANUP;
      }

      long bytes = 0;
      wchar_t name [64] = { L'\0' };

      if (FAILED (hr = disk.apEnumAccess [i]->ReadPropertyValue (
                             disk.lNameHandle,
                             sizeof (wchar_t) * 64,
                             &bytes,
                             (LPBYTE)name )
                 )
         )
      {
        goto DISK_CLEANUP;
      }

      WideCharToMultiByte (CP_OEMCP, 0, name, 31, disk.disks [i].name, 31, " ", NULL);
      disk.disks [i].name [31] = '\0';

      if (i == 0)
        strcpy (disk.disks [i].name, "Total");

      disk.disks [i].name [15] = '\0';

      static BMF::Framerate::Stats write_rate    [16];
      static BMF::Framerate::Stats read_rate     [16];
      static BMF::Framerate::Stats combined_rate [16];

      disk.disks [i].percent_idle   = (disk.disks [i].percent_idle   + percent_idle)   / 2;
      disk.disks [i].percent_load   = (disk.disks [i].percent_load   + percent_load)   / 2;
      disk.disks [i].percent_read   = (disk.disks [i].percent_read   + percent_read)   / 2;
      disk.disks [i].percent_write  = (disk.disks [i].percent_write  + percent_write)  / 2;

      write_rate    [i].addSample ((double)bytes_write_sec, now);
      read_rate     [i].addSample ((double)bytes_read_sec,  now);
      combined_rate [i].addSample ((double)bytes_sec,       now);

      double combined_mean = combined_rate [i].calcMean (3.0);
      double write_mean    = write_rate    [i].calcMean (3.0);
      double read_mean     = read_rate     [i].calcMean (3.0);

      disk.disks [i].bytes_sec       = isnan (combined_mean) ? 0 : combined_mean;
      disk.disks [i].write_bytes_sec = isnan (write_mean)    ? 0 : write_mean;
      disk.disks [i].read_bytes_sec  = isnan (read_mean)     ? 0 : read_mean;

      //disk.disks [i].bytes_sec       = (disk.disks [i].bytes_sec       + bytes_sec)       >> 1;
      //disk.disks [i].write_bytes_sec = (disk.disks [i].write_bytes_sec + bytes_write_sec) >> 1;
      //disk.disks [i].read_bytes_sec  = (disk.disks [i].read_bytes_sec  + bytes_read_sec)  >> 1;

      // Done with the object
      disk.apEnumAccess [i]->Release ();
      disk.apEnumAccess [i] = nullptr;
    }

    disk.num_disks = disk.dwNumReturned;

    ++iter;

    LeaveCriticalSection (&com_cs);
  }

  EnterCriticalSection (&com_cs);

DISK_CLEANUP:
  //dll_log.Log (L" >> DISK_CLEANUP");

  if (disk.apEnumAccess != nullptr)
  {
    for (unsigned int i = 0; i < disk.dwNumReturned; i++)
    {
      if (disk.apEnumAccess [i] != nullptr)
      {
        disk.apEnumAccess [i]->Release ();
        disk.apEnumAccess [i] = nullptr;
      }
    }
    delete [] disk.apEnumAccess;
  }

  if (disk.pEnum)
  {
    disk.pEnum->Release ();
    disk.pEnum = nullptr;
  }

  if (disk.pConfig != nullptr)
  {
    disk.pConfig->Release ();
    disk.pConfig = nullptr;
  }

  if (disk.pRefresher != nullptr)
  {
    disk.pRefresher->Release ();
    disk.pRefresher = nullptr;
  }

  CoUninitialize ();

  LeaveCriticalSection (&com_cs);

  return 0;
}

pagefile_perf_t pagefile_stats;

DWORD
WINAPI
BMF_MonitorPagefile (LPVOID user)
{
  EnterCriticalSection (&com_cs);

  BMF_InitCOM ();

  pagefile_perf_t&  pagefile = pagefile_stats;
  const double update = config.pagefile.interval;

  HRESULT hr;

  if (FAILED (hr = CoCreateInstance (
                     CLSID_WbemRefresher,
                     NULL,
                     CLSCTX_INPROC_SERVER,
                     IID_IWbemRefresher, 
                     (void**) &pagefile.pRefresher )
             )
     )
  {
    goto PAGEFILE_CLEANUP;
  }

  if (FAILED (hr = pagefile.pRefresher->QueryInterface (
                        IID_IWbemConfigureRefresher,
                        (void **)&pagefile.pConfig )
             )
     )
  {
    goto PAGEFILE_CLEANUP;
  }

  // Add an enumerator to the refresher.
  if (FAILED (hr = pagefile.pConfig->AddEnum (
                     pNameSpace,
                     L"Win32_PerfRawData_PerfOS_PagingFile",
                     0,
                     NULL,
                     &pagefile.pEnum,
                     &pagefile.lID )
             )
     )
  {
    goto PAGEFILE_CLEANUP;
  }

  pagefile.pConfig->Release ();
  pagefile.pConfig = nullptr;

  int iter = 0;

  pagefile.dwNumReturned = 0;
  pagefile.dwNumObjects  = 0;

  LeaveCriticalSection (&com_cs);

  while (pagefile.lID != 0)
  {
    // Sleep for 500 ms.
    Sleep ((DWORD)(update * 1000.0));

    // Only poll WMI while the pagefile stats are shown
    if (! config.pagefile.show)
      continue;

    pagefile.dwNumReturned = 0;

    EnterCriticalSection (&com_cs);

    if (FAILED (hr = pagefile.pRefresher->Refresh (0L)))
    {
      goto PAGEFILE_CLEANUP;
    }

    hr = pagefile.pEnum->GetObjects ( 0L,
                                      pagefile.dwNumObjects,
                                      pagefile.apEnumAccess,
                                      &pagefile.dwNumReturned );

    // If the buffer was not big enough,
    // allocate a bigger buffer and retry.
    if (hr == WBEM_E_BUFFER_TOO_SMALL 
        && pagefile.dwNumReturned > pagefile.dwNumObjects)
    {
      pagefile.apEnumAccess = new IWbemObjectAccess* [pagefile.dwNumReturned];
      if (pagefile.apEnumAccess == nullptr)
      {
        hr = E_OUTOFMEMORY;
        goto PAGEFILE_CLEANUP;
      }

      SecureZeroMemory (pagefile.apEnumAccess,
                        pagefile.dwNumReturned * sizeof (IWbemObjectAccess *));

      pagefile.dwNumObjects = pagefile.dwNumReturned;

      if (FAILED (hr = pagefile.pEnum->GetObjects ( 0L,
                                                    pagefile.dwNumObjects,
                                                    pagefile.apEnumAccess,
                                                    &pagefile.dwNumReturned )
                 )
         )
      {
        goto PAGEFILE_CLEANUP;
      }
    }
    else
    {
      if (hr != WBEM_S_NO_ERROR)
      {
        hr = WBEM_E_NOT_FOUND;
        goto PAGEFILE_CLEANUP;
      }
    }

    // First time through, get the handles.
    if (iter == 0)
    {
      CIMTYPE NameType;

      CIMTYPE PercentUsageType;
      CIMTYPE PercentUsagePeakType;
      CIMTYPE PercentUsage_BaseType;

      if (FAILED (hr = pagefile.apEnumAccess [0]->GetPropertyHandle (
                            L"Name",
                            &NameType,
                            &pagefile.lNameHandle )
                 )
         )
      {
        goto PAGEFILE_CLEANUP;
      }

      if (FAILED (hr = pagefile.apEnumAccess [0]->GetPropertyHandle (
                            L"PercentUsage",
                            &PercentUsageType,
                            &pagefile.lPercentUsageHandle )
                 )
         )
      {
        goto PAGEFILE_CLEANUP;
      }

      if (FAILED (hr = pagefile.apEnumAccess [0]->GetPropertyHandle (
                            L"PercentUsagePeak",
                            &PercentUsagePeakType,
                            &pagefile.lPercentUsagePeakHandle )
                 )
         )
      {
        goto PAGEFILE_CLEANUP;
      }

      if (FAILED (hr = pagefile.apEnumAccess [0]->GetPropertyHandle (
                            L"PercentUsage_Base",
                            &PercentUsage_BaseType,
                            &pagefile.lPercentUsage_BaseHandle )
                 )
         )
      {
        goto PAGEFILE_CLEANUP;
      }
    }

    for (unsigned int i = 0; i < pagefile.dwNumReturned; i++)
    {
      DWORD size;
      DWORD usage;
      DWORD usage_peak;

      if (FAILED (hr = pagefile.apEnumAccess [i]->ReadDWORD (
                                 pagefile.lPercentUsageHandle,
                                 &usage )
                 )
         )
      {
        goto PAGEFILE_CLEANUP;
      }

      if (FAILED (hr = pagefile.apEnumAccess [i]->ReadDWORD (
                                 pagefile.lPercentUsagePeakHandle,
                                 &usage_peak )
                 )
         )
      {
        goto PAGEFILE_CLEANUP;
      }

      if (FAILED (hr = pagefile.apEnumAccess [i]->ReadDWORD (
                                 pagefile.lPercentUsage_BaseHandle,
                                 &size )
                 )
         )
      {
        goto PAGEFILE_CLEANUP;
      }

      long bytes = 0;
      wchar_t name [256] = { L'\0' };

      if (FAILED (hr = pagefile.apEnumAccess [i]->ReadPropertyValue (
                             pagefile.lNameHandle,
                             sizeof (wchar_t) * 255,
                             &bytes,
                             (LPBYTE)name )
                 )
         )
      {
        goto PAGEFILE_CLEANUP;
      }

      WideCharToMultiByte (CP_OEMCP, 0, name, 255, pagefile.pagefiles [i].name,
                           255, " ", NULL);

      pagefile.pagefiles [i].name [31] = '\0';

      if (i == (pagefile.dwNumReturned - 1))
        strcpy (pagefile.pagefiles [i].name, "Total");

      pagefile.pagefiles [i].size  = (pagefile.pagefiles [i].size  + size) / 2;
      pagefile.pagefiles [i].usage = (pagefile.pagefiles [i].usage + usage)/ 2;
      pagefile.pagefiles [i].usage_peak
                                   = (pagefile.pagefiles [i].usage_peak
                                                                   +
                                                               usage_peak) / 2;

      // Done with the object
      pagefile.apEnumAccess [i]->Release ();
      pagefile.apEnumAccess [i] = nullptr;
    }

    pagefile.num_pagefiles = pagefile.dwNumReturned;

    LeaveCriticalSection (&com_cs);

    ++iter;
  }

  EnterCriticalSection (&com_cs);

PAGEFILE_CLEANUP:
  //dll_log.Log (L" >> PAGEFILE_CLEANUP");

  if (pagefile.apEnumAccess != nullptr)
  {
    for (unsigned int i = 0; i < pagefile.dwNumReturned; i++)
    {
      if (pagefile.apEnumAccess [i] != nullptr)
      {
        pagefile.apEnumAccess [i]->Release ();
        pagefile.apEnumAccess [i] = nullptr;
      }
    }
    delete [] pagefile.apEnumAccess;
  }

  if (pagefile.pEnum)
  {
    pagefile.pEnum->Release ();
    pagefile.pEnum = nullptr;
  }

  if (pagefile.pConfig != nullptr)
  {
    pagefile.pConfig->Release ();
    pagefile.pConfig = nullptr;
  }

  if (pagefile.pRefresher != nullptr)
  {
    pagefile.pRefresher->Release ();
    pagefile.pRefresher = nullptr;
  }

  CoUninitialize ();

  LeaveCriticalSection (&com_cs);

  return 0;
}