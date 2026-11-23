/* Replacement for gameinput.dll: GameInput v0, the console's in-box version, for gamepads.
 *
 * WineGDK's GameInput reads only mice (GetCurrentReading returns E_NOTIMPL for anything else),
 * so a console title would see no controller. Layouts and method order are the v0 interfaces of
 * WineGDK's include/gameinput.idl. One gamepad is always connected: its state comes from XInput
 * (Wine maps real controllers to it), or, while no XInput controller is connected, from the
 * keyboard (REBOX_KEYBOARD_PAD=0 turns that off):
 *
 *   WASD left stick, arrows right stick, Space A, Left Ctrl B, R X, F Y,
 *   Q/E shoulders, Z/C triggers, Enter menu, Tab view, I/K/J/L D-pad, Shift/V stick clicks.
 *
 * The gamepad is also a UI navigation device (menus written against GameInputKindUiNavigation).
 * Each GetCurrentReading is a new reading; those whose state changed are also kept, so a title
 * that steps through readings with GetNextReading sees every press.
 *
 * Keyboard and mouse readings, raw reports, force feedback and the other device kinds report
 * that they have no readings or do nothing. The first call of each method with each input kind
 * is logged. */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xinput.h>

#include "../common/rebox_log.h"

#define GAMEINPUT_E_DEVICE_NOT_FOUND ((HRESULT)0x838a0002)
#define GAMEINPUT_E_READING_NOT_FOUND ((HRESULT)0x838a0003)
#define KIND_GAMEPAD 0x00040000u
#define KIND_UI_NAVIGATION 0x01000000u
#define KINDS (KIND_GAMEPAD | KIND_UI_NAVIGATION)
#define STATUS_CONNECTED 0x1u
/* GameInputDeviceStatus as xcl reports it: connected, input enabled, output enabled,
 * synchronized. A screen that wants input enabled ignores a device that's only connected. */
#define STATUS_ALL 0x47u

typedef struct
{
    uint32_t buttons;
    float leftTrigger, rightTrigger, leftThumbstickX, leftThumbstickY, rightThumbstickX, rightThumbstickY;
} GameInputGamepadState;

typedef struct
{
    float lowFrequency, highFrequency, leftTrigger, rightTrigger;
} GameInputRumbleParams;

typedef struct { uint16_t major, minor, build, revision; } GameInputVersion;
/* APP_LOCAL_DEVICE_ID comes from the Windows headers: 32 bytes. */

typedef struct
{
    uint32_t infoSize;
    uint16_t vendorId, productId, revisionNumber;
    uint8_t interfaceNumber, collectionNumber;
    struct { uint16_t page, id; } usage;
    GameInputVersion hardwareVersion, firmwareVersion;
    APP_LOCAL_DEVICE_ID deviceId, deviceRootId;
    int32_t deviceFamily;
    uint32_t capabilities, supportedInput, supportedRumbleMotors;
    uint32_t inputReportCount, outputReportCount, featureReportCount, controllerAxisCount, controllerButtonCount,
             controllerSwitchCount, touchPointCount, touchSensorCount, forceFeedbackMotorCount,
             hapticFeedbackMotorCount, deviceStringCount, deviceDescriptorSize;
    const void *inputReportInfo, *outputReportInfo, *featureReportInfo, *controllerAxisInfo, *controllerButtonInfo,
               *controllerSwitchInfo, *keyboardInfo, *mouseInfo, *touchSensorInfo, *motionInfo, *arcadeStickInfo,
               *flightStickInfo, *gamepadInfo, *racingWheelInfo, *uiNavigationInfo, *forceFeedbackMotorInfo,
               *hapticFeedbackMotorInfo, *displayName, *deviceStrings, *deviceDescriptorData;
    uint32_t supportedSystemButtons;
} GameInputDeviceInfo;
/* Checked against the header WineGDK generates from its gameinput.idl. */
C_ASSERT(sizeof(GameInputDeviceInfo) == 328);
C_ASSERT(offsetof(GameInputDeviceInfo, deviceId) == 32);
C_ASSERT(offsetof(GameInputDeviceInfo, supportedInput) == 104);
C_ASSERT(offsetof(GameInputDeviceInfo, gamepadInfo) == 256);
C_ASSERT(offsetof(GameInputDeviceInfo, supportedSystemButtons) == 320);
C_ASSERT(sizeof(GameInputGamepadState) == 28);
C_ASSERT(sizeof(GameInputRumbleParams) == 16);

