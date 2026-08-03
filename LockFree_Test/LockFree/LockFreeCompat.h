#pragma once
// ==========================================================================
// LockFreeCompat — Windows 원자연산·타입·컴파일러 지시자를 리눅스로 잇는 경계
//
//   이 저장소의 자료구조는 Win32 Interlocked* 위에서 설계·검증됐다. 리눅스로 옮길 때
//   호출부 18곳을 전부 고치는 대신, 같은 이름·같은 시그니처의 어댑터를 여기 두어
//   자료구조 코드 자체는 건드리지 않는다. 원본과의 diff가 작아야 이식 중 생긴
//   오류를 눈으로 잡을 수 있기 때문이다.
//
//   Windows에서는 <windows.h>를 그대로 include할 뿐, 아무것도 바꾸지 않는다.
//
//   [함정] 16바이트 CAS — GCC는 -mcx16 없이는 cmpxchg16b 대신 libatomic의
//          뮤텍스 폴백으로 조용히 내려간다. 락프리인 줄 알고 락을 쓰게 되므로
//          아래 static_assert로 컴파일 단계에서 막는다.
// ==========================================================================

#ifdef _WIN32

#include <windows.h>

#else   // ────────────── 리눅스 (GCC / Clang) ──────────────

#include <cstdint>
#include <cstring>

// ── 스칼라 타입 ──
//   MSVC의 __int64는 long long이다. LP64 리눅스에서는 long도 64비트지만,
//   호출부가 (INT64*)·(volatile INT64*)로 캐스팅하므로 타입이 정확히 일치해야 한다.
using INT64    = long long;
using LONG64   = long long;
using SHORT    = short;
using PVOID    = void*;
using UINT_PTR = std::uintptr_t;

#ifndef NULL
    #define NULL 0
#endif
#ifndef FALSE
    #define FALSE 0
#endif
#ifndef TRUE
    #define TRUE 1
#endif

// ── 컴파일러 지시자 ──
#define __forceinline inline __attribute__((always_inline))

// 이 저장소가 쓰는 __declspec은 noinline 하나뿐이라 인자를 그대로 속성으로 넘긴다.
//   dllexport 같은 다른 인자가 들어오면 이 매크로는 맞지 않는다 — 그때는 호출부를 고칠 것.
#ifndef __declspec
    #define __declspec(attr) __attribute__((attr))
#endif

// 복구 불가능한 불변식 위반에서 즉시 죽는다 (Windows __fastfail 대응).
//   abort()가 아니라 trap인 이유: 시그널 핸들러·atexit를 타지 않고 그 자리에서 멈춰야
//   디버거·코어덤프에 깨진 시점의 스택이 그대로 남는다.
#define FAST_FAIL_INVALID_ARG 5
#define __fastfail(code) __builtin_trap()

// 스핀 대기 힌트 — 경합 중 코어를 형제 스레드에 양보하고 전력·파이프라인 낭비를 줄인다.
#if defined(__x86_64__) || defined(__i386__)
    #define YieldProcessor() __builtin_ia32_pause()
#elif defined(__aarch64__)
    #define YieldProcessor() __asm__ __volatile__("yield" ::: "memory")
#else
    #define YieldProcessor() ((void)0)
#endif

// -- 16바이트 CAS 가용성 --
//   [실측 2026-08-03, g++ 13.3] 16바이트만은 __atomic_* 를 쓰면 안 된다.
//     - __atomic_compare_exchange_n(16B) -> -mcx16이 있어도 `call __atomic_compare_exchange_16@PLT`
//       (libatomic 함수 호출. 락프리이긴 하나 핫패스에 호출 오버헤드가 붙는다)
//     - __sync_val_compare_and_swap(16B) -> -mcx16이 있으면 `lock cmpxchg16b` 인라인
//   그래서 이 어댑터만 legacy __sync 계열을 쓴다. 8바이트/16비트/포인터는
//   -mcx16 없이도 __atomic_* 가 인라인(lock xaddq / lock xaddw / lock cmpxchgq)이라 그대로 둔다.
//
//   방어막도 __atomic_always_lock_free(16,0)으로는 못 세운다 - GCC가 16바이트를 인라인으로
//   처리하지 않아 -mcx16 유무와 무관하게 항상 false다. -mcx16일 때만 정의되는
//   __GCC_HAVE_SYNC_COMPARE_AND_SWAP_16으로 판별한다.
#ifndef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_16
    #error "16-byte CAS cannot be inlined here: add -mcx16. Without it the 128-bit CAS degrades to a libatomic call."
