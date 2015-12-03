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
#include "resource.h"

#include "core.h"

#include "config.h"
#include "log.h"

// PlaySound
#pragma comment (lib, "winmm.lib")

bmf_logger_t steam_log;
HANDLE       hSteamHeap = NULL;

static bool init = false;

// We're not going to use DLL Import - we will load these function pointers
//  by hand.
#define STEAM_API_NODLL
#include "steam_api.h"


// To spoof Overlay Activation (pause the game)
#include <set>
std::set <class CCallbackBase *> overlay_activation_callbacks;
CRITICAL_SECTION callback_cs = { 0 };


S_API typedef bool (S_CALLTYPE *SteamAPI_Init_t    )(void);
S_API typedef bool (S_CALLTYPE *SteamAPI_InitSafe_t)(void);

S_API typedef bool (S_CALLTYPE *SteamAPI_RestartAppIfNecessary_t)( uint32 unOwnAppID );
S_API typedef bool (S_CALLTYPE *SteamAPI_IsSteamRunning_t)(void);

S_API typedef void (S_CALLTYPE *SteamAPI_Shutdown_t)(void);

S_API typedef void (S_CALLTYPE *SteamAPI_RegisterCallback_t)
          (class CCallbackBase *pCallback, int iCallback);
S_API typedef void (S_CALLTYPE *SteamAPI_UnregisterCallback_t)
          (class CCallbackBase *pCallback);
S_API typedef void (S_CALLTYPE *SteamAPI_RunCallbacks_t)(void);

S_API typedef HSteamUser (*SteamAPI_GetHSteamUser_t)(void);
S_API typedef HSteamPipe (*SteamAPI_GetHSteamPipe_t)(void);

S_API typedef ISteamClient*    (S_CALLTYPE *SteamClient_t   )(void);

S_API SteamAPI_RunCallbacks_t       SteamAPI_RunCallbacks       = nullptr;
S_API SteamAPI_RegisterCallback_t   SteamAPI_RegisterCallback   = nullptr;
S_API SteamAPI_UnregisterCallback_t SteamAPI_UnregisterCallback = nullptr;

S_API SteamAPI_Init_t               SteamAPI_Init               = nullptr;
S_API SteamAPI_InitSafe_t           SteamAPI_InitSafe           = nullptr;
S_API SteamAPI_Shutdown_t           SteamAPI_Shutdown           = nullptr;

S_API SteamAPI_RestartAppIfNecessary_t SteamAPI_RestartAppIfNecessary=nullptr;
S_API SteamAPI_IsSteamRunning_t        SteamAPI_IsSteamRunning       =nullptr;

S_API SteamAPI_GetHSteamUser_t      SteamAPI_GetHSteamUser      = nullptr;
S_API SteamAPI_GetHSteamPipe_t      SteamAPI_GetHSteamPipe      = nullptr;

S_API SteamClient_t                 SteamClient                 = nullptr;


S_API SteamAPI_RegisterCallback_t   SteamAPI_RegisterCallback_Original   = nullptr;
S_API SteamAPI_UnregisterCallback_t SteamAPI_UnregisterCallback_Original = nullptr;

S_API
void
S_CALLTYPE
SteamAPI_RegisterCallback_Detour (class CCallbackBase *pCallback, int iCallback)
{
  EnterCriticalSection (&callback_cs);

  switch (iCallback)
  {
    case GameOverlayActivated_t::k_iCallback:
      steam_log.Log (L" * Game Installed Overlay Activation Callback");
      overlay_activation_callbacks.insert (pCallback);
      break;
    case ScreenshotRequested_t::k_iCallback:
      steam_log.Log (L" * Game Installed Screenshot Callback");
      break;
    default:
      steam_log.Log ( L" * Game Installed Unknown Callback (Class=%lu00, Id=%lu)",
                        iCallback / 100, iCallback % 100 );
      break;
  }

  SteamAPI_RegisterCallback_Original (pCallback, iCallback);

  LeaveCriticalSection (&callback_cs);
}