typedef void (WINAPI *device_callback)(uint64_t token, void *context, void *device, uint64_t timestamp,
                                       uint32_t current, uint32_t previous);

static uint64_t timestamp_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;

    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart / (double)freq.QuadPart * 1e6);
}

/* ---------------------------------------------------------------------- controller state */

typedef DWORD (WINAPI *xinput_get_fn)(DWORD, XINPUT_STATE *);
typedef DWORD (WINAPI *xinput_set_fn)(DWORD, XINPUT_VIBRATION *);
static xinput_get_fn xinput_get;
static xinput_set_fn xinput_set;

static BOOL CALLBACK load_xinput(INIT_ONCE *once, void *param, void **ctx)
{
    HMODULE m = LoadLibraryA("xinput1_4.dll");

    if (!m)
        m = LoadLibraryA("xinput1_3.dll");
    if (m)
    {
        xinput_get = (xinput_get_fn)(void *)GetProcAddress(m, "XInputGetState");
        xinput_set = (xinput_set_fn)(void *)GetProcAddress(m, "XInputSetState");
    }
    rebox_log("gameinput: one gamepad, from %s", xinput_get ? "XInput, else the keyboard" : "the keyboard");
    return TRUE;
}

static float stick(SHORT v)
{
    return v < 0 ? v / 32768.0f : v / 32767.0f;
}

static BOOL key(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void read_keyboard(GameInputGamepadState *s)
{
    static const struct { int vk; uint32_t button; } map[] = {
        { VK_RETURN, 0x1 }, { VK_TAB, 0x2 }, { VK_SPACE, 0x4 }, { VK_LCONTROL, 0x8 }, { 'R', 0x10 }, { 'F', 0x20 },
        { 'I', 0x40 }, { 'K', 0x80 }, { 'J', 0x100 }, { 'L', 0x200 }, { 'Q', 0x400 }, { 'E', 0x800 },
        { VK_LSHIFT, 0x1000 }, { 'V', 0x2000 },
    };
    const char *env = getenv("REBOX_KEYBOARD_PAD");

    if (env && *env == '0')
        return;
    for (unsigned i = 0; i < ARRAYSIZE(map); i++)
        if (key(map[i].vk))
            s->buttons |= map[i].button;
    s->leftThumbstickX = (float)key('D') - (float)key('A');
    s->leftThumbstickY = (float)key('W') - (float)key('S');
    s->rightThumbstickX = (float)key(VK_RIGHT) - (float)key(VK_LEFT);
    s->rightThumbstickY = (float)key(VK_UP) - (float)key(VK_DOWN);
    s->leftTrigger = key('Z');
    s->rightTrigger = key('C');
}

static void read_gamepad(GameInputGamepadState *s)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    XINPUT_STATE x;

    InitOnceExecuteOnce(&once, load_xinput, NULL, NULL);
    memset(s, 0, sizeof(*s));
    if (xinput_get && xinput_get(0, &x) == ERROR_SUCCESS)
    {
        static const struct { WORD x; uint32_t g; } map[] = {
            { XINPUT_GAMEPAD_START, 0x1 }, { XINPUT_GAMEPAD_BACK, 0x2 }, { XINPUT_GAMEPAD_A, 0x4 },
            { XINPUT_GAMEPAD_B, 0x8 }, { XINPUT_GAMEPAD_X, 0x10 }, { XINPUT_GAMEPAD_Y, 0x20 },
            { XINPUT_GAMEPAD_DPAD_UP, 0x40 }, { XINPUT_GAMEPAD_DPAD_DOWN, 0x80 }, { XINPUT_GAMEPAD_DPAD_LEFT, 0x100 },
            { XINPUT_GAMEPAD_DPAD_RIGHT, 0x200 }, { XINPUT_GAMEPAD_LEFT_SHOULDER, 0x400 },
            { XINPUT_GAMEPAD_RIGHT_SHOULDER, 0x800 }, { XINPUT_GAMEPAD_LEFT_THUMB, 0x1000 },
            { XINPUT_GAMEPAD_RIGHT_THUMB, 0x2000 },
        };
        for (unsigned i = 0; i < ARRAYSIZE(map); i++)
            if (x.Gamepad.wButtons & map[i].x)
                s->buttons |= map[i].g;
        s->leftTrigger = x.Gamepad.bLeftTrigger / 255.0f;
        s->rightTrigger = x.Gamepad.bRightTrigger / 255.0f;
        s->leftThumbstickX = stick(x.Gamepad.sThumbLX);
        s->leftThumbstickY = stick(x.Gamepad.sThumbLY);
        s->rightThumbstickX = stick(x.Gamepad.sThumbRX);
        s->rightThumbstickY = stick(x.Gamepad.sThumbRY);
    }
    else
        read_keyboard(s);
}

