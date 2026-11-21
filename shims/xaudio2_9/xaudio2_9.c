/* Replacement for xaudio2_9.dll: Wine's XAudio2, minus the XMA2 failure.
 *
 * Under WineGDK, FAudio hands XMA2 to Wine's WMA decoder, which accepts only
 * WMA formats, so creating an XMA2 source voice fails. Console titles use
 * XMA2, and a failed voice can take a title down. Here everything goes to
 * Wine's xaudio2_8.dll (the same FAudio code built as 2.8; the 2.8/2.9 ABI is
 * identical apart from an XAPO helper), except XMA2 source voices: those
 * become PCM voices with the same rate and channels, and every XMA2 buffer
 * is replaced by silence of about the same length. Playback is silent but
 * timing, OnBufferEnd/OnStreamEnd callbacks and GetState keep working.
 *
 * Exports are by ordinal, as on the console (Bink imports ordinal 1). */

#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <xaudio2.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"

#define WAVE_FORMAT_XMA 0x0165
#define WAVE_FORMAT_XMA2 0x0166

/* XMA2WAVEFORMATEX (xma2defs.h), packed like WAVEFORMATEX. */
#pragma pack(push, 1)
typedef struct
{
    WAVEFORMATEX wfx;
    WORD NumStreams;
    DWORD ChannelMask;
    DWORD SamplesEncoded;
    DWORD BytesPerBlock;
    DWORD PlayBegin, PlayLength, LoopBegin, LoopLength;
    BYTE LoopCount, EncoderVersion;
    WORD BlockCount;
} XMA2WAVEFORMATEX;
#pragma pack(pop)

/* ----------------------------------------------------------------- backend */

static HMODULE backend;
static void *backend_fn[8];

static BOOL CALLBACK load_backend(INIT_ONCE *once, void *param, void **ctx)
{
    if (!(backend = LoadLibraryA("xaudio2_8.dll")))
    {
        rebox_log("xaudio2_9: cannot load Wine's xaudio2_8.dll");
        return TRUE;
    }
    for (int i = 1; i < 8; i++)
        backend_fn[i] = (void *)GetProcAddress(backend, MAKEINTRESOURCEA(i));
    return TRUE;
}

static void *backend_ordinal(int n)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, load_backend, NULL, NULL);
    return backend_fn[n];
}

/* Ordinals 2-7 pass straight through. Resolve the target lazily, then jump,
 * so every argument register survives, including XMM0-3 for floats
 * (X3DAudioInitialize). Stack: entry rsp = 8 mod 16; 4 pushes + 0x48 keeps
 * the XMM save area aligned and rsp = 0 mod 16 at the call after the 0x20
 * shadow space. */
#define FORWARD_ORDINAL(name, n)                                                  \
    void *name##_target(void) { return backend_ordinal(n); }                  \
    __asm__(".globl " #name "\n" #name ":\n"                                \
            "    pushq %rcx\n    pushq %rdx\n    pushq %r8\n    pushq %r9\n" \
            "    subq $0x48, %rsp\n"                                         \
            "    movdqa %xmm0, 0x00(%rsp)\n    movdqa %xmm1, 0x10(%rsp)\n"  \
            "    movdqa %xmm2, 0x20(%rsp)\n    movdqa %xmm3, 0x30(%rsp)\n"  \
            "    subq $0x20, %rsp\n"                                         \
            "    call " #name "_target\n"                                    \
            "    addq $0x20, %rsp\n"                                         \
            "    movdqa 0x00(%rsp), %xmm0\n    movdqa 0x10(%rsp), %xmm1\n"  \
            "    movdqa 0x20(%rsp), %xmm2\n    movdqa 0x30(%rsp), %xmm3\n"  \
            "    addq $0x48, %rsp\n"                                         \
            "    popq %r9\n    popq %r8\n    popq %rdx\n    popq %rcx\n"    \
            "    jmp *%rax\n");

FORWARD_ORDINAL(CreateAudioReverb, 2)
FORWARD_ORDINAL(CreateAudioVolumeMeter, 3)
FORWARD_ORDINAL(CreateFX, 4)
FORWARD_ORDINAL(X3DAudioCalculate, 5)
FORWARD_ORDINAL(X3DAudioInitialize, 6)
FORWARD_ORDINAL(XAudio2CreateWithVersionInfo, 7)

/* ------------------------------------------------------------------ thunks */

/* Wrappers keep the wrapped object at +8; forwarded slots use
 * mov rcx, [rcx+8]; mov rax, [rcx]; jmp [rax+slot*8]. */