S_API
void
S_CALLTYPE
SteamAPI_UnregisterCallback_Detour (class CCallbackBase *pCallback)
{
  EnterCriticalSection (&callback_cs);

  switch (pCallback->GetICallback ())
  {
    case GameOverlayActivated_t::k_iCallback:
      steam_log.Log (L" * Game Uninstalled Overlay Activation Callback");
      overlay_activation_callbacks.erase (pCallback);
      break;
    case ScreenshotRequested_t::k_iCallback:
      steam_log.Log (L" * Game Uninstalled Screenshot Callback");
      break;
  }

  SteamAPI_UnregisterCallback_Original (pCallback);

  LeaveCriticalSection (&callback_cs);
}


extern "C" void __cdecl SteamAPIDebugTextHook (int nSeverity, const char *pchDebugText);

class BMF_SteamAPIContext {
public:
  bool Init (HMODULE hSteamDLL)
  {
    wchar_t wszSteamDLLName [MAX_PATH];
    GetModuleFileNameW (hSteamDLL, wszSteamDLLName, MAX_PATH);

    if (hSteamHeap == nullptr)
      hSteamHeap = HeapCreate (HEAP_CREATE_ENABLE_EXECUTE, 0, 0);

    if (SteamAPI_InitSafe == nullptr) {
      SteamAPI_InitSafe =
        (SteamAPI_InitSafe_t)GetProcAddress (
          hSteamDLL,
            "SteamAPI_InitSafe"
        );
    }

    if (SteamAPI_GetHSteamUser == nullptr) {
      SteamAPI_GetHSteamUser =
        (SteamAPI_GetHSteamUser_t)GetProcAddress (
           hSteamDLL,
             "SteamAPI_GetHSteamUser"
        );
    }

    if (SteamAPI_GetHSteamPipe == nullptr) {
      SteamAPI_GetHSteamPipe =
        (SteamAPI_GetHSteamPipe_t)GetProcAddress (
           hSteamDLL,
             "SteamAPI_GetHSteamPipe"
        );
    }

    SteamAPI_IsSteamRunning =
      (SteamAPI_IsSteamRunning_t)GetProcAddress (
         hSteamDLL,
           "SteamAPI_IsSteamRunning"
      );

    if (SteamClient == nullptr) {
      SteamClient =
        (SteamClient_t)GetProcAddress (
           hSteamDLL,
             "SteamClient"
        );
    }

    if (SteamAPI_RegisterCallback == nullptr) {
#if 0
      SteamAPI_RegisterCallback =
        (SteamAPI_RegisterCallback_t)GetProcAddress (
          hSteamDLL,
            "SteamAPI_RegisterCallback"
        );
#else
      BMF_CreateDLLHook (wszSteamDLLName, "SteamAPI_RegisterCallback",
                           SteamAPI_RegisterCallback_Detour,
                (LPVOID *)&SteamAPI_RegisterCallback_Original,
                (LPVOID *)&SteamAPI_RegisterCallback);
      BMF_EnableHook (SteamAPI_RegisterCallback);
#endif
    }

    if (SteamAPI_UnregisterCallback == nullptr) {
#if 0
      SteamAPI_UnregisterCallback =
        (SteamAPI_UnregisterCallback_t)GetProcAddress (
          hSteamDLL,
            "SteamAPI_UnregisterCallback"
        );
#else
      BMF_CreateDLLHook (wszSteamDLLName, "SteamAPI_UnregisterCallback",
                           SteamAPI_UnregisterCallback_Detour,
                (LPVOID *)&SteamAPI_UnregisterCallback_Original,
                (LPVOID *)&SteamAPI_UnregisterCallback);
      BMF_EnableHook (SteamAPI_UnregisterCallback);
#endif
    }

    if (SteamAPI_RunCallbacks == nullptr) {
      SteamAPI_RunCallbacks =
        (SteamAPI_RunCallbacks_t)GetProcAddress (
          hSteamDLL,
            "SteamAPI_RunCallbacks"
        );
    }

    if (SteamAPI_Shutdown == nullptr) {
      SteamAPI_Shutdown =
        (SteamAPI_Shutdown_t)GetProcAddress (
          hSteamDLL,
            "SteamAPI_Shutdown"
        );
    }

    if (SteamAPI_InitSafe == nullptr)
      return false;

    if (SteamAPI_GetHSteamUser == nullptr)
      return false;

    if (SteamAPI_GetHSteamPipe == nullptr)
      return false;

    if (SteamClient == nullptr)
      return false;

    if (SteamAPI_RegisterCallback == nullptr)
      return false;

    if (SteamAPI_UnregisterCallback == nullptr)
      return false;

    if (SteamAPI_RunCallbacks == nullptr)
      return false;

    if (! SteamAPI_InitSafe ())
      return false;

    client_ = SteamClient ();

    HSteamUser hSteamUser = SteamAPI_GetHSteamUser ();
    HSteamPipe hSteamPipe = SteamAPI_GetHSteamPipe ();

    if (! client_)
      return false;

    user_stats_ =
      client_->GetISteamUserStats (
        hSteamUser,
          hSteamPipe,
            STEAMUSERSTATS_INTERFACE_VERSION
      );

    if (user_stats_ == nullptr) {
      steam_log.Log ( L" >> ISteamUserStats NOT FOUND for version %hs <<",
                        STEAMUSERSTATS_INTERFACE_VERSION );
      return false;
    }

    utils_ =
      client_->GetISteamUtils ( hSteamPipe,
                                  STEAMUTILS_INTERFACE_VERSION );

    if (utils_ == nullptr) {
      steam_log.Log ( L" >> ISteamUtils NOT FOUND for version %hs <<",
                        STEAMUTILS_INTERFACE_VERSION );
      return false;
    }

    screenshots_ =
      client_->GetISteamScreenshots ( hSteamUser,
                                        hSteamPipe,
                                          STEAMSCREENSHOTS_INTERFACE_VERSION );

    if (screenshots_ == nullptr) {
      steam_log.Log ( L" >> ISteamScreenshots NOT FOUND for version %hs <<",
                        STEAMSCREENSHOTS_INTERFACE_VERSION );
      return false;
    }

    steam_log.LogEx (true, L" # Installing Steam API Debug Text Callback... ");
    SteamClient ()->SetWarningMessageHook (&SteamAPIDebugTextHook);
    steam_log.LogEx (false, L"SteamAPIDebugTextHook\n\n");

    // 4 == Don't Care
    if (config.steam.notify_corner != 4)
      utils_->SetOverlayNotificationPosition (
        (ENotificationPosition)config.steam.notify_corner
      );

    if (config.steam.inset_x != 0 ||
        config.steam.inset_y != 0) {
      utils_->SetOverlayNotificationInset (config.steam.inset_x,
                                           config.steam.inset_y);
    }

    return true;
  }