/* ------------------------------------------------------------------------ COM plumbing */

struct object
{
    void *const *vtbl;
    LONG refs;
};

static ULONG WINAPI obj_AddRef(struct object *o) { return InterlockedIncrement(&o->refs); }

/* ------------------------------------------------------------------------------ device */

static const BYTE gamepad_info[56]; /* GameInputGamepadInfo: every label GameInputLabelNone */
static GameInputDeviceInfo device_info = {
    sizeof(GameInputDeviceInfo), 0x045e, 0x0b12, 1, 0, 0, { 0x01, 0x05 }, { 1 }, { 1 },
    { { 's', 'c', 'a', 'r', 'l', 'e', 't', 't', 1 } }, { { 's', 'c', 'a', 'r', 'l', 'e', 't', 't', 1 } },
    1 /* GameInputFamilyXboxOne */, 0, KIND_GAMEPAD,
    0xf /* low, high, left and right trigger motors */,
    .controllerAxisCount = 6, .controllerButtonCount = 14, .gamepadInfo = gamepad_info,
};

static HRESULT WINAPI device_QueryInterface(struct object *o, REFIID iid, void **out)
{
    *out = o;
    obj_AddRef(o);
    return S_OK;
}

static ULONG WINAPI device_Release(struct object *o) { return 1; }
static const GameInputDeviceInfo *WINAPI device_GetDeviceInfo(void *self) { return &device_info; }
static uint32_t WINAPI device_GetDeviceStatus(void *self) { return STATUS_ALL; }
/* GameInputBatteryState v0: four floats and a status, all 0 (unknown). */
static void WINAPI device_GetBatteryState(void *self, void *state) { if (state) memset(state, 0, 20); }
static HRESULT WINAPI device_CreateForceFeedbackEffect(void *self, uint32_t motor, const void *params, void **effect)
{
    if (effect) *effect = NULL;
    return E_NOTIMPL;
}
static bool WINAPI device_IsForceFeedbackMotorPoweredOn(void *self, uint32_t motor) { return false; }
static void WINAPI device_SetForceFeedbackMotorGain(void *self, uint32_t motor, float gain) {}
static HRESULT WINAPI device_SetHapticMotorState(void *self, uint32_t motor, const void *params) { return E_NOTIMPL; }

static void WINAPI device_SetRumbleState(void *self, const GameInputRumbleParams *p)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    XINPUT_VIBRATION v = { 0 };

    InitOnceExecuteOnce(&once, load_xinput, NULL, NULL);
    if (p)
    {
        v.wLeftMotorSpeed = (WORD)(min(max(p->lowFrequency, 0.0f), 1.0f) * 65535);
        v.wRightMotorSpeed = (WORD)(min(max(p->highFrequency, 0.0f), 1.0f) * 65535);
    }
    if (xinput_set)
        xinput_set(0, &v);
}