static void *forward_thunk(unsigned slot)
{
    static BYTE *cur, *end;
    UINT32 disp = slot * 8;
    BYTE *t;

    if (!cur || cur + 16 > end)
    {
        cur = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        end = cur + 0x1000;
    }
    t = cur;
    cur += 16;
    t[0] = 0x48; t[1] = 0x8b; t[2] = 0x49; t[3] = 0x08;
    t[4] = 0x48; t[5] = 0x8b; t[6] = 0x01;
    t[7] = 0xff; t[8] = 0xa0; memcpy(t + 9, &disp, 4);
    return t;
}

/* ---------------------------------------------------------- silent voices */

struct xma_voice
{
    void **vtbl;
    IXAudio2SourceVoice *inner; /* +8, used by the forward thunks */
    UINT32 channels, rate;
    double samples_per_byte;
};

C_ASSERT(offsetof(struct xma_voice, inner) == 8);

static void **xma_voice_vtbl;

/* One zero-filled block shared by every silent buffer. It only ever grows;
 * older blocks stay allocated because queued buffers may still point at
 * them. */
static const BYTE *silence(SIZE_T bytes)
{
    static BYTE *block;
    static SIZE_T size;
    static SRWLOCK lock = SRWLOCK_INIT;
    const BYTE *p;

    AcquireSRWLockExclusive(&lock);
    if (bytes > size)
    {
        SIZE_T n = size ? size : 1 << 20;
        while (n < bytes)
            n *= 2;
        if ((block = calloc(1, n)))
            size = n;
        else
            size = 0;
    }
    p = block;
    ReleaseSRWLockExclusive(&lock);
    return p;
}

static HRESULT STDMETHODCALLTYPE xma_SubmitSourceBuffer(struct xma_voice *v, const XAUDIO2_BUFFER *buf,
        const XAUDIO2_BUFFER_WMA *wma)
{
    XAUDIO2_BUFFER pcm;
    UINT64 samples;

    if (!buf)
        return E_POINTER;
    pcm = *buf;
    /* XMA2 is compressed and we don't decode it: estimate the length from
     * the bytes, and make sure the play and loop regions fit. */
    samples = (UINT64)(buf->AudioBytes * v->samples_per_byte);
    if (buf->PlayLength && buf->PlayBegin + (UINT64)buf->PlayLength > samples)
        samples = buf->PlayBegin + (UINT64)buf->PlayLength;
    if (buf->LoopLength && buf->LoopBegin + (UINT64)buf->LoopLength > samples)
        samples = buf->LoopBegin + (UINT64)buf->LoopLength;
    if (!samples)
        samples = 1;
    if (samples > 0x10000000)
        samples = 0x10000000;
    pcm.AudioBytes = (UINT32)(samples * v->channels * 2);
    if (!(pcm.pAudioData = silence(pcm.AudioBytes)))
        return E_OUTOFMEMORY;
    return IXAudio2SourceVoice_SubmitSourceBuffer(v->inner, &pcm, NULL);
}

static void STDMETHODCALLTYPE xma_DestroyVoice(struct xma_voice *v)
{
    IXAudio2SourceVoice_DestroyVoice(v->inner);
    free(v);
}

/* ------------------------------------------------------------- the engine */

struct engine
{
    void **vtbl;
    IXAudio2 *inner; /* +8 */
    LONG refs;
};

C_ASSERT(offsetof(struct engine, inner) == 8);

static void **engine_vtbl;

static HRESULT STDMETHODCALLTYPE engine_QueryInterface(struct engine *e, REFIID riid, void **out)
{
    static const GUID iid_unknown = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

    if (IsEqualGUID(riid, &iid_unknown) || IsEqualGUID(riid, &IID_IXAudio2))
    {
        InterlockedIncrement(&e->refs);
        *out = e;
        return S_OK;
    }
    return IXAudio2_QueryInterface(e->inner, riid, out);
}

static ULONG STDMETHODCALLTYPE engine_AddRef(struct engine *e)
{
    return InterlockedIncrement(&e->refs);
}

static ULONG STDMETHODCALLTYPE engine_Release(struct engine *e)
{
    ULONG refs = InterlockedDecrement(&e->refs);
    if (!refs)
    {
        IXAudio2_Release(e->inner);
        free(e);
    }
    return refs;
}