  void Shutdown (void) {


    if (hSteamHeap != nullptr) {
      HeapDestroy (hSteamHeap);
      hSteamHeap = nullptr;
    }

    client_      = nullptr;
    user_stats_  = nullptr;
    utils_       = nullptr;
    screenshots_ = nullptr;

    if (SteamAPI_Shutdown != nullptr) {
      // We probably should not shutdown Steam API; the underlying
      //  game will do this at a more opportune time for sure.
      SteamAPI_Shutdown ();
    }
  }

  ISteamUserStats*   UserStats   (void) { return user_stats_;  }
  ISteamUtils*       Utils       (void) { return utils_;       }
  ISteamScreenshots* Screenshots (void) { return screenshots_; }

protected:
private:
  // TODO: We have an obvious lack of thread-safety here...

  ISteamClient*      client_      = nullptr;
  ISteamUserStats*   user_stats_  = nullptr;
  ISteamUtils*       utils_       = nullptr;
  ISteamScreenshots* screenshots_ = nullptr;
} steam_ctx;

#if 0
struct BaseStats_t
{
  uint64  m_nGameID;
  EResult status;
  uint64  test;
};

S_API typedef void (S_CALLTYPE *steam_callback_run_t)
                       (CCallbackBase *pThis, void *pvParam);

S_API typedef void (S_CALLTYPE *steam_callback_run_ex_t)
                       (CCallbackBase *pThis, void *pvParam, bool, SteamAPICall_t);

steam_callback_run_t    Steam_Callback_RunStat_Orig   = nullptr;
steam_callback_run_ex_t Steam_Callback_RunStatEx_Orig = nullptr;

S_API
void
S_CALLTYPE
Steam_Callback_RunStat (CCallbackBase *pThis, void *pvParam)
{
  steam_log.Log ( L"CCallback::Run (%04Xh, %04Xh)  <Stat %s>;",
                    pThis, pvParam, pThis->GetICallback () == 1002L ? L"Store" : L"Receive" );

  BaseStats_t* stats = (BaseStats_t *)pvParam;

  steam_log.Log ( L" >> Size: %04i, Event: %04i - %04lu, %04llu, %04llu\n", pThis->GetCallbackSizeBytes (), pThis->GetICallback (), stats->status, stats->m_nGameID, stats->test);

  Steam_Callback_RunStat_Orig (pThis, pvParam);
}