static void WINAPI device_SetInputSynchronizationState(void *self, bool enabled) {}
static void WINAPI device_SendInputSynchronizationHint(void *self) {}
static void WINAPI device_PowerOff(void *self) {}
static HRESULT WINAPI device_raw_report(void *self, uint32_t id, uint32_t kind, void **report)
{
    if (report) *report = NULL;
    return E_NOTIMPL;
}
static HRESULT WINAPI device_raw_feature(void *self, uint32_t id, void **report)
{
    if (report) *report = NULL;
    return E_NOTIMPL;
}
static HRESULT WINAPI device_raw_send(void *self, void *report) { return E_NOTIMPL; }
static HRESULT WINAPI device_raw_send_response(void *self, void *request, void **reply)
{
    if (reply) *reply = NULL;
    return E_NOTIMPL;
}
static HRESULT WINAPI device_raw_ioctl(void *self, uint32_t code, size_t in_size, const void *in, size_t out_size,
                                       void *out, size_t *size)
{
    if (size) *size = 0;
    return E_NOTIMPL;
}
static bool WINAPI device_AcquireExclusiveRawDeviceAccess(void *self, uint64_t timeout) { return false; }
static void WINAPI device_ReleaseExclusiveRawDeviceAccess(void *self) {}

static void *const device_vtbl[] = {
    device_QueryInterface, obj_AddRef, device_Release, device_GetDeviceInfo, device_GetDeviceStatus,
    device_GetBatteryState, device_CreateForceFeedbackEffect, device_IsForceFeedbackMotorPoweredOn,
    device_SetForceFeedbackMotorGain, device_SetHapticMotorState, device_SetRumbleState,
    device_SetInputSynchronizationState, device_SendInputSynchronizationHint, device_PowerOff, device_raw_report,
    device_raw_feature, device_raw_send, device_raw_send, device_raw_send_response, device_raw_ioctl,
    device_AcquireExclusiveRawDeviceAccess, device_ReleaseExclusiveRawDeviceAccess,
};

/* The one gamepad, never freed. */
static struct object gamepad = { device_vtbl, 1 };

/* ----------------------------------------------------------------------------- reading */

struct reading
{
    struct object obj;
    uint32_t kind;
    uint64_t timestamp, sequence;
    GameInputGamepadState state;
};

static HRESULT WINAPI reading_QueryInterface(struct object *o, REFIID iid, void **out)
{
    *out = o;
    obj_AddRef(o);
    return S_OK;
}

static ULONG WINAPI reading_Release(struct object *o)
{
    LONG left = InterlockedDecrement(&o->refs);
    if (!left)
        free(o);
    return left;
}

static uint32_t WINAPI reading_GetInputKind(struct reading *r) { return r->kind; }
static uint64_t WINAPI reading_GetSequenceNumber(struct reading *r, uint32_t kind) { return r->sequence; }
static uint64_t WINAPI reading_GetTimestamp(struct reading *r) { return r->timestamp; }
static void WINAPI reading_GetDevice(struct reading *r, void **device)
{
    obj_AddRef(&gamepad);
    *device = &gamepad;
}
static bool WINAPI reading_GetRawReport(struct reading *r, void **report)
{
    if (report) *report = NULL;
    return false;
}
static uint32_t WINAPI reading_zero_count(struct reading *r) { return 0; }
static uint32_t WINAPI reading_zero_state(struct reading *r, uint32_t count, void *state) { return 0; }
static bool WINAPI reading_no_state(struct reading *r, void *state) { return false; }
static bool WINAPI reading_GetGamepadState(struct reading *r, GameInputGamepadState *state)
{
    *state = r->state;
    return true;
}

/* GameInputUiNavigationButtons from the gamepad: A accept, B cancel, D-pad or left stick
 * directions, X/Y context 1/2, shoulders page left/right, triggers page up/down, right stick
 * scrolls. */