#endif

// ── 원자연산 어댑터 ──

// MSVC 원형: unsigned char _InterlockedCompareExchange128(
//                __int64 volatile* dest, __int64 high, __int64 low, __int64* comparandResult)
//   comparandResult는 in/out이다 — 실패하면 그 시점 관측값으로 덮어써진다.
//   GCC의 __atomic_compare_exchange_n도 expected를 같은 방식으로 다루므로 시맨틱이 그대로 맞는다.
//   메모리 배치: 리틀엔디안에서 comparandResult[0]=low, [1]=high 이고 이는 __int128의
//   바이트 순서와 같다. 그래서 필드를 뜯지 않고 memcpy로 통째로 옮긴다.
__forceinline unsigned char InterlockedCompareExchange128(
    volatile INT64* destination,
    INT64 exchangeHigh,
    INT64 exchangeLow,
    INT64* comparandResult)
{
    unsigned __int128 expected;
    __builtin_memcpy(&expected, comparandResult, sizeof(expected));

    const unsigned __int128 desired =
        (static_cast<unsigned __int128>(static_cast<unsigned long long>(exchangeHigh)) << 64) |
         static_cast<unsigned __int128>(static_cast<unsigned long long>(exchangeLow));

    // __sync_val_compare_and_swap은 "연산 전 값"을 돌려주고 full barrier다(Interlocked와 동일).
    //   성공했다면 정의상 이전 값 == 기대값이므로, 그 비교가 곱 성공 판정이 된다.
    const unsigned __int128 previous = __sync_val_compare_and_swap(
        reinterpret_cast<volatile unsigned __int128*>(destination), expected, desired);

    if (previous != expected)
    {
        __builtin_memcpy(comparandResult, &previous, sizeof(previous));
        return 0;
    }
    return 1;
}

// MSVC 원형: PVOID InterlockedCompareExchangePointer(PVOID volatile* dest, PVOID exchange, PVOID comparand)
//   반환값은 "교환 전 값"이다. 호출부는 이 값을 comparand와 비교해 성공을 판정한다.
//   성공하면 expected가 comparand 그대로 남고, 실패하면 관측값으로 갱신되므로 그대로 돌려주면 된다.
__forceinline PVOID InterlockedCompareExchangePointer(
    volatile PVOID* destination, PVOID exchange, PVOID comparand)
{
    PVOID expected = comparand;
    __atomic_compare_exchange_n(destination, &expected, exchange,
                                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

// 아래 셋은 모두 "연산 후 값"을 돌려준다 (Windows Interlocked* 계열과 동일).
//   호출부가 `if (0 == InterlockedDecrement16(...))` 처럼 결과값으로 분기하므로
//   fetch_ 계열이 아니라 _fetch 계열이어야 한다.
__forceinline LONG64 InterlockedIncrement64(volatile LONG64* addend)
{
    return __atomic_add_fetch(addend, 1, __ATOMIC_SEQ_CST);
}

__forceinline LONG64 InterlockedDecrement64(volatile LONG64* addend)
{
    return __atomic_sub_fetch(addend, 1, __ATOMIC_SEQ_CST);
}

// 대상은 ExternalTlsFreeList의 alignas(64) volatile SHORT FreeCount — 16비트 원자연산이다.
__forceinline SHORT InterlockedDecrement16(volatile SHORT* addend)
{
    return __atomic_sub_fetch(addend, 1, __ATOMIC_SEQ_CST);
}

#endif  // _WIN32