S_API
void
S_CALLTYPE
Steam_Callback_RunStatEx (CCallbackBase *pThis, void           *pvParam,
                          bool           tf,    SteamAPICall_t  call)
{
  steam_log.Log ( L"CCallback::Run (%04Xh, %04Xh, %01i, %04llu)  "
                  L"<Stat %s>;",
                    pThis, pvParam, tf, call, pThis->GetICallback () == 1002L ? L"Store" : L"Receive" );

  BaseStats_t* stats = (BaseStats_t *)pvParam;

  steam_log.Log ( L" >> Size: %04i, Event: %04i - %04lu, %04llu, %04llu\n", pThis->GetCallbackSizeBytes (), pThis->GetICallback (), stats->status, stats->m_nGameID, stats->test);

  Steam_Callback_RunStatEx_Orig (pThis, pvParam, tf, call);
}
#endif

class BMF_Steam_AchievementManager {
public:
  BMF_Steam_AchievementManager (const wchar_t* wszUnlockSound) :
       unlock_listener ( this, &BMF_Steam_AchievementManager::OnUnlock      ),
       stat_listener   ( this, &BMF_Steam_AchievementManager::OnRecvStats   ),
       stat_receipt    ( this, &BMF_Steam_AchievementManager::AckStoreStats )
  {
    FILE* fWAV = _wfopen (wszUnlockSound, L"rb");

    if (fWAV != nullptr) {
      steam_log.LogEx (true, L"  >> Loading Achievement Unlock Sound: '%s'...",
                       wszUnlockSound);

                  fseek  (fWAV, 0, SEEK_END);
      long size = ftell  (fWAV);
                  rewind (fWAV);

      unlock_sound = (uint8_t *)HeapAlloc (hSteamHeap, HEAP_ZERO_MEMORY, size);

      if (unlock_sound != nullptr)
        fread  (unlock_sound, size, 1, fWAV);

      fclose (fWAV);

      steam_log.LogEx (false, L" %d bytes\n", size);

      default_loaded = false;
    } else {
      steam_log.Log (L"  * Failed to Load Unlock Sound: '%s', using DEFAULT",
                       wszUnlockSound);

      extern HMODULE hModSelf;
      HRSRC   default_sound =
        FindResource (hModSelf, MAKEINTRESOURCE (IDR_WAVE1), L"WAVE");

      if (default_sound != nullptr) {
        HGLOBAL sound_ref     =
          LoadResource (hModSelf, default_sound);
        if (sound_ref != 0) {
          unlock_sound        = (uint8_t *)LockResource (sound_ref);

          default_loaded = true;
        }
      }
    }
  }

  ~BMF_Steam_AchievementManager (void) {
    if ((! default_loaded) && (unlock_sound != nullptr)) {
      HeapFree (hSteamHeap, 0, unlock_sound);
      unlock_sound = nullptr;
    }
  }

  class BMF_SteamAchievement {
  public:
    BMF_SteamAchievement (const char* szName, ISteamUserStats* stats) {
      name_ =
        stats->GetAchievementDisplayAttribute (szName, "name");
      desc_ =
        stats->GetAchievementDisplayAttribute (szName, "desc");

      stats->GetAchievementAndUnlockTime ( szName,
                                             &unlocked_,
                                               (uint32_t *)&time_ );
    }

    const char* name_;
    const char* desc_;

    bool        unlocked_;
    __time32_t  time_;
  };

  void log_all_achievements (void)
  {
    ISteamUserStats* stats = steam_ctx.UserStats ();

    for (uint32 i = 0; i < stats->GetNumAchievements (); i++)
    {
      BMF_SteamAchievement achievement (
        stats->GetAchievementName (i),
        stats
      );

      steam_log.LogEx (false, L"\n [%c] Achievement %03lu......: '%hs'\n",
                         achievement.unlocked_ ? L'X' : L' ',
                           i, stats->GetAchievementName (i)
                      );
      steam_log.LogEx (false,
                              L"  + Human Readable Name...: %hs\n",
                         achievement.name_);
      if (strlen (achievement.desc_))
        steam_log.LogEx (false,
                                L"  *- Detailed Description.: %hs\n",
                          achievement.desc_);

      if (achievement.unlocked_) {
        steam_log.LogEx (false,
                                L"  @-- Player Unlocked At..: %s",
                                  _wctime32 (&achievement.time_));
      }
    }

    steam_log.LogEx (false, L"\n");
  }