static uint32_t ui_navigation(const GameInputGamepadState *g)
{
    uint32_t b = g->buttons & 0x3; /* menu, view */
    if (g->buttons & 0x4) b |= 0x4;
    if (g->buttons & 0x8) b |= 0x8;
    if ((g->buttons & 0x40) || g->leftThumbstickY > 0.5f) b |= 0x10;
    if ((g->buttons & 0x80) || g->leftThumbstickY < -0.5f) b |= 0x20;
    if ((g->buttons & 0x100) || g->leftThumbstickX < -0.5f) b |= 0x40;
    if ((g->buttons & 0x200) || g->leftThumbstickX > 0.5f) b |= 0x80;
    if (g->buttons & 0x10) b |= 0x100;
    if (g->buttons & 0x20) b |= 0x200;
    if (g->leftTrigger > 0.5f) b |= 0x1000;
    if (g->rightTrigger > 0.5f) b |= 0x2000;
    if (g->buttons & 0x400) b |= 0x4000;
    if (g->buttons & 0x800) b |= 0x8000;
    if (g->rightThumbstickY > 0.5f) b |= 0x10000;
    if (g->rightThumbstickY < -0.5f) b |= 0x20000;
    if (g->rightThumbstickX < -0.5f) b |= 0x40000;
    if (g->rightThumbstickX > 0.5f) b |= 0x80000;
    return b;
}

static bool WINAPI reading_GetUiNavigationState(struct reading *r, uint32_t *state)
{
    *state = ui_navigation(&r->state);
    return true;
}

static void *const reading_vtbl[] = {
    reading_QueryInterface, obj_AddRef, reading_Release, reading_GetInputKind, reading_GetSequenceNumber,
    reading_GetTimestamp, reading_GetDevice, reading_GetRawReport,
    reading_zero_count, reading_zero_state, /* controller axes */
    reading_zero_count, reading_zero_state, /* controller buttons */
    reading_zero_count, reading_zero_state, /* controller switches */
    reading_zero_count, reading_zero_state, /* keys */
    reading_no_state,                       /* mouse */
    reading_zero_count, reading_zero_state, /* touch */
    reading_no_state, reading_no_state, reading_no_state, /* motion, arcade stick, flight stick */
    reading_GetGamepadState, reading_no_state, reading_GetUiNavigationState, /* gamepad, racing wheel, UI navigation */
};

/* ---------------------------------------------------------------------------- GameInput */

static LONG64 next_token = 1, sequence;

static HRESULT WINAPI input_QueryInterface(struct object *o, REFIID iid, void **out)
{
    *out = o;
    obj_AddRef(o);
    return S_OK;
}

static ULONG WINAPI input_Release(struct object *o) { return 1; }
static uint64_t WINAPI input_GetCurrentTimestamp(void *self) { return timestamp_us(); }

/* The first call of each method with each kind (and with each answer), for finding out how a
 * title reads its input. */
static void log_call(const char *method, uint32_t kind, HRESULT hr)
{
    static struct { const char *method; uint32_t kind; HRESULT hr; } seen[64];
    static LONG count;
    static SRWLOCK lock = SRWLOCK_INIT;
    BOOL known = FALSE;

    AcquireSRWLockExclusive(&lock);
    for (LONG i = 0; i < count && !known; i++)
        known = seen[i].method == method && seen[i].kind == kind && seen[i].hr == hr;
    if (!known && count < ARRAYSIZE(seen))
    {
        seen[count].method = method;
        seen[count].kind = kind;
        seen[count++].hr = hr;
        rebox_log("gameinput: %s(kind %#x): %#lx", method, kind, (unsigned long)hr);
    }
    ReleaseSRWLockExclusive(&lock);
}

/* The readings while the state changed, oldest first: each holds a reference here. */
#define HISTORY 64
static struct reading *history[HISTORY];
static unsigned history_count;
static SRWLOCK history_lock = SRWLOCK_INIT;

/* A new reading of the gamepad now. Every GetCurrentReading gets a fresh one, with a new
 * sequence number and timestamp (a GDK title polls it, and took no input when an unchanged state
 * gave back the same reading). */