static HRESULT STDMETHODCALLTYPE engine_CreateSourceVoice(struct engine *e, IXAudio2SourceVoice **out,
        const WAVEFORMATEX *fmt, UINT32 flags, float max_ratio, IXAudio2VoiceCallback *callback,
        const XAUDIO2_VOICE_SENDS *sends, const XAUDIO2_EFFECT_CHAIN *effects)
{
    const XMA2WAVEFORMATEX *xma = (const XMA2WAVEFORMATEX *)fmt;
    WAVEFORMATEX pcm;
    struct xma_voice *v;
    HRESULT hr;
    static LONG logged;

    if (!fmt || (fmt->wFormatTag != WAVE_FORMAT_XMA2 && fmt->wFormatTag != WAVE_FORMAT_XMA))
        return IXAudio2_CreateSourceVoice(e->inner, out, fmt, flags, max_ratio, callback, sends, effects);

    if (!InterlockedExchange(&logged, 1))
        rebox_log("xaudio2_9: XMA2 source voice (%u Hz, %u channels): playing silence, XMA2 isn't decoded yet",
                     (unsigned)fmt->nSamplesPerSec, fmt->nChannels);
    if (!out)
        return E_POINTER;
    pcm.wFormatTag = WAVE_FORMAT_PCM;
    pcm.nChannels = fmt->nChannels ? fmt->nChannels : 2;
    pcm.nSamplesPerSec = fmt->nSamplesPerSec ? fmt->nSamplesPerSec : 48000;
    pcm.wBitsPerSample = 16;
    pcm.nBlockAlign = pcm.nChannels * 2;
    pcm.nAvgBytesPerSec = pcm.nSamplesPerSec * pcm.nBlockAlign;
    pcm.cbSize = 0;

    v = calloc(1, sizeof(*v));
    v->vtbl = xma_voice_vtbl;
    v->channels = pcm.nChannels;
    v->rate = pcm.nSamplesPerSec;
    /* Samples per compressed byte: from the header when it says how much it
     * encodes, else a rough XMA2 ratio (~1 sample per byte per channel). */
    if (fmt->wFormatTag == WAVE_FORMAT_XMA2 && fmt->cbSize >= sizeof(*xma) - sizeof(WAVEFORMATEX)
        && xma->SamplesEncoded && xma->BlockCount && xma->BytesPerBlock)
        v->samples_per_byte = (double)xma->SamplesEncoded / ((double)xma->BlockCount * xma->BytesPerBlock);
    else
        v->samples_per_byte = 1.0 / pcm.nChannels;

    if (FAILED(hr = IXAudio2_CreateSourceVoice(e->inner, &v->inner, &pcm, flags, max_ratio, callback, sends,
                                               effects)))
    {
        free(v);
        return hr;
    }
    *out = (IXAudio2SourceVoice *)v;
    return S_OK;
}

static BOOL CALLBACK init_vtables(INIT_ONCE *once, void *param, void **ctx)
{
    static void *engine_slots[13], *voice_slots[29];

    for (unsigned i = 0; i < ARRAYSIZE(engine_slots); i++)
        engine_slots[i] = forward_thunk(i);
    engine_slots[0] = (void *)engine_QueryInterface;
    engine_slots[1] = (void *)engine_AddRef;
    engine_slots[2] = (void *)engine_Release;
    engine_slots[5] = (void *)engine_CreateSourceVoice;
    engine_vtbl = engine_slots;

    for (unsigned i = 0; i < ARRAYSIZE(voice_slots); i++)
        voice_slots[i] = forward_thunk(i);
    voice_slots[offsetof(IXAudio2SourceVoiceVtbl, DestroyVoice) / sizeof(void *)] = (void *)xma_DestroyVoice;
    voice_slots[offsetof(IXAudio2SourceVoiceVtbl, SubmitSourceBuffer) / sizeof(void *)] = (void *)xma_SubmitSourceBuffer;
    xma_voice_vtbl = voice_slots;
    return TRUE;
}

HRESULT WINAPI XAudio2Create(IXAudio2 **out, UINT32 flags, XAUDIO2_PROCESSOR processor)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    HRESULT (WINAPI *create)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR) = backend_ordinal(1);
    struct engine *e;
    IXAudio2 *inner;
    HRESULT hr;

    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!create)
        return E_FAIL;
    if (FAILED(hr = create(&inner, flags, processor)))
        return hr;
    InitOnceExecuteOnce(&once, init_vtables, NULL, NULL);
    e = calloc(1, sizeof(*e));
    e->vtbl = engine_vtbl;
    e->inner = inner;
    e->refs = 1;
    *out = (IXAudio2 *)e;
    return S_OK;
}