  STEAM_CALLBACK ( BMF_Steam_AchievementManager,
                   AckStoreStats,
                   UserStatsStored_t,
                   stat_receipt )
  {
    // Sometimes we receive event callbacks for games that aren't this one...
    //   ignore those!
    if (pParam->m_nGameID != BMF::SteamAPI::AppID ())
      return;

    steam_log.Log ( L" >> Stats Stored for AppID: %llu",
                      pParam->m_nGameID );
  }

  STEAM_CALLBACK ( BMF_Steam_AchievementManager,
                   OnRecvStats,
                   UserStatsReceived_t,
                   stat_listener )
  {
    // Sometimes we receive event callbacks for games that aren't this one...
    //   ignore those!
    if (pParam->m_nGameID != BMF::SteamAPI::AppID ())
      return;

    log_all_achievements ();
  }

  STEAM_CALLBACK ( BMF_Steam_AchievementManager,
                   OnUnlock,
                   UserAchievementStored_t,
                   unlock_listener )
  {
    // Sometimes we receive event callbacks for games that aren't this one...
    //   ignore those!
    if (pParam->m_nGameID != BMF::SteamAPI::AppID ())
      return;

    BMF_SteamAchievement achievement (
      pParam->m_rgchAchievementName,
        steam_ctx.UserStats ()
    );

    if (pParam->m_nMaxProgress == 0 &&
        pParam->m_nCurProgress == 0) {
      if (! config.steam.nosound)
        PlaySound ( (LPCWSTR)unlock_sound, NULL, SND_ASYNC | SND_MEMORY );

      steam_log.Log (L" Achievement: '%hs' (%hs) - Unlocked!",
        achievement.name_, achievement.desc_);

      if (config.steam.achievement_sshot) {
        ISteamScreenshots* pScreenshots = steam_ctx.Screenshots ();

        if (pScreenshots != nullptr) {
          steam_log.LogEx (true, L"  >> Triggering Screenshot: ");
          pScreenshots->TriggerScreenshot ();
          steam_log.LogEx (false, L"done!\n");
        }
      }
    }

    else {
      steam_log.Log (L" Achievement: '%hs' (%hs) - "
                     L"Progress %lu / %lu (%04.01f%%)",
                achievement.name_,
                achievement.desc_,
                      pParam->m_nCurProgress,
                      pParam->m_nMaxProgress,
     100.0f * ((float)pParam->m_nCurProgress / (float)pParam->m_nMaxProgress));
    }
  }

  void* operator new (size_t size) {
    return HeapAlloc (hSteamHeap, HEAP_ZERO_MEMORY, size);
  }

  void operator delete (void* pMemory) {
    HeapFree (hSteamHeap, 0, pMemory);
  }

protected:
private:
  bool     default_loaded;
  uint8_t* unlock_sound;   // A .WAV (PCM) file
} *steam_achievements = nullptr;

#if 0
S_API typedef void (S_CALLTYPE *steam_unregister_callback_t)
                       (class CCallbackBase *pCallback);
S_API typedef void (S_CALLTYPE *steam_register_callback_t)
                       (class CCallbackBase *pCallback, int iCallback);

steam_register_callback_t SteamAPI_RegisterCallbackOrig = nullptr;


S_API bool S_CALLTYPE SteamAPI_Init_Detour (void);
#endif