static struct reading *new_reading(uint32_t kind)
{
    struct reading *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->obj = (struct object){ reading_vtbl, 1 };
    /* A gamepad reading says it's a gamepad, as before; one asked for as UI navigation is both. */
    r->kind = (kind & KIND_GAMEPAD) ? KIND_GAMEPAD : KINDS;
    r->timestamp = timestamp_us();
    r->sequence = InterlockedIncrement64(&sequence);
    read_gamepad(&r->state);
    return r;
}

/* Keeps a reading whose state differs from the last kept one. Caller holds history_lock. */
static void remember(struct reading *r)
{
    if (history_count && !memcmp(&history[history_count - 1]->state, &r->state, sizeof(r->state)))
        return;
    obj_AddRef(&r->obj);
    if (history_count == HISTORY)
    {
        reading_Release(&history[0]->obj);
        memmove(history, history + 1, (HISTORY - 1) * sizeof(*history));
        history_count--;
    }
    history[history_count++] = r;
}

/* Reads the gamepad into the history. Caller holds history_lock. */
static void sample(uint32_t kind)
{
    struct reading *r = new_reading(kind);
    if (r)
    {
        remember(r);
        reading_Release(&r->obj);
    }
}

static BOOL wanted(uint32_t kind, void *device)
{
    return (kind & KINDS) && (!device || device == &gamepad);
}

static HRESULT WINAPI input_GetCurrentReading(void *self, uint32_t kind, void *device, void **out)
{
    HRESULT hr = GAMEINPUT_E_READING_NOT_FOUND;
    struct reading *r;

    if (!out)
        return E_POINTER;
    *out = NULL;
    if (wanted(kind, device))
    {
        if ((r = new_reading(kind)))
        {
            AcquireSRWLockExclusive(&history_lock);
            remember(r);
            ReleaseSRWLockExclusive(&history_lock);
            *out = r;
            hr = S_OK;
        }
        else
            hr = E_OUTOFMEMORY;
    }
    log_call("GetCurrentReading", kind, hr);
    return hr;
}

/* The reading after (or before) a reference one, from the history. */
static HRESULT step(const char *method, void *reference, uint32_t kind, void *device, void **out, BOOL forward)
{
    struct reading *ref = reference;
    HRESULT hr = GAMEINPUT_E_READING_NOT_FOUND;

    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!ref)
        hr = E_INVALIDARG;
    else if (wanted(kind, device))
    {
        AcquireSRWLockExclusive(&history_lock);
        sample(kind);
        if (forward)
        {
            for (unsigned i = 0; i < history_count; i++)
                if (history[i]->sequence > ref->sequence)
                {
                    *out = history[i];
                    break;
                }
        }
        else
        {
            for (unsigned i = history_count; i-- > 0;)
                if (history[i]->sequence < ref->sequence)
                {
                    *out = history[i];
                    break;
                }
        }
        if (*out)
        {
            obj_AddRef(*out);
            hr = S_OK;
        }
        ReleaseSRWLockExclusive(&history_lock);
    }
    log_call(method, kind, hr);
    return hr;
}

static HRESULT WINAPI input_GetNextReading(void *self, void *reference, uint32_t kind, void *device, void **out)
{
    return step("GetNextReading", reference, kind, device, out, TRUE);
}
static HRESULT WINAPI input_GetPreviousReading(void *self, void *reference, uint32_t kind, void *device, void **out)
{
    return step("GetPreviousReading", reference, kind, device, out, FALSE);
}
static HRESULT WINAPI input_GetTemporalReading(void *self, uint64_t timestamp, void *device, void **out)
{
    if (out) *out = NULL;
    return GAMEINPUT_E_READING_NOT_FOUND;
}

static HRESULT WINAPI input_RegisterReadingCallback(void *self, void *device, uint32_t kind, float threshold,
                                                    void *context, void *callback, uint64_t *token)
{
    log_call("RegisterReadingCallback (never called back)", kind, S_OK);
    if (token) *token = InterlockedIncrement64(&next_token);
    return S_OK;
}

