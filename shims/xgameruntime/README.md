# xgameruntime: the Gaming Runtime stand-in

Titles load `xgameruntime.dll` with `LoadLibrary` and get every Gaming Runtime API family
(XUser, XGameSave, XPackage, XStore, XTaskQueue, ...) as an interface from `QueryApiImpl`.
WineGDK's runtime answers five of them, and its XUser is stubs. This directory is
[americas-recomp](https://github.com/realdody/americas-recomp)'s replacement
(`tools/xbox/platform/xgr` at 7b8e3428fd633266ec8f34806e0fc29d633fa3a0, GPL-2.0-or-later), which
vendors GrandTheftXbox's XTaskQueue/XAsync layer (MIT, `runtime/GTX_LICENSE.txt`). Its behaviour,
offline answers and settings are described in `UPSTREAM-README.md`, with a summary below.

`rebox-run` installs `build/xgameruntime.dll` like the other shims: system32 plus a native
override, so it replaces WineGDK's builtin.

## Build

The interface table (60 interfaces, every vtable slot) is generated from the Xodus project's IDLs
(`xodus-gaming/xgameruntime`, LGPL-2.1, pinned in `flake.nix`; the dev shell sets `XODUS_IDL`):
`idl_table.py` makes `build/xgr/table.json` and `gen_stub.py` the stub source, whose
`QueryApiImpl` hands out an object per interface ID. Methods the runtime implements replace the
stub's slots, and every other slot logs its first call and fails. `make` builds it with mingw's
g++ when `XODUS_IDL` is set.

Local changes for mingw, each marked "rebox local change" in the file:

- `runtime/impl_accessibility.cpp`: mingw's WinRT headers can't be included (they define
  `IReference<unsigned char>` twice), so the Windows caption settings aren't read and
  `XClosedCaptionGetProperties` reports the defaults.
- `runtime/xgr_internal.h`: defines `E_ILLEGAL_METHOD_CALL`, which mingw lacks.
- `runtime/gtx/log.h`, `gen_stub.py`: `_ReturnAddress` becomes `__builtin_return_address(0)`.

## What titles get

| Family | Answer |
|---|---|
| XTaskQueue, XAsync, XThread | Complete (GrandTheftXbox's layer) |
| XUser | One offline local user, always signed in (`XGR_GAMERTAG`, `XGR_USER_ID`), every privilege granted. The gamepad is associated with it once the title has it (`XGR_REASSOCIATE_SECONDS=N` reconnects it every N seconds, leaving the user and coming back; `rebox-run` sets 10, which one title's world-creation screen needed). Tokens fail with `E_GAMEUSER_FAILED_TO_GET_TOKEN`, so nothing reaches Xbox Live |
| XGameSave, XGameSaveFiles | Local storage under `%LOCALAPPDATA%\xgr\saves` in the prefix (`XGR_SAVE_ROOT`); each container is replaced atomically, nothing syncs |
| XPersistentLocalStorage | A directory with the quota from `MicrosoftGame.Config` (2048 MB in one title's). `rebox-run` puts it in the prefix (`XGR_PLS_DIR`) |
| XPackage | The title is one fully installed package: identity from `MicrosoftGame.Config`, every chunk Ready, install monitors complete |
| XStore | An active, full, permanent game licence; no add-ons; anything that needs the Store service fails with `E_GAMESTORE_NETWORK_ERROR` |
| XSystem, XSystemAnalytics, XDisplay, XNetworking | Series X device info, desktop HDR state, offline connectivity, port 3074 |
| XGameUI | Headless: dialogs complete with `E_ABORT` unless the title draws them itself |
| XAccessibility, XAppCapture, XGameStreaming, XLaunch, XGameEvent, XGameInvite | The documented offline or "not available" answers |

`rebox-run` sends the runtime's logs to the prefix: `xgameruntime.log` (`XGR_LOG`: every
interface asked for, each unimplemented method's first call, and interface IDs it doesn't know)
and `xgameruntime-tasks.log` (`GTX_LOG`). After a title's first run, the unknown IDs and
unimplemented methods in `xgameruntime.log` are what to implement next.

A title that stops waiting on the runtime shows it in `xgameruntime-tasks.log`: a wait in
`XAsyncGetStatus` or an untimed `XTaskQueueDispatch` that lasts 5 s logs the call, or the queue
and port with every async call in flight. `XGR_TRACE_ASYNC=1` also logs each call's beginning and
completion, with its queue and whether a completion callback goes there.

## Tests

`tests/run-xgr.sh` builds upstream's tests with mingw and runs them under Wine, `test_save`
twice (the second run checks that saves survived). Under Wine 11.21, 9 of 12 pass
outright:

- `test_launch`: 5 checks fail. Launching a URI through the program registered for its scheme,
  and `RegisterApplicationRestart`, aren't what Wine does.
- `test_system`: the desktop HDR state through DXGI, which Wine doesn't report.
- `test_package`: mounting the title's own persistent local storage as a package fails in the
  phase that changes `XGR_PLS_DIR`. The storage root and quota are read once per process and kept
  in statics, and under Wine the DLL isn't really unloaded between the test's phases, so the
  earlier phase's settings stay. A title sets nothing mid-run: `rebox-run` sets `XGR_PLS_DIR`
  before it starts, and the title's `MicrosoftGame.Config` gives the quota.
- `test_accessibility` isn't built: it needs the WinRT headers too.

Local changes that follow xcl's (ivall/xcl) answers, with which a retail title boots:

- `runtime/impl_package.cpp`: chunk, installation and download questions about any package
  identifier or selector are answered as for the installed package (ready, installed, nothing to
  download), and any installation monitor handle reports it installed; what the documented checks
  would have refused goes to `GTX_LOG` (the first 16). `XGR_STRICT_PACKAGE=1` keeps the checks.
- `runtime/impl_user.cpp`: `XUserGetMaxUsers` is 1, and token-and-signature requests complete with
  `HRESULT_FROM_WIN32(ERROR_INTERNET_CANNOT_CONNECT)` (no Xbox network) rather than
  `E_GAMEUSER_FAILED_TO_GET_TOKEN`.
- `runtime/impl_system.cpp`: the connectivity hint is internet access (`XGR_CONNECTIVITY=none`: none).
- `runtime/impl_save.cpp`: `XGameSaveGetContainerInfo` of a container that doesn't exist returns
  `E_GS_CONTAINER_NOT_FOUND` (0x80830010), not S_OK with no callback.
- `runtime/impl_user.cpp`: `XUserCheckPrivilege` takes its fifth argument and writes the deny reason
  (none); `XUserGetGamertag` tells a size probe (no buffer, or a short one) the size it needs.