void
BMF_UnlockSteamAchievement (int idx)
{
  //
  // If we got this far without initialization, something's weird - but
  //   we CAN recover.
  //
  // * Lazy loading steam_api*.dll is supported though usually doesn't work.
  //
  if (! init)
    BMF::SteamAPI::Init (false);

  steam_log.LogEx (true, L" >> Attempting to Unlock Achievement: %i... ",
    idx );

  ISteamUserStats* stats = steam_ctx.UserStats ();

  if (stats) {
    // I am dubious about querying these things by name, so duplicate this
    //   string immediately.
    const char* szName = _strdup (stats->GetAchievementName (idx));

    if (szName != nullptr) {
      steam_log.LogEx (false, L" (%hs - Found)\n", szName);

      UserAchievementStored_t store;
      store.m_nCurProgress = 0;
      store.m_nMaxProgress = 0;
      store.m_nGameID = BMF::SteamAPI::AppID ();
      strncpy (store.m_rgchAchievementName, szName, 128);

      steam_achievements->OnUnlock (&store);

//      stats->ClearAchievement            (szName);
//      stats->IndicateAchievementProgress (szName, 0, 1);
//      stats->StoreStats                  ();
#if 0
      bool achieved;
      if (stats->GetAchievement (szName, &achieved)) {
        if (achieved) {
          steam_log.LogEx (true, L"Clearing first\n");
          stats->ClearAchievement            (szName);
          stats->StoreStats                  ();

          SteamAPI_RunCallbacks              ();
        } else {
          steam_log.LogEx (true, L"Truly unlocking\n");
          stats->SetAchievement              (szName);

          stats->StoreStats                  ();

          // Dispatch these ASAP, there's a lot of latency apparently...
          SteamAPI_RunCallbacks ();
        }
      }
      else {
        steam_log.LogEx (true, L" >> GetAchievement (...) Failed\n");
      }
#endif
      free ((void *)szName);
    }
    else {
      steam_log.LogEx (false, L" (None Found)\n");
    }
  } else {
    steam_log.LogEx (false, L" (ISteamUserStats is NULL?!)\n");
  }
}

#define STEAMAPI_CALL1(x,y,z) ((x) = SteamAPI_##y z)
#define STEAMAPI_CALL0(y,z)   (SteamAPI_##y z)

#include <Windows.h>

extern "C"
void
__cdecl
SteamAPIDebugTextHook (int nSeverity, const char *pchDebugText)
{
  steam_log.Log (" [SteamAPI] Severity: %d - '%hs'",
    nSeverity, pchDebugText);
}

// Fancy name, for something that barely does anything ...
//   most init is done in the BMF_SteamAPIContext singleton.
bool
BMF_Load_SteamAPI_Imports (HMODULE hDLL, bool pre_load)
{
  SteamAPI_Init =
    (SteamAPI_Init_t)GetProcAddress (
      hDLL,
      "SteamAPI_Init"
    );

  SteamAPI_InitSafe =
    (SteamAPI_InitSafe_t)GetProcAddress (
       hDLL,
         "SteamAPI_InitSafe"
    );

  SteamAPI_RestartAppIfNecessary =
    (SteamAPI_RestartAppIfNecessary_t)GetProcAddress (
      hDLL,
        "SteamAPI_RestartAppIfNecessary"
    );

  if (SteamAPI_InitSafe != nullptr) {
    int appid;

    FILE* steam_appid = fopen ("steam_appid.txt", "r+");
    if (steam_appid == nullptr) {
      steam_log.LogEx (true, L"  * No steam_appid.txt in the CWD... ");
      if (config.steam.appid != 0 &&
          config.steam.appid != 1) {
        steam_appid = fopen ("steam_appid.txt", "w+");
        fprintf (steam_appid, "%d", config.steam.appid);
        fclose (steam_appid);
        steam_log.LogEx (false, L"wrote one for %d!\n\n",
                           config.steam.appid);
        return BMF_Load_SteamAPI_Imports (hDLL, pre_load);
      } else {
        steam_log.LogEx (false, L"\n");
      }

      // If we are pre-loading the DLL, we cannot recover from this...
      //if (pre_load)
        //return false;
    } else {
      fscanf (steam_appid, "%d", &appid);
      fclose (steam_appid);

      config.steam.appid = appid;
    }

    bool ret = false;

    if (config.steam.appid != 0) {
      steam_log.LogEx (true, L"Starting Steam App (%lu)... ", config.steam.appid);

      STEAMAPI_CALL1 (ret, RestartAppIfNecessary, (config.steam.appid));

      if (ret) {
        steam_log.LogEx (false, L"Steam will Restart!\n");
        exit (0);
        return false;
      }
      else {
        steam_log.LogEx (false, L"Success!\n");
      }
    }

#if 0
    steam_log.LogEx (true, L" [!] SteamAPI_InitSafe ()... ");

    STEAMAPI_CALL1 (ret, InitSafe, ());
#else
    steam_log.LogEx (true, L" [!] SteamAPI_Init ()... ");

    STEAMAPI_CALL1 (ret, Init, ());
#endif

    steam_log.LogEx (false, L"%s! (Status: %lu) [%d-bit]\n\n",
      ret ? L"done" : L"failed",
      (unsigned)ret,
#ifdef _WIN64
      64
#else
      32
#endif
      );

    return ret;
  }

  return false;
}