/* Enumeration reports the gamepad as connected, on this thread; it never disconnects. */
static HRESULT WINAPI input_RegisterDeviceCallback(void *self, void *device, uint32_t kind, uint32_t filter,
                                                   uint32_t enum_kind, void *context, device_callback callback,
                                                   uint64_t *token)
{
    uint64_t t = InterlockedIncrement64(&next_token);

    if (token)
        *token = t;
    log_call("RegisterDeviceCallback", kind, S_OK);
    if (callback && enum_kind && (kind & KINDS) && (filter & STATUS_CONNECTED)
            && (!device || device == &gamepad))
        callback(t, context, &gamepad, timestamp_us(), STATUS_ALL, 0);
    return S_OK;
}

static HRESULT WINAPI input_register_other(void *self, void *device, uint32_t filter, void *context, void *callback,
                                           uint64_t *token)
{
    if (token) *token = InterlockedIncrement64(&next_token);
    return S_OK;
}
static HRESULT WINAPI input_RegisterKeyboardLayoutCallback(void *self, void *device, void *context, void *callback,
                                                           uint64_t *token)
{
    if (token) *token = InterlockedIncrement64(&next_token);
    return S_OK;
}
static void WINAPI input_StopCallback(void *self, uint64_t token) {}
static bool WINAPI input_UnregisterCallback(void *self, uint64_t token, uint64_t timeout) { return true; }
static HRESULT WINAPI input_CreateDispatcher(void *self, void **out)
{
    if (out) *out = NULL;
    return E_NOTIMPL;
}
static HRESULT WINAPI input_CreateAggregateDevice(void *self, uint32_t kind, void **out)
{
    if (!out)
        return E_POINTER;
    if (!(kind & KINDS))
    {
        *out = NULL;
        return E_NOTIMPL;
    }
    obj_AddRef(&gamepad);
    *out = &gamepad;
    return S_OK;
}
static HRESULT WINAPI input_find(void *self, const void *value, void **out)
{
    if (out) *out = NULL;
    log_call("FindDevice (not by id)", 0, E_NOTIMPL);
    return E_NOTIMPL;
}

/* FindDeviceFromId: the gamepad for its APP_LOCAL_DEVICE_ID, the id XUser's device association
 * names (xcl, GPL-2.0-only, has it: a GDK title looks the associated device up here). */
static HRESULT WINAPI input_FindDeviceFromId(void *self, const APP_LOCAL_DEVICE_ID *id, void **out)
{
    HRESULT hr;
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (id && !memcmp(id, &device_info.deviceId, sizeof(*id)))
    {
        obj_AddRef(&gamepad);
        *out = &gamepad;
        hr = S_OK;
    }
    else
        hr = GAMEINPUT_E_DEVICE_NOT_FOUND;
    log_call("FindDeviceFromId", 0, hr);
    return hr;
}
static HRESULT WINAPI input_EnableOemDeviceSupport(void *self, uint16_t vid, uint16_t pid, uint8_t iface,
                                                   uint8_t collection)
{
    return S_OK;
}
static void WINAPI input_SetFocusPolicy(void *self, uint32_t policy) {}

static void *const input_vtbl[] = {
    input_QueryInterface, obj_AddRef, input_Release, input_GetCurrentTimestamp, input_GetCurrentReading,
    input_GetNextReading, input_GetPreviousReading, input_GetTemporalReading,
    input_RegisterReadingCallback, input_RegisterDeviceCallback, input_register_other /* system buttons */,
    input_RegisterKeyboardLayoutCallback, input_StopCallback, input_UnregisterCallback, input_CreateDispatcher,
    input_CreateAggregateDevice, input_FindDeviceFromId, input_find /* object */, input_find /* platform handle */,
    input_find /* platform string */, input_EnableOemDeviceSupport, input_SetFocusPolicy,
};

static struct object game_input = { input_vtbl, 1 };

HRESULT WINAPI GameInputCreate(void **out)
{
    if (!out)
        return E_POINTER;
    *out = &game_input;
    return S_OK;
}
