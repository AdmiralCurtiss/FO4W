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

#include "osd.h"

#include "RTSSSharedMemory.h"

#include <shlwapi.h>
#include <float.h>
#include <io.h>
#include <tchar.h>

#include "config.h"
#include "io_monitor.h"
#include "gpu_monitor.h"
#include "memory_monitor.h"

#include "core.h"

#include "log.h"

#define OSD_PRINTF   if (config.osd.show)     { pszOSD += sprintf (pszOSD,
#define OSD_M_PRINTF if (config.osd.show &&\
                         config.mem.show)     { pszOSD += sprintf (pszOSD,
#define OSD_B_PRINTF if (config.osd.show &&\
                         config.load_balance\
                         .use)                { pszOSD += sprintf (pszOSD,
#define OSD_S_PRINTF if (config.osd.show &&\
                         config.mem.show &&\
                         config.sli.show)     { pszOSD += sprintf (pszOSD,
#define OSD_C_PRINTF if (config.osd.show &&\
                         config.cpu.show)     { pszOSD += sprintf (pszOSD,
#define OSD_G_PRINTF if (config.osd.show &&\
                         config.gpu.show)     { pszOSD += sprintf (pszOSD,
#define OSD_D_PRINTF if (config.osd.show &&\
                         config.disk.show)    { pszOSD += sprintf (pszOSD,
#define OSD_P_PRINTF if (config.osd.show &&\
                         config.pagefile.show)\
                                              { pszOSD += sprintf (pszOSD,
#define OSD_I_PRINTF if (config.osd.show &&\
                         config.io.show)      { pszOSD += sprintf (pszOSD,
#define OSD_END    ); }

extern char* szOSD;

#include "nvapi.h"
extern NV_GET_CURRENT_SLI_STATE sli_state;
extern BOOL nvapi_init;

// Probably need to use a critical section to make this foolproof, we will
//   cross that bridge later though. The OSD is performance critical
bool osd_shutting_down = false;

// Initialize some things (like color, position and scale) on first use
bool osd_init          = false;

static CRITICAL_SECTION osd_cs  = { 0 };
static bool             cs_init = false;

class BMF_AutoCriticalSection {
public:
  BMF_AutoCriticalSection ( CRITICAL_SECTION* pCS,
                            bool              try_only = false )
  {
    cs_ = pCS;

    if (try_only)
      TryEnter ();
    else {
      Enter ();
    }
  }

  ~BMF_AutoCriticalSection (void)
  {
    Leave ();
  }

  bool try_result (void)
  {
    return acquired_;
  }

protected:
  bool TryEnter (_Acquires_lock_(* this->cs_) void)
  {
    return (acquired_ = (TryEnterCriticalSection (cs_) != FALSE));
  }

  void Enter (_Acquires_lock_(* this->cs_) void)
  {
    EnterCriticalSection (cs_);

    acquired_ = true;
  }

  void Leave (_Releases_lock_(* this->cs_) void)
  {
    if (acquired_ != false)
      LeaveCriticalSection (cs_);

    acquired_ = false;
  }

private:
  bool              acquired_;
  CRITICAL_SECTION* cs_;
};

BOOL
BMF_ReleaseSharedMemory (LPVOID lpMemory)
{
  //BMF_AutoCriticalSection auto_cs (&osd_cs);

  if (lpMemory != nullptr) {
    return UnmapViewOfFile (lpMemory);
  }

  return FALSE;
}

LPVOID
BMF_GetSharedMemory (DWORD dwProcID)
{
  //BMF_AutoCriticalSection auto_cs (&osd_cs);

  if (osd_shutting_down && osd_init == false)
    return nullptr;

  HANDLE hMapFile =
    OpenFileMappingA (FILE_MAP_ALL_ACCESS, FALSE, "RTSSSharedMemoryV2");

  if (hMapFile) {
    LPVOID               pMapAddr =
      MapViewOfFile (hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, 0);

    // We got our pointer, now close the file... we'll clean this pointer up later
    CloseHandle (hMapFile);

    LPRTSS_SHARED_MEMORY pMem     =
      (LPRTSS_SHARED_MEMORY)pMapAddr;

    if (pMem)
    {
      if ((pMem->dwSignature == 'RTSS') && 
          (pMem->dwVersion >= 0x00020000))
      {
        // ProcID is a wild-card, just return memory without checking to see if RTSS
        //   knows about a particular process.
        if (dwProcID == 0)
          return pMapAddr;

        for (DWORD dwApp = 0; dwApp < pMem->dwAppArrSize; dwApp++)
        {
          RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_APP_ENTRY pApp =
            (RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_APP_ENTRY)
              ((LPBYTE)pMem + pMem->dwAppArrOffset +
                      dwApp * pMem->dwAppEntrySize);

          if (pApp->dwProcessID == dwProcID)
          {
            // Everything is good and RTSS knows about dwProcID!
            return pMapAddr;
          }
        }
      }

      // We got a pointer, but... it was not to useable RivaTuner memory
      UnmapViewOfFile (pMapAddr);
    }
  }

  return nullptr;
}

LPVOID
BMF_GetSharedMemory (void)
{
  //BMF_AutoCriticalSection auto_cs (&osd_cs);

  return BMF_GetSharedMemory (GetCurrentProcessId ());
}

#include "log.h"

std::wstring
BMF_GetAPINameFromOSDFlags (DWORD dwFlags)
{
  // Both are DXGI-based and probable
  if (dwFlags & APPFLAG_D3D11)
    return L"D3D11";
  if (dwFlags & APPFLAG_D3D10)
    return L"D3D10";

  if (dwFlags & APPFLAG_OGL)
    return L"OpenGL";

  // Plan to expand this to D3D9 eventually
  if (dwFlags & APPFLAG_D3D9EX)
    return L"D3D9EX";
  if (dwFlags & APPFLAG_D3D9)
    return L"D3D9";

#ifdef OLDER_D3D_SUPPORT
  if (dwFlags & APPFLAG_D3D8)
    return L"D3D8";
  if (dwFlags & APPFLAG_DD)
    return L"DDRAW";
#endif

  return L"UNKNOWN";
}

enum BMF_UNITS {
  Celsius    = 0,
  Fahrenheit = 1,
  B          = 2,
  KiB        = 3,
  MiB        = 4,
  GiB        = 5,
  Auto       = 32
};

std::wstring
BMF_SizeToString (uint64_t size, BMF_UNITS unit = Auto)
{
  wchar_t str [64];

  if (unit == Auto) {
    if      (size > (1ULL << 32ULL)) unit = GiB;
    else if (size > (1ULL << 22ULL)) unit = MiB;
    else if (size > (1ULL << 12ULL)) unit = KiB;
    else                             unit = B;
  }

  switch (unit)
  {
    case GiB:
      _swprintf (str, L"%#5llu GiB", size >> 30);
      break;
    case MiB:
      _swprintf (str, L"%#5llu MiB", size >> 20);
      break;
    case KiB:
      _swprintf (str, L"%#5llu KiB", size >> 10);
      break;
    case B:
    default:
      _swprintf (str, L"%#3llu Bytes", size);
      break;
  }

  return str;
}

std::wstring
BMF_SizeToStringF (uint64_t size, int width, int precision, BMF_UNITS unit = Auto)
{
  wchar_t str [64];

  if (unit == Auto) {
    if      (size > (1ULL << 32ULL)) unit = GiB;
    else if (size > (1ULL << 22ULL)) unit = MiB;
    else if (size > (1ULL << 12ULL)) unit = KiB;
    else                             unit = B;
  }

  switch (unit)
  {
  case GiB:
    _swprintf (str, L"%#*.*f GiB", width, precision,
              (float)size / (1024.0f * 1024.0f * 1024.0f));
    break;
  case MiB:
    _swprintf (str, L"%#*.*f MiB", width, precision,
              (float)size / (1024.0f * 1024.0f));
    break;
  case KiB:
    _swprintf (str, L"%#*.*f KiB", width, precision, (float)size / 1024.0f);
    break;
  case B:
  default:
    _swprintf (str, L"%#*llu Bytes", width-1-precision, size);
    break;
  }

  return str;
}

std::wstring
BMF_FormatTemperature (int32_t in_temp, BMF_UNITS in_unit, BMF_UNITS out_unit)
{
  int32_t converted;
  wchar_t wszOut [16];

  if (in_unit == Celsius && out_unit == Fahrenheit) {
    //converted = in_temp * 2 + 30;
    converted = (int32_t)((float)(in_temp) * 9.0f/5.0f + 32.0f);
    _swprintf (wszOut, L"%#3li�F", converted);
  } else if (in_unit == Fahrenheit && out_unit == Celsius) {
    converted = (int32_t)(((float)in_temp - 32.0f) * (5.0f/9.0f));
    _swprintf (wszOut, L"%#2li�C", converted);
  } else {
    _swprintf (wszOut, L"%#2li�C", in_temp);
  }

  return wszOut;
}


#include <dwmapi.h>
#pragma comment (lib, "dwmapi.lib")

#define CINTERFACE
#include <d3d9.h>
extern IDirect3DSwapChain9* g_pSwapChain9;

BOOL
__stdcall
BMF_DrawExternalOSD (std::string app_name, std::string text)
{
  if (! cs_init) {
    InitializeCriticalSectionAndSpinCount (&osd_cs, 123UL);
    cs_init = true;
  }

  BMF_UpdateOSD (text.c_str (), nullptr, app_name.c_str ());

  return TRUE;
}

BOOL
BMF_DrawOSD (void)
{
  if (! cs_init) {
    InitializeCriticalSectionAndSpinCount (&osd_cs, 123UL);
    cs_init = true;
  }

  //BMF_AutoCriticalSection auto_cs (&osd_cs, true);

  //if (! auto_cs.try_result ())
    //return false;

  static unsigned int connect_attempts = 1;

  // Bail-out early when shutting down, or RTSS does not know about our process
  LPVOID pMemory = BMF_GetSharedMemory ();

  if (! pMemory) {
    ++connect_attempts;
    LeaveCriticalSection (&osd_cs);
    return false;
  }

  if (! osd_init) {
    DwmEnableMMCSS (TRUE);

    osd_init = true;

    extern bmf_logger_t dll_log;

    dll_log.LogEx ( true,
      L"[RTSS] Opening Connection to RivaTuner Statistics Server... " );

    dll_log.LogEx ( false,
      L"successful after %u attempt(s)!\n", connect_attempts );

    BMF_SetOSDScale (config.osd.scale);
    BMF_SetOSDPos   (config.osd.pos_x, config.osd.pos_y);
    BMF_SetOSDColor (config.osd.red, config.osd.green, config.osd.blue);
  }

  char* pszOSD = szOSD;
  *pszOSD = '\0';

  static io_perf_t
    io_counter;

  buffer_t buffer = mem_info [0].buffer;
  int      nodes  = mem_info [buffer].nodes;

  extern std::wstring BMF_VER_STR;

  if (config.time.show)
  {
    SYSTEMTIME st;
    GetLocalTime (&st);

    wchar_t time [64];
    GetTimeFormat (config.time.format,0L,&st,NULL,time,64);

    OSD_PRINTF "Fallout 4 \"Works\" v 0.0.3   %ws\n\n",
      time
    OSD_END
  }

  if (config.fps.show)
  {
    LPRTSS_SHARED_MEMORY pMem =
      (LPRTSS_SHARED_MEMORY)pMemory;

    if (pMem)
    {
      if ((pMem->dwSignature == 'RTSS') && 
          (pMem->dwVersion >= 0x00020000))
      {
        for (DWORD dwApp = 0; dwApp < pMem->dwAppArrSize; dwApp++)
        {
          RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_APP_ENTRY pApp =
            (RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_APP_ENTRY)
              ((LPBYTE)pMem + pMem->dwAppArrOffset +
                      dwApp * pMem->dwAppEntrySize);

          //
          // Print the API Statistics and Framerate
          //
          if (pApp->dwProcessID == GetCurrentProcessId ())
          {
            static float last_ms = pApp->dwFrameTime / 1000.0f;

            // Logic to eliminate frametime hiccups from hacking Kernel32.dll
            if (pApp->dwFrameTime / 1000.0f < 1000.0f)
              last_ms = pApp->dwFrameTime / 1000.0f;

            std::wstring api_name = BMF_GetAPINameFromOSDFlags (pApp->dwFlags);
            OSD_PRINTF "  %-6ws :  %#4.01f FPS, %#13.01f ms",
              api_name.c_str (),
                // Cast to FP to avoid integer division by zero.
                1000.0f * (float)pApp->dwFrames / (float)(pApp->dwTime1 - pApp->dwTime0),
                  last_ms
                //1000000.0f / pApp->dwFrameTime,
                  //pApp->dwFrameTime / 1000.0f
            OSD_END

#if 0
            extern IDXGISwapChain2* g_pSwapChain2;
            if (g_pSwapChain2 != nullptr) {
              DXGI_FRAME_STATISTICS stats;
              g_pSwapChain2->GetFrameStatistics (&stats);

              static uint32_t start_frame = stats.PresentRefreshCount;

              OSD_PRINTF ",  %lu Dropped Frames (%3.1f%%)",
                         stats.PresentRefreshCount -
                         stats.PresentCount        -,
                 (float)(stats.PresentRefreshCount - stats.PresentCount -
                        (stats.PresentRefreshCount - start_frame)) /
                 (float)(stats.PresentRefreshCount -
                        (stats.PresentRefreshCount - start_frame))
              OSD_END
            }
#endif

            OSD_PRINTF "\n" OSD_END

#ifdef DUMP_SWAPCHAIN_INFO
            if (g_pSwapChain9 != nullptr) {
              D3DDISPLAYMODE        dmode;
              D3DPRESENT_PARAMETERS pparams;

              g_pSwapChain9->GetDisplayMode       (&dmode);
              g_pSwapChain9->GetPresentParameters (&pparams);

              OSD_PRINTF "  SWAPCH :  %#3lu Hz (%#4lux%#4lu) - Flags: 0x%04X, Refresh: %#3lu Hz, Interval: %#2lu, Effect: %lu\n",
                dmode.RefreshRate, dmode.Width, dmode.Height,
                pparams.Flags, pparams.FullScreen_RefreshRateInHz, pparams.PresentationInterval, pparams.SwapEffect
              OSD_END
            }
#endif
            break;
          }
        }
      }
    }
  }

  // Poll GPU stats...
  BMF_PollGPU ();

  int afr_idx  = sli_state.currentAFRIndex,
      afr_last = sli_state.previousFrameAFRIndex,
      afr_next = sli_state.nextFrameAFRIndex;

  for (int i = 0; i < gpu_stats.num_gpus; i++) {
    OSD_G_PRINTF "  GPU%i   :            %#3lu%%",
      i, gpu_stats.gpus [i].loads_percent.gpu
    OSD_END

    if (nvapi_init && gpu_stats.gpus [i].loads_percent.vid > 0) {
      // Vector 3D (subtract 1 space)
      OSD_G_PRINTF ",  VID%i %#3lu%% ,",

      // Raster 3D
      //OSD_G_PRINTF ",  VID%i %#3lu%%  ,",
        i, gpu_stats.gpus [i].loads_percent.vid
      OSD_END
    } else {
      // Vector 3D (subtract 1 space)
      OSD_G_PRINTF ",             " OSD_END

      // Raster 3D
      //OSD_G_PRINTF ",              " OSD_END
    }

    OSD_G_PRINTF " %#4lu MHz",
          gpu_stats.gpus [i].clocks_kHz.gpu / 1000UL
    OSD_END

    if (gpu_stats.gpus [i].volts_mV.supported)
    {
      // Over (or under) voltage limit!
      if (false)//gpu_stats.gpus [i].volts_mV.over)
      {
        OSD_G_PRINTF ", %#6.1fmV (%+#6.1fmV)",
          gpu_stats.gpus [i].volts_mV.core, gpu_stats.gpus [i].volts_mV.ov
        OSD_END
      } else {
        OSD_G_PRINTF ", %#6.1fmV",
          gpu_stats.gpus [i].volts_mV.core
        OSD_END
      }
    }

    if (gpu_stats.gpus [i].fans_rpm.supported)
    {
      OSD_G_PRINTF ", %#4lu RPM",
        gpu_stats.gpus [i].fans_rpm.gpu
      OSD_END
    }

    std::wstring temp = 
      BMF_FormatTemperature (
        gpu_stats.gpus [i].temps_c.gpu,
          Celsius,
            config.system.prefer_fahrenheit ? Fahrenheit :
                                              Celsius );
    OSD_G_PRINTF ", (%ws)",
      temp.c_str ()
    OSD_END

    if (config.sli.show) {
      if (afr_last == i)
        OSD_G_PRINTF "@" OSD_END

      if (afr_idx == i)
        OSD_G_PRINTF "!" OSD_END

      if (afr_next == i)
        OSD_G_PRINTF "#" OSD_END
    }

    if (nvapi_init &&
        config.gpu.print_slowdown &&
        gpu_stats.gpus [i].nv_perf_state != NV_GPU_PERF_DECREASE_NONE) {
      OSD_G_PRINTF "   SLOWDOWN:" OSD_END

      if (gpu_stats.gpus [i].nv_perf_state & NV_GPU_PERF_DECREASE_REASON_AC_BATT)
        OSD_G_PRINTF " (Battery)" OSD_END
      if (gpu_stats.gpus [i].nv_perf_state & NV_GPU_PERF_DECREASE_REASON_API_TRIGGERED)
        OSD_G_PRINTF " (Driver)" OSD_END
      if (gpu_stats.gpus [i].nv_perf_state & NV_GPU_PERF_DECREASE_REASON_INSUFFICIENT_POWER)
        OSD_G_PRINTF " (Power Supply)" OSD_END
      if (gpu_stats.gpus [i].nv_perf_state & NV_GPU_PERF_DECREASE_REASON_POWER_CONTROL)
        OSD_G_PRINTF " (Power Limit)" OSD_END
      if (gpu_stats.gpus [i].nv_perf_state & NV_GPU_PERF_DECREASE_REASON_THERMAL_PROTECTION)
        OSD_G_PRINTF " (Thermal Limit)" OSD_END
    }

    OSD_G_PRINTF "\n" OSD_END
  }

  //
  // DXGI 1.4 Memory Info (VERY accurate)
  ///
  if (nodes > 0) {
    // We need to be careful here, it's not guaranteed that NvAPI adapter indices
    //   match up with DXGI 1.4 node indices... Adapter LUID may shed some light
    //     on that in the future.
    for (int i = 0; i < nodes; i++) {
      if (nvapi_init) {
        OSD_G_PRINTF "  VRAM%i  : %#5llu MiB (%#3lu%%: %#5.01lf GiB/s)",
          i,
          mem_info [buffer].local    [i].CurrentUsage >> 20ULL,
                      gpu_stats.gpus [i].loads_percent.fb,
          (double)((uint64_t)gpu_stats.gpus [i].clocks_kHz.ram * 2ULL * 1000ULL *
                   (uint64_t)gpu_stats.gpus [i].hwinfo.mem_bus_width) / 8.0 /
                     (1024.0 * 1024.0 * 1024.0) *
                    ((double)gpu_stats.gpus [i].loads_percent.fb / 100.0)
        OSD_END
      } else {
        OSD_G_PRINTF "  VRAM%i  : %#5llu MiB",
          i, mem_info [buffer].local [i].CurrentUsage >> 20ULL
        OSD_END
      }

      OSD_G_PRINTF ", %#4lu MHz",
        gpu_stats.gpus [i].clocks_kHz.ram / 1000UL
      OSD_END

#if 0
      // Add memory temperature if it exists
      if (i <= gpu_stats.num_gpus &&
          gpu_stats.gpus [i].temps_c.ram != 0) {
        OSD_G_PRINTF " (%#3luC)",
          gpu_stats.gpus [i].temps_c.ram
        OSD_END
      }
#endif

      OSD_G_PRINTF "\n" OSD_END
    }

    for (int i = 0; i < nodes; i++) {
      // Figure out the generation from the transfer rate...
      int pcie_gen = gpu_stats.gpus [i].hwinfo.pcie_gen;

      if (nvapi_init) {
        OSD_G_PRINTF "  SHARE%i : %#5llu MiB (%#3lu%%: %#5.02lf GiB/s), PCIe %li.0x%lu\n",
          i,
           mem_info [buffer].nonlocal [i].CurrentUsage >> 20ULL,
                       gpu_stats.gpus [i].loads_percent.bus,
                       gpu_stats.gpus [i].hwinfo.pcie_bandwidth_mb () / 1024.0 *
              ((double)gpu_stats.gpus [i].loads_percent.bus / 100.0),
                       pcie_gen,
                       gpu_stats.gpus [i].hwinfo.pcie_lanes
                       //gpu_stats.gpus [i].hwinfo.pcie_transfer_rate
        OSD_END
      } else {
        OSD_G_PRINTF "  SHARE%i : %#5llu MiB, PCIe %li.0x%lu\n",
          i,
          mem_info [buffer].nonlocal [i].CurrentUsage >> 20ULL,
          pcie_gen,
          gpu_stats.gpus [i].hwinfo.pcie_lanes
        OSD_END
      }
    }
  }

  //
  // NvAPI or ADL Memory Info (Reasonably Accurate on Windows 8.1 and older)
  //
  else {
    // We need to be careful here, it's not guaranteed that NvAPI adapter indices
    //   match up with DXGI 1.4 node indices... Adapter LUID may shed some light
    //     on that in the future.
    for (int i = 0; i < gpu_stats.num_gpus; i++) {
      if (nvapi_init) {
        OSD_G_PRINTF "  VRAM%i  : %#5llu MiB (%#3lu%%: %#5.01lf GiB/s)",
          i,
                      gpu_stats.gpus [i].memory_B.local >> 20ULL,
                      gpu_stats.gpus [i].loads_percent.fb,
          (double)((uint64_t)gpu_stats.gpus [i].clocks_kHz.ram * 2ULL * 1000ULL *
                   (uint64_t)gpu_stats.gpus [i].hwinfo.mem_bus_width) / 8.0 /
                     (1024.0 * 1024.0 * 1024.0) *
                    ((double)gpu_stats.gpus [i].loads_percent.fb / 100.0)
        OSD_END
      } else {
        OSD_G_PRINTF "  VRAM%i  : %#5llu MiB",
          i, gpu_stats.gpus [i].memory_B.local >> 20ULL
        OSD_END
      }

      OSD_G_PRINTF ", %#4lu MHz",
        gpu_stats.gpus [i].clocks_kHz.ram / 1000UL
      OSD_END

      OSD_G_PRINTF "\n" OSD_END
    }

    for (int i = 0; i < gpu_stats.num_gpus; i++) {
      // Figure out the generation from the transfer rate...
      int pcie_gen = gpu_stats.gpus [i].hwinfo.pcie_gen;

      if (nvapi_init) {
        OSD_G_PRINTF "  SHARE%i : %#5llu MiB (%#3lu%%: %#5.02lf GiB/s), PCIe %li.0x%lu\n",
          i,
                       gpu_stats.gpus [i].memory_B.nonlocal >> 20ULL,
                       gpu_stats.gpus [i].loads_percent.bus,
                       gpu_stats.gpus [i].hwinfo.pcie_bandwidth_mb () / 1024.0 *
              ((double)gpu_stats.gpus [i].loads_percent.bus / 100.0),
                       pcie_gen,
                       gpu_stats.gpus [i].hwinfo.pcie_lanes
                       //gpu_stats.gpus [i].hwinfo.pcie_transfer_rate
        OSD_END
      } else {
        OSD_G_PRINTF "  SHARE%i : %#5llu MiB, PCIe %li.0x%lu\n",
          i,
          gpu_stats.gpus [i].memory_B.nonlocal    >> 20ULL,
          pcie_gen,
          gpu_stats.gpus [i].hwinfo.pcie_lanes
        OSD_END
      }

#if 0
      // Add memory temperature if it exists
      if (gpu_stats.gpus [i].temps_c.ram != 0) {
        OSD_G_PRINTF " (%#3luC)",
          gpu_stats.gpus [i].temps_c.ram
        OSD_END
      }
#endif
    }
  }

  //OSD_G_PRINTF "\n" OSD_END

  OSD_C_PRINTF "\n  Total  : %#3llu%%  -  (Kernel: %#3llu%%   "
                 "User: %#3llu%%   Interrupt: %#3llu%%)\n",
        cpu_stats.cpus [0].percent_load, 
          cpu_stats.cpus [0].percent_kernel, 
            cpu_stats.cpus [0].percent_user, 
              cpu_stats.cpus [0].percent_interrupt
  OSD_END

  for (DWORD i = 1; i < cpu_stats.num_cpus; i++) {
    if (! config.cpu.simple) {
      OSD_C_PRINTF "  CPU%lu   : %#3llu%%  -  (Kernel: %#3llu%%   "
                   "User: %#3llu%%   Interrupt: %#3llu%%)\n",
        i-1,
          cpu_stats.cpus [i].percent_load, 
            cpu_stats.cpus [i].percent_kernel, 
              cpu_stats.cpus [i].percent_user, 
                cpu_stats.cpus [i].percent_interrupt
      OSD_END
    } else {
      OSD_C_PRINTF "  CPU%lu   : %#3llu%%\n",
        i-1,
          cpu_stats.cpus [i].percent_load
      OSD_END
    }
  }

  // Only do this if the IO data view is active
  if (config.io.show)
    BMF_CountIO (io_counter, config.io.interval / 1.0e-7);

  OSD_I_PRINTF "\n  Read   :%#6.02f MiB/s - (%#6.01f IOP/s)"
               "\n  Write  :%#6.02f MiB/s - (%#6.01f IOP/s)"
               "\n  Other  :%#6.02f MiB/s - (%#6.01f IOP/s)\n",
               io_counter.read_mb_sec,  io_counter.read_iop_sec,
               io_counter.write_mb_sec, io_counter.write_iop_sec,
               io_counter.other_mb_sec, io_counter.other_iop_sec
  OSD_END

  if (nodes > 0) {
    int i = 0;

    OSD_M_PRINTF "\n"
                   "----- (DXGI 1.4): Local Memory -------"
                   "--------------------------------------\n"
    OSD_END

    while (i < nodes) {
      OSD_M_PRINTF "  %8s %i  (Reserve:  %#5llu / %#5llu MiB  - "
                   " Budget:  %#5llu / %#5llu MiB)",
                  nodes > 1 ? (nvapi_init ? "SLI Node" : "CFX Node") : "GPU",
                  i,
                  mem_info [buffer].local [i].CurrentReservation      >> 20ULL,
                  mem_info [buffer].local [i].AvailableForReservation >> 20ULL,
                  mem_info [buffer].local [i].CurrentUsage            >> 20ULL,
                  mem_info [buffer].local [i].Budget                  >> 20ULL
      OSD_END

      //
      // SLI Status Indicator
      //
      if (afr_last == i)
        OSD_S_PRINTF "@" OSD_END

      if (afr_idx == i)
        OSD_S_PRINTF "!" OSD_END

      if (afr_next == i)
        OSD_S_PRINTF "#" OSD_END

      OSD_M_PRINTF "\n" OSD_END

      i++;
    }

    i = 0;

    OSD_M_PRINTF "----- (DXGI 1.4): Non-Local Memory ---"
                 "--------------------------------------\n"
    OSD_END

    while (i < nodes) {
      if ((mem_info [buffer].nonlocal [i].CurrentUsage >> 20ULL) > 0) {
        OSD_M_PRINTF "  %8s %i  (Reserve:  %#5llu / %#5llu MiB  -  "
                     "Budget:  %#5llu / %#5llu MiB)\n",
                         nodes > 1 ? "SLI Node" : "GPU",
                         i,
                mem_info [buffer].nonlocal [i].CurrentReservation      >> 20ULL,
                mem_info [buffer].nonlocal [i].AvailableForReservation >> 20ULL,
                mem_info [buffer].nonlocal [i].CurrentUsage            >> 20ULL,
                mem_info [buffer].nonlocal [i].Budget                  >> 20ULL
        OSD_END
      }

      i++;
    }

    OSD_M_PRINTF "----- (DXGI 1.4): Miscellaneous ------"
                 "--------------------------------------\n"
    OSD_END

    int64_t headroom = mem_info [buffer].local [0].Budget -
                       mem_info [buffer].local [0].CurrentUsage;

    OSD_M_PRINTF "  Max. Resident Set:  %#5llu MiB  -"
                 "  Max. Over Budget:  %#5llu MiB\n"
                 "     Budget Changes:  %#5llu      - "
                  "      Budget Left:  %#5lli MiB\n",
                                    mem_stats [0].max_usage       >> 20ULL,
                                    mem_stats [0].max_over_budget >> 20ULL,
                                    mem_stats [0].budget_changes,
                                    headroom / 1024 / 1024
    OSD_END
  }

  OSD_M_PRINTF "\n" OSD_END

  std::wstring working_set =
    BMF_SizeToString (process_stats.memory.working_set,   MiB);
  std::wstring commit =
    BMF_SizeToString (process_stats.memory.private_bytes, MiB);
  std::wstring virtual_size = 
    BMF_SizeToString (process_stats.memory.virtual_bytes, MiB);

  OSD_M_PRINTF "  Working Set: %ws,  Committed: %ws,  Address Space: %ws\n",
    working_set.c_str  (),
    commit.c_str       (),
    virtual_size.c_str ()
  OSD_END

  std::wstring working_set_peak =
    BMF_SizeToString (process_stats.memory.working_set_peak,     MiB);
  std::wstring commit_peak =
    BMF_SizeToString (process_stats.memory.page_file_bytes_peak, MiB);
  std::wstring virtual_peak = 
    BMF_SizeToString (process_stats.memory.virtual_bytes_peak,   MiB);

  OSD_M_PRINTF "        *Peak: %ws,      *Peak: %ws,          *Peak: %ws\n",
    working_set_peak.c_str (),
    commit_peak.c_str      (),
    virtual_peak.c_str     ()
  OSD_END

  extern int gpu_prio;

  OSD_B_PRINTF "\n  GPU Priority: %+1i\n",
    gpu_prio
  OSD_END

#if 0
  bool use_mib_sec = disk_stats.num_disks > 0 ?
                       (disk_stats.disks [0].bytes_sec > (1024 * 1024 * 2)) : false;

  if (use_mib_sec) {
#endif
    for (DWORD i = 0; i < disk_stats.num_disks; i++) {
      std::wstring read_bytes_sec =
        BMF_SizeToStringF (disk_stats.disks [i].read_bytes_sec, 6, 1);

      std::wstring write_bytes_sec =
        BMF_SizeToStringF (disk_stats.disks [i].write_bytes_sec, 6, 1);

      if (i == 0) {
        OSD_D_PRINTF "\n  Disk %16s %#3llu%%  -  (Read %#3llu%%: %ws/s, "
                                                 "Write %#3llu%%: %ws/s)\n",
          disk_stats.disks [i].name,
            disk_stats.disks [i].percent_load,
              disk_stats.disks [i].percent_read,
                read_bytes_sec.c_str (),
                  disk_stats.disks [i].percent_write,
                    write_bytes_sec.c_str ()
        OSD_END
      }
      else {
        OSD_D_PRINTF "  Disk %-16s %#3llu%%  -  (Read %#3llu%%: %ws/s, "
                                                "Write %#3llu%%: %ws/s)\n",
          disk_stats.disks [i].name,
            disk_stats.disks [i].percent_load,
              disk_stats.disks [i].percent_read,
                read_bytes_sec.c_str (),
                  disk_stats.disks [i].percent_write,
                    write_bytes_sec.c_str ()
        OSD_END
      }
    }
#if 0
  }
  else
  {
    for (int i = 0; i < disk_stats.num_disks; i++) {
      OSD_D_PRINTF "\n  Disk %16s %#3llu%%  -  (Read: %#3llu%%   Write: %#3llu%%) - "
                                        "(Read: %#5.01f KiB   Write: %#5.01f KiB)",
        disk_stats.disks[i].name,
          disk_stats.disks[i].percent_load,
            disk_stats.disks[i].percent_read,
              disk_stats.disks[i].percent_write,
                (float)disk_stats.disks[i].read_bytes_sec / (1024.0f),
                (float)disk_stats.disks[i].write_bytes_sec / (1024.0f)
      OSD_END

      if (i == 0)
        OSD_D_PRINTF "\n" OSD_END
    }
  }
#endif

  for (DWORD i = 0; i < pagefile_stats.num_pagefiles; i++) {
      std::wstring usage =
        BMF_SizeToStringF (pagefile_stats.pagefiles [i].usage, 5,2);
      std::wstring size = 
        BMF_SizeToStringF (pagefile_stats.pagefiles [i].size, 5,2);
      std::wstring peak =
        BMF_SizeToStringF (pagefile_stats.pagefiles [i].usage_peak, 5,2);

      OSD_P_PRINTF "\n  Pagefile %20s  %ws / %ws  (Peak: %ws)",
        pagefile_stats.pagefiles [i].name,
          usage.c_str    (),
            size.c_str   (),
              peak.c_str ()
      OSD_END
  }

  OSD_P_PRINTF "\n" OSD_END

  BOOL ret = BMF_UpdateOSD (szOSD, pMemory);

  BMF_ReleaseSharedMemory (pMemory);

  return ret;
}

BOOL
BMF_UpdateOSD (LPCSTR lpText, LPVOID pMapAddr, LPCSTR lpAppName)
{
  if (lpAppName == nullptr)
    lpAppName = "Batman Tweak";

  //BMF_AutoCriticalSection auto_cs (&osd_cs);

  static DWORD dwProcID =
    GetCurrentProcessId ();

  BOOL bResult = FALSE;

  if (osd_shutting_down && osd_init == false) {
    return bResult;
  }

  // If pMapAddr == nullptr, manage memory ourselves
  bool own_memory = false;
  if (pMapAddr == nullptr) {
    // When shutting down, don't care if RivaTuner knows about our process
    //   anymore ... we simply want the text gone!
    if (osd_shutting_down) {
      pMapAddr = BMF_GetSharedMemory (0);
      osd_init = false;
      lpText = "\0";
    }
    else
      pMapAddr = BMF_GetSharedMemory (dwProcID);
    own_memory = true;
  }

  LPRTSS_SHARED_MEMORY pMem     =
    (LPRTSS_SHARED_MEMORY)pMapAddr;

  if (pMem)
  {
    for (DWORD dwPass = 0; dwPass < 2; dwPass++)
    {
      //1st Pass: Find previously captured OSD slot
      //2nd Pass: Otherwise find the first unused OSD slot and capture it

      for (DWORD dwEntry = 1; dwEntry < pMem->dwOSDArrSize; dwEntry++)
      {
        // Allow primary OSD clients (i.e. EVGA Precision / MSI Afterburner)
        //   to use the first slot exclusively, so third party applications
        //     start scanning the slots from the second one

        RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_OSD_ENTRY pEntry =
          (RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_OSD_ENTRY)
            ((LPBYTE)pMem + pMem->dwOSDArrOffset +
                  dwEntry * pMem->dwOSDEntrySize);

        if (dwPass)
        {
          if (! strlen (pEntry->szOSDOwner))
            strcpy (pEntry->szOSDOwner, lpAppName);
        }

        if (! strcmp (pEntry->szOSDOwner, lpAppName))
        {
          if (pMem->dwVersion >= 0x00020007)
            //Use extended text slot for v2.7 and higher shared memory,
            // it allows displaying 4096 symbols instead of 256 for regular
            //  text slot
            strncpy (pEntry->szOSDEx, lpText, sizeof pEntry->szOSDEx - 1);
            //snprintf (pEntry->szOSDEx, sizeof pEntry->szOSDEx - 1, "Frame: %d\n%s", pMem->dwOSDFrame, lpText);
          else
            strncpy (pEntry->szOSD,   lpText, sizeof pEntry->szOSD   - 1);

          pMem->dwOSDFrame++;

          bResult = TRUE;
          break;
        }
      }

      if (bResult)
        break;
    }
  }

  if (own_memory)
    BMF_ReleaseSharedMemory (pMapAddr);

  return bResult;
}

void
BMF_ReleaseOSD (void)
{
  //BMF_AutoCriticalSection auto_cs (&osd_cs);

  osd_shutting_down = true;
  BMF_UpdateOSD ("");
}


void
BMF_SetOSDPos (int x, int y)
{
  //BMF_AutoCriticalSection auto_cs (&osd_cs);

  // 0,0 means don't touch anything.
  if (x == 0 && y == 0)
    return;

  LPVOID pMapAddr =
    BMF_GetSharedMemory ();

  LPRTSS_SHARED_MEMORY pMem     =
    (LPRTSS_SHARED_MEMORY)pMapAddr;

  if (pMem)
  {
    for (DWORD dwEntry = 1; dwEntry < pMem->dwOSDArrSize; dwEntry++)
    {
      RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_OSD_ENTRY pEntry =
        (RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_OSD_ENTRY)
        ((LPBYTE)pMem + pMem->dwOSDArrOffset +
          dwEntry * pMem->dwOSDEntrySize);

      if (! strcmp (pEntry->szOSDOwner, "Batman Fix"))
      {
        for (DWORD dwApp = 0; dwApp < pMem->dwAppArrSize; dwApp++)
        {
          RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_APP_ENTRY pApp =
            (RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_APP_ENTRY)
            ((LPBYTE)pMem + pMem->dwAppArrOffset +
              dwApp * pMem->dwAppEntrySize);

          if (pApp->dwProcessID == GetCurrentProcessId ())
          {
            config.osd.pos_x = x;
            config.osd.pos_y = y;

            pApp->dwOSDX = x;
            pApp->dwOSDX = y;

            pApp->dwFlags |= OSDFLAG_UPDATED;
            break;
          }
        }
        break;
      }
    }
  }
  BMF_ReleaseSharedMemory (pMapAddr);
}

void
BMF_SetOSDColor (int red, int green, int blue)
{
  //BMF_AutoCriticalSection auto_cs (&osd_cs);

  LPVOID pMapAddr =
    BMF_GetSharedMemory ();

  LPRTSS_SHARED_MEMORY pMem     =
    (LPRTSS_SHARED_MEMORY)pMapAddr;

  if (pMem)
  {
    for (DWORD dwEntry = 1; dwEntry < pMem->dwOSDArrSize; dwEntry++)
    {
      RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_OSD_ENTRY pEntry =
        (RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_OSD_ENTRY)
        ((LPBYTE)pMem + pMem->dwOSDArrOffset +
          dwEntry * pMem->dwOSDEntrySize);

      if (! strcmp (pEntry->szOSDOwner, "Batman Fix"))
      {
        for (DWORD dwApp = 0; dwApp < pMem->dwAppArrSize; dwApp++)
        {
          RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_APP_ENTRY pApp =
            (RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_APP_ENTRY)
            ((LPBYTE)pMem + pMem->dwAppArrOffset +
              dwApp * pMem->dwAppEntrySize);

          if (pApp->dwProcessID == GetCurrentProcessId ())
          {
            int red_   = (pApp->dwOSDColor >> 16) & 0xFF;
            int green_ = (pApp->dwOSDColor >>  8) & 0xFF;
            int blue_  = (pApp->dwOSDColor      ) & 0xFF;

            if (red >= 0 && red <= 255) {
              config.osd.red = red;
              red_ = red;
            }

            if (green >= 0 && green <= 255) {
              config.osd.green = green;
              green_ = green;
            }

            if (blue >= 0 && blue <= 255) {
              config.osd.blue = blue;
              blue_ = blue;
            }

            pApp->dwOSDColor = ((red_ << 16) & 0xff0000) | ((green_ << 8) & 0xff00) | (blue_ & 0xff);

            pApp->dwFlags |= OSDFLAG_UPDATED;
            break;
          }
        }
        break;
      }
    }
  }
  BMF_ReleaseSharedMemory (pMapAddr);
}

void
BMF_SetOSDScale (DWORD dwScale, bool relative)
{
  //BMF_AutoCriticalSection auto_cs (&osd_cs);

  LPVOID pMapAddr =
    BMF_GetSharedMemory ();

  LPRTSS_SHARED_MEMORY pMem     =
    (LPRTSS_SHARED_MEMORY)pMapAddr;

  if (pMem)
  {
    for (DWORD dwEntry = 1; dwEntry < pMem->dwOSDArrSize; dwEntry++)
    {
      RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_OSD_ENTRY pEntry =
        (RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_OSD_ENTRY)
        ((LPBYTE)pMem + pMem->dwOSDArrOffset +
          dwEntry * pMem->dwOSDEntrySize);

      if (! strcmp (pEntry->szOSDOwner, "Batman Fix"))
      {
        for (DWORD dwApp = 0; dwApp < pMem->dwAppArrSize; dwApp++)
        {
          RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_APP_ENTRY pApp =
            (RTSS_SHARED_MEMORY::LPRTSS_SHARED_MEMORY_APP_ENTRY)
            ((LPBYTE)pMem + pMem->dwAppArrOffset +
              dwApp * pMem->dwAppEntrySize);

          if (pApp->dwProcessID == GetCurrentProcessId ())
          {
            if (! relative)
              pApp->dwOSDPixel = dwScale;

             else
               pApp->dwOSDPixel += dwScale;

            // Clamp to a sane range :)
            if (pApp->dwOSDPixel < 1)
              pApp->dwOSDPixel = 1;

            config.osd.scale = pApp->dwOSDPixel;

            pApp->dwFlags |= OSDFLAG_UPDATED;
            break;
          }
        }
        break;
      }
    }
  }
  BMF_ReleaseSharedMemory (pMapAddr);
}

void
BMF_ResizeOSD (int scale_incr)
{
  //BMF_AutoCriticalSection auto_cs (&osd_cs);

  BMF_SetOSDScale (scale_incr, true);
}