#include <ctime>

void
BMF::SteamAPI::Init (bool pre_load)
{
  // We allow a fixed number of chances to initialize, and then we give up.
  static int    init_tries = 0;
  static time_t last_try   = 0;

  if (init)
    return;

  InitializeCriticalSectionAndSpinCount (&callback_cs, 1024UL);

  // We want to give the init a second-chance because it's not quite
  //  up to snuff yet, but some games would just continue to try and
  //   do this indefinitely.
  if (init_tries > 4 && (time (NULL) - last_try) < 5)
    return;

  last_try = time (NULL);

  if (init_tries++ == 0) {
    steam_log.init ("logs/steam_api.log", "w");
    steam_log.silent = config.steam.silent;

    steam_log.Log (L"Initializing SteamWorks Backend");
    steam_log.Log (L"-------------------------------\n");
  }

#ifdef _WIN64
  const wchar_t* steam_dll_str    = L"steam_api64.dll";
#else
  const wchar_t* steam_dll_str    = L"steam_api.dll";
#endif

  HMODULE hSteamAPI = nullptr;
  bool    bImported = false;

  if (pre_load) {
    if (init_tries == 1) {
      steam_log.Log (L" @ %s was already loaded...\n", steam_dll_str);
    }

    hSteamAPI = GetModuleHandle (steam_dll_str);
  }
  else {
    hSteamAPI = LoadLibrary (steam_dll_str);
  }

  bImported = BMF_Load_SteamAPI_Imports (hSteamAPI, pre_load);

  if (! bImported) {
    init = false;
    return;
  }

  steam_ctx.Init (hSteamAPI);

  ISteamUserStats* stats = steam_ctx.UserStats ();

  if (stats)
    stats->RequestCurrentStats ();
  else 
  // Close, but no - we still have not initialized this monster.
  {
    init = false;
    return;
  }

  steam_log.Log (L" Creating Achievement Manager...");

  // Don't report our own callbacks!
  BMF_DisableHook (SteamAPI_RegisterCallback);
  {
    steam_achievements = new BMF_Steam_AchievementManager (
        config.steam.achievement_sound.c_str ()
      );
  }
  BMF_EnableHook (SteamAPI_RegisterCallback);

  steam_log.LogEx (false, L"\n");

  // Phew, finally!
  steam_log.Log (L"--- Initialization Finished (%d tries) ---", init_tries);

  init = true;
}

void
BMF::SteamAPI::Shutdown (void)
{
  BMF_AutoClose_Log (steam_log);

  steam_ctx.Shutdown ();
}

void BMF::SteamAPI::Pump (void)
{
#if 0
  if (steam_ctx.UserStats ()) {
    if (SteamAPI_RunCallbacks != nullptr)
      SteamAPI_RunCallbacks ();
  } else {
    Init (true);
  }
#endif
}

uint32_t
BMF::SteamAPI::AppID (void)
{
  ISteamUtils* utils = steam_ctx.Utils ();

  if (utils != nullptr) {
    uint32_t id = utils->GetAppID ();

    // If no AppID was manually set, let's assign one now.
    if (config.steam.appid == 0)
      config.steam.appid = id;

    return id;
  }

  return 0;
}

void
__stdcall
BMF::SteamAPI::SetOverlayState (bool active)
{
  EnterCriticalSection (&callback_cs);

  GameOverlayActivated_t state;
  state.m_bActive = active;

  std::set <CCallbackBase *>::iterator it =
    overlay_activation_callbacks.begin ();

  while (it != overlay_activation_callbacks.end ()) {
    (*it++)->Run (&state);
  }

  LeaveCriticalSection (&callback_cs);
}












//
// Hacks that break what little planning this project had to begin with ;)
//

void
__stdcall
BMF_SteamAPI_SetOverlayState (bool active)
{
  BMF::SteamAPI::SetOverlayState (active);
}