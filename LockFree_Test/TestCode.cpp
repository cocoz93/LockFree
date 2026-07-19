//
#include <iostream>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>
#include <cstring>
#include <thread>
#include <mutex>
#include <memory> 
#include <string>
#include <set>
#include <unordered_set>
#include <cstdlib>

//=============================================================================
// 경합 창 증폭 훅 (USE_RACE_HOOK 빌드 전용)
// 자료구조 헤더의 LF_RACE_HOOK()를 "1/8 확률 ~1µs 헛돌기"로 재정의한다.
// 스냅샷 읽기 사이의 나노초 틈이 마이크로초로 벌어져, OS 선점으로만 드물게
// 터지던 낡은-스냅샷 경합이 수 초 안에 재현된다. (일반 빌드에선 흔적 없음)
//   빌드 예: cl /O2 /DNDEBUG /DUSE_RACE_HOOK /std:c++17 /EHsc TestCode.cpp
//=============================================================================
#ifdef USE_RACE_HOOK
#include <windows.h>
static thread_local unsigned t_raceRng = 0;
static void RaceStall()
{
    // xorshift 난수로 스레드마다 스톨 타이밍을 다르게 섞는다 (전부 같이 멈추면 경합이 안 생김)
    unsigned x = t_raceRng;
    if (x == 0) x = GetCurrentThreadId() * 2654435761u + 97u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    t_raceRng = x;
    if ((x & 7u) == 0u)                        // 1/8 확률로만 스톨
    {
        volatile int v = 0;
        for (int i = 0; i < 4000; ++i) v++;    // ~1µs급: 다른 스레드가 여러 op를 끝낼 시간
    }
}
// Enqueue 전용(더 긴) 스톨. Enqueue 결함 창은 tail 노드가 Free→재활용되기까지
// 수 µs를 기다려야 열리므로, 일반 스톨(~1µs)로는 부족하다. 더 자주(1/4), 더 길게(~30µs)
// 멈춰 "낡은 tail 스냅샷"과 "재활용된 private 노드"가 시간상 겹치도록 만든다.
static void RaceStallEnq()
{
    unsigned x = t_raceRng;
    if (x == 0) x = GetCurrentThreadId() * 2654435761u + 101u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    t_raceRng = x;
    if ((x & 3u) == 0u)                        // 1/4 확률로 스톨
    {
        volatile int v = 0;
        for (int i = 0; i < 120000; ++i) v++;  // ~30µs급: tail 노드가 재활용될 시간
    }
}
#define LF_RACE_HOOK() RaceStall()
#define LF_RACE_HOOK_ENQ() RaceStallEnq()
#endif

#include "LockFree/InternalFreeList.h"
#include "LockFree/LockFreeStack.h"
#include "LockFree/LockFreeQueue.h"

//=============================================================================
// 테스트 설정 상수 (반복 횟수 조절 가능)
//=============================================================================
namespace TestConfig
{
    // Phase 1: 단일 스레드 테스트 (기능 검증 — 싱글 스레드이므로 과도한 반복 불필요)
    const uint64_t DATA_INTEGRITY_ITERATIONS = 500'000;              // 50만 번
    const uint64_t INVARIANT_ITERATIONS = 500'000;                   // 50만 번
    const uint64_t PLACEMENT_NEW_ITERATIONS = 500'000;               // 50만 번
    const uint64_t DEFENSE_ITERATIONS = 500'000;                     // 50만 번

    // Phase 2: 멀티스레드 테스트 (race window 노출을 위해 고반복 유지)
    const uint64_t MT_ALLOC_FREE_OPS_PER_THREAD = 10'000'000;      // 각 스레드당 Alloc/Free 횟수
    const uint64_t HIGH_CONTENTION_OPS_PER_THREAD = 10'000'000;    // 각 스레드당 작업 횟수

    // Stack/Queue 테스트
    const uint64_t STACK_SINGLE_ITERATIONS = 500'000;                // 50만 번
    const uint64_t QUEUE_SINGLE_ITERATIONS = 500'000;                // 50만 번
    const uint64_t NUMBERS_PER_THREAD = 10'000'000;                  // Producer-Consumer 각 스레드당

    // Phase 2-3: 보존 불변식 (Conservation) 테스트
    const int64_t CONSERVATION_PREFILL = 50'000;        // 미리 채우는 개수 (스레드 수보다 훨씬 크게)
    const int     CONSERVATION_SECONDS = 15;            // 스레드 조합당 실행 시간
    const int     CONSERVATION_HANG_SECONDS = 10;       // 이 시간 동안 진행 0이면 HANG 판정

    // 진행 상황 출력 주기
    const uint64_t PROGRESS_INTERVAL = 100'000;
}

// 전역 카운터
std::atomic<uint64_t> g_testCount(0);
std::atomic<uint64_t> g_totalIterations(0);

// 크래시 함수
void Crash()
{
    int* crash = nullptr;
    *crash = 0xDEADBEEF;
}

// 테스트 실패 시 크래시
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cout << "\n[CRASH] " << message << std::endl; \
            std::cout << "  File: " << __FILE__ << std::endl; \
            std::cout << "  Line: " << __LINE__ << std::endl; \
            std::cout << "  Iteration: " << g_totalIterations << std::endl; \
            Crash(); \
        } \
    } while(0)

// 진행 상황 출력
void PrintProgress(const char* testName, uint64_t current, uint64_t total)
{
    if (current % TestConfig::PROGRESS_INTERVAL == 0)
    {
        double progress = (double)current / total * 100.0;
        std::cout << "[" << testName << "] "
            << "진행: " << current << " / " << total
            << " (" << progress << "%)" << std::endl;
    }
}


//=============================================================================
//=============================================================================
//
//  InternalFreeList 테스트
//
//=============================================================================
//=============================================================================


//=============================================================================
// FreeList 테스트에 사용할 페이로드 구조체
//=============================================================================
struct TestPayload
{
    // Init이 정상 수행된 노드인지 빠르게 식별하는 고정 시그니처
    uint32_t magic;
    uint32_t threadId;
    uint32_t sequence;
    // (magic, threadId, sequence) 조합 무결성 확인용 경량 체크값
    uint32_t checksum;

    TestPayload() : magic(0), threadId(0), sequence(0), checksum(0) {}

    void Init(uint32_t tid, uint32_t seq)
    {
        // 필드 세트를 구성한 뒤, 동일 세트 여부를 Verify에서 재계산으로 확인한다.
        magic = 0xDEADBEEF;
        threadId = tid;
        sequence = seq;
        checksum = magic ^ threadId ^ sequence;
    }

    bool Verify() const
    {
        // magic이 유효하고, 현재 필드 조합으로 다시 만든 체크값이 저장값과 같아야 정상 데이터다.
        return magic == 0xDEADBEEF && checksum == (magic ^ threadId ^ sequence);
    }
};
static_assert(sizeof(TestPayload) == 16, "TestPayload must be 16 bytes");


//=============================================================================
// PlacementNew 테스트용 구조체 - 생성자/소멸자 호출 횟수 추적
//=============================================================================
struct LifecycleTracker
{
    static std::atomic<int64_t> CtorCount;
    static std::atomic<int64_t> DtorCount;

    uint64_t value;

    LifecycleTracker() : value(0xAAAAAAAA)
    {
        CtorCount.fetch_add(1, std::memory_order_relaxed);
    }
    ~LifecycleTracker()
    {
        DtorCount.fetch_add(1, std::memory_order_relaxed);
    }

    static void Reset()
    {
        CtorCount.store(0, std::memory_order_relaxed);
        DtorCount.store(0, std::memory_order_relaxed);
    }
};
std::atomic<int64_t> LifecycleTracker::CtorCount(0);
std::atomic<int64_t> LifecycleTracker::DtorCount(0);


//=============================================================================
// Phase 1-1: 싱글 스레드 - 데이터 무결성 반복 테스트
// Alloc → 데이터 기록 → Free → 재Alloc → 데이터 검증
//=============================================================================
void FreeList_Test_DataIntegrity()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 1-1] FreeList 데이터 무결성 테스트" << std::endl;
    std::cout << "  목표: " << TestConfig::DATA_INTEGRITY_ITERATIONS / 1'000'000 << "백만 번 반복" << std::endl;
    std::cout << "========================================" << std::endl;

    const uint64_t ITERATIONS = TestConfig::DATA_INTEGRITY_ITERATIONS;
    LockFree::CInternalFreeList<TestPayload> freeList;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> batchDis(1, 64);

    uint32_t globalSeq = 0;
    auto startTime = std::chrono::steady_clock::now();
    const auto TIMEOUT = std::chrono::minutes(5);

    for (uint64_t i = 0; i < ITERATIONS; i++)
    {
        g_totalIterations = i;
        PrintProgress("FreeList 무결성", i, ITERATIONS);

        int batchSize = batchDis(gen);
        std::vector<TestPayload*> allocated(batchSize);

        // Alloc → 데이터 기록
        for (int j = 0; j < batchSize; j++)
        {
            allocated[j] = freeList.Alloc();
            TEST_ASSERT(allocated[j] != nullptr, "Alloc 실패");
            allocated[j]->Init(0, globalSeq++);
        }

        // 데이터 검증 후 Free
        for (int j = 0; j < batchSize; j++)
        {
            TEST_ASSERT(allocated[j]->Verify(), "Free 전 데이터 손상 감지");
            bool freed = freeList.Free(allocated[j]);
            TEST_ASSERT(freed, "Free 실패");
        }

        if (std::chrono::steady_clock::now() - startTime > TIMEOUT)
        {
            std::cout << "[ERROR] 타임아웃 발생! (5분 초과)" << std::endl;
            Crash();
        }
    }

    auto endTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();

    std::cout << "\n[PASS] FreeList 데이터 무결성 테스트 완료!" << std::endl;
    std::cout << "  - 총 반복: " << ITERATIONS << " 회" << std::endl;
    std::cout << "  - 총 Alloc/Free: " << globalSeq << " 개" << std::endl;
    std::cout << "  - AllocCount: " << freeList.GetAllocCount() << std::endl;
    std::cout << "  - 소요 시간: " << elapsed << " 초" << std::endl;

    g_testCount++;
}


//=============================================================================
// Phase 1-2: 싱글 스레드 - 불변 조건 검증
// AllocCount >= 사용중 노드 수 + FreeListSize 항상 성립
// 노드 재활용 확인: Free 후 Alloc 시 같은 주소 반환 여부
//=============================================================================
void FreeList_Test_Invariants()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 1-2] FreeList 불변 조건 검증 테스트" << std::endl;
    std::cout << "  목표: " << TestConfig::INVARIANT_ITERATIONS / 1'000'000 << "백만 번 반복" << std::endl;
    std::cout << "========================================" << std::endl;

    const uint64_t ITERATIONS = TestConfig::INVARIANT_ITERATIONS;

    // UseApproxSize = true로 FreeListSize 추적
    LockFree::CInternalFreeList<TestPayload, false, true> freeList;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> opDis(0, 2);
    std::uniform_int_distribution<> batchDis(1, 16);

    std::vector<TestPayload*> inUse;
    inUse.reserve(1024);

    auto startTime = std::chrono::steady_clock::now();

    for (uint64_t i = 0; i < ITERATIONS; i++)
    {
        g_totalIterations = i;
        PrintProgress("불변 조건", i, ITERATIONS);

        int operation = opDis(gen);

        switch (operation)
        {
        case 0: // Alloc
        {
            int count = batchDis(gen);
            for (int j = 0; j < count; j++)
            {
                TestPayload* p = freeList.Alloc();
                TEST_ASSERT(p != nullptr, "Alloc 실패");
                p->Init(0, (uint32_t)i);
                inUse.push_back(p);
            }
            break;
        }
        case 1: // Free (일부)
        {
            if (!inUse.empty())
            {
                int count = (std::min)((int)batchDis(gen), (int)inUse.size());
                for (int j = 0; j < count; j++)
                {
                    TEST_ASSERT(inUse.back()->Verify(), "Free 전 데이터 손상");
                    bool freed = freeList.Free(inUse.back());
                    TEST_ASSERT(freed, "Free 실패");
                    inUse.pop_back();
                }
            }
            break;
        }
        case 2: // 불변 조건 검사
        {
            INT64 allocCount = freeList.GetAllocCount();
            INT64 freeListSize = freeList.GetFreeListSize();
            INT64 inUseCount = (INT64)inUse.size();

            // 불변 조건: AllocCount == 사용중 + FreeList에 보관중
            TEST_ASSERT(allocCount == inUseCount + freeListSize,
                "불변 조건 위반: AllocCount(" + std::to_string(allocCount) +
                ") != InUse(" + std::to_string(inUseCount) +
                ") + FreeListSize(" + std::to_string(freeListSize) + ")");
            break;
        }
        }
    }

    // 정리: 남은 노드 모두 Free
    for (auto* p : inUse)
    {
        TEST_ASSERT(p->Verify(), "최종 정리 시 데이터 손상");
        freeList.Free(p);
    }
    inUse.clear();

    // 최종 불변 조건: 모든 노드가 FreeList로 반환
    INT64 finalAllocCount = freeList.GetAllocCount();
    INT64 finalFreeListSize = freeList.GetFreeListSize();
    TEST_ASSERT(finalAllocCount == finalFreeListSize,
        "최종 불변 조건 위반: AllocCount != FreeListSize");

    auto endTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();

    std::cout << "\n[PASS] FreeList 불변 조건 검증 완료!" << std::endl;
    std::cout << "  - 총 반복: " << ITERATIONS << " 회" << std::endl;
    std::cout << "  - 최종 AllocCount: " << finalAllocCount << std::endl;
    std::cout << "  - 최종 FreeListSize: " << finalFreeListSize << std::endl;
    std::cout << "  - 소요 시간: " << elapsed << " 초" << std::endl;

    g_testCount++;
}


//=============================================================================
// Phase 1-3: 싱글 스레드 - PlacementNew 모드 생성자/소멸자 호출 검증
// PlacementNew=true 시 Alloc마다 생성자, Free마다 소멸자 호출 확인
//=============================================================================
void FreeList_Test_PlacementNew()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 1-3] PlacementNew 생성자/소멸자 검증 테스트" << std::endl;
    std::cout << "  목표: " << TestConfig::PLACEMENT_NEW_ITERATIONS / 1'000'000 << "백만 번 반복" << std::endl;
    std::cout << "========================================" << std::endl;

    const uint64_t ITERATIONS = TestConfig::PLACEMENT_NEW_ITERATIONS;
    LifecycleTracker::Reset();

    {
        // PlacementNew=true: Free 시 소멸자, 재Alloc 시 생성자 호출
        LockFree::CInternalFreeList<LifecycleTracker, true> freeList;

        // 최초 Alloc 시 AllocNewNode 내부에서 생성자 1회 호출
        // 이후 Free 시 소멸자, Alloc 시 생성자 호출

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> batchDis(1, 32);

        int64_t expectedCtorCount = 0;
        int64_t expectedDtorCount = 0;

        auto startTime = std::chrono::steady_clock::now();

        for (uint64_t i = 0; i < ITERATIONS; i++)
        {
            g_totalIterations = i;
            PrintProgress("PlacementNew", i, ITERATIONS);

            int batchSize = batchDis(gen);
            std::vector<LifecycleTracker*> allocated(batchSize);

            for (int j = 0; j < batchSize; j++)
            {
                allocated[j] = freeList.Alloc();
                TEST_ASSERT(allocated[j] != nullptr, "Alloc 실패");
                expectedCtorCount++;
            }

            for (int j = 0; j < batchSize; j++)
            {
                freeList.Free(allocated[j]);
                expectedDtorCount++;
            }
        }

        int64_t actualCtor = LifecycleTracker::CtorCount.load();
        int64_t actualDtor = LifecycleTracker::DtorCount.load();

        TEST_ASSERT(actualCtor == expectedCtorCount,
            "생성자 호출 횟수 불일치: expected=" + std::to_string(expectedCtorCount) +
            " actual=" + std::to_string(actualCtor));

        TEST_ASSERT(actualDtor == expectedDtorCount,
            "소멸자 호출 횟수 불일치: expected=" + std::to_string(expectedDtorCount) +
            " actual=" + std::to_string(actualDtor));

        auto endTime = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();

        std::cout << "\n[PASS] PlacementNew 검증 완료!" << std::endl;
        std::cout << "  - 총 반복: " << ITERATIONS << " 회" << std::endl;
        std::cout << "  - 생성자 호출: " << actualCtor << " 회 (예상: " << expectedCtorCount << ")" << std::endl;
        std::cout << "  - 소멸자 호출: " << actualDtor << " 회 (예상: " << expectedDtorCount << ")" << std::endl;
        std::cout << "  - 소요 시간: " << elapsed << " 초" << std::endl;

        // 소멸자에서 남은 노드들의 소멸자도 호출됨 (PlacementNew=true이므로 free list 노드는 이미 소멸자 호출됨, ~CInternalFreeList에서 skip)
    }

    g_testCount++;
}


//=============================================================================
// Phase 1-4: 싱글 스레드 - 잘못된 사용 패턴 방어 테스트
//=============================================================================
void FreeList_Test_Defense()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 1-4] FreeList 방어 로직 테스트" << std::endl;
    std::cout << "========================================" << std::endl;

    // 1. nullptr Free 시도
    {
        LockFree::CInternalFreeList<TestPayload> freeList;
        bool result = freeList.Free(nullptr);
        TEST_ASSERT(result == false, "nullptr Free가 허용됨");
        std::cout << "  > nullptr Free 방어: OK" << std::endl;
    }

    // 2. 노드 재활용 검증 (LIFO 특성)
    {
        LockFree::CInternalFreeList<TestPayload> freeList;
        TestPayload* p1 = freeList.Alloc();
        TEST_ASSERT(p1 != nullptr, "Alloc 실패");

        freeList.Free(p1);

        TestPayload* p2 = freeList.Alloc();
        TEST_ASSERT(p2 != nullptr, "재Alloc 실패");
        TEST_ASSERT(p1 == p2, "LIFO 재활용 실패: 같은 주소가 반환되어야 함");
        std::cout << "  > LIFO 노드 재활용: OK" << std::endl;

        freeList.Free(p2);
    }

    // 3. 대량 Alloc 후 전체 Free - 메모리 누수 없음 확인
    {
        LockFree::CInternalFreeList<TestPayload, false, true> freeList;
        const int COUNT = 10000;
        std::vector<TestPayload*> ptrs(COUNT);

        for (int i = 0; i < COUNT; i++)
        {
            ptrs[i] = freeList.Alloc();
            TEST_ASSERT(ptrs[i] != nullptr, "대량 Alloc 실패");
        }

        TEST_ASSERT(freeList.GetAllocCount() == COUNT, "AllocCount 불일치");

        for (int i = 0; i < COUNT; i++)
            freeList.Free(ptrs[i]);

        TEST_ASSERT(freeList.GetFreeListSize() == COUNT, "FreeListSize 불일치");
        std::cout << "  > 대량 Alloc/Free 무결성: OK (노드 " << COUNT << "개)" << std::endl;
    }

    // 4. 주소 고유성 검증 - Alloc된 주소가 중복되지 않음
    {
        LockFree::CInternalFreeList<TestPayload> freeList;
        const int COUNT = 10000;
        std::set<TestPayload*> addrSet;

        for (int i = 0; i < COUNT; i++)
        {
            TestPayload* p = freeList.Alloc();
            TEST_ASSERT(p != nullptr, "주소 고유성 Alloc 실패");
            auto result = addrSet.insert(p);
            TEST_ASSERT(result.second, "중복 주소 발견!");
        }

        TEST_ASSERT((int)addrSet.size() == COUNT, "고유 주소 개수 불일치");
        std::cout << "  > 주소 고유성 검증: OK (" << COUNT << "개 모두 고유)" << std::endl;

        for (auto* p : addrSet)
            freeList.Free(p);
    }

    // 5. 반복 Alloc/Free 스트레스 (Defense iterations만큼)
    {
        LockFree::CInternalFreeList<TestPayload> freeList;
        auto startTime = std::chrono::steady_clock::now();

        for (uint64_t i = 0; i < TestConfig::DEFENSE_ITERATIONS; i++)
        {
            g_totalIterations = i;
            PrintProgress("방어 스트레스", i, TestConfig::DEFENSE_ITERATIONS);

            TestPayload* p = freeList.Alloc();
            TEST_ASSERT(p != nullptr, "스트레스 Alloc 실패");
            p->Init(0, (uint32_t)i);
            TEST_ASSERT(p->Verify(), "스트레스 데이터 손상");
            freeList.Free(p);
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startTime).count();
        std::cout << "  > Alloc/Free 스트레스: OK (" << TestConfig::DEFENSE_ITERATIONS / 1'000'000 << "백만 번, " << elapsed << "초)" << std::endl;
    }

    std::cout << "\n[PASS] FreeList 방어 로직 테스트 완료!" << std::endl;
    g_testCount++;
}


//=============================================================================
// Phase 2-1: 멀티스레드 - Alloc/Free 정합성 테스트
// 여러 스레드가 동시에 Alloc/Free하며 주소 중복 없음, 데이터 corruption 없음 검증
//=============================================================================
void RunFreeListMTTest(
    int threadCount,
    uint64_t opsPerThread,
    const std::vector<std::string>& completedLines,
    const std::string& runningLine)
{
    LockFree::CInternalFreeList<TestPayload, false, true> freeList;

    std::atomic<uint64_t> totalAllocs(0);
    std::atomic<uint64_t> totalFrees(0);
    std::atomic<uint64_t> totalProgress(0);
    std::atomic<bool> verifyFailed(false);

    // 진행률 출력
    std::atomic<bool> running(true);
    std::atomic<bool> pauseProgress(false);
    std::thread progressThread([&]() {
        while (running)
        {
            if (!pauseProgress)
            {
#ifdef _WIN32
                system("cls");
#else
                system("clear");
#endif
                std::cout << "========================================" << std::endl;
                std::cout << "[Phase 2-1] FreeList 멀티스레드 정합성 테스트" << std::endl;
                std::cout << "========================================" << std::endl;
                for (size_t j = 0; j < completedLines.size(); ++j)
                    std::cout << completedLines[j] << std::endl;
                std::cout << runningLine << std::endl;

                uint64_t total = opsPerThread * threadCount;
                uint64_t current = totalProgress.load();
                double progress = (double)current / total * 100.0;

                std::cout << "[진행률] " << current << " / " << total
                    << " (" << progress << "%)" << std::endl;
                std::cout << "  - Alloc: " << totalAllocs.load()
                    << "  Free: " << totalFrees.load() << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        });

    std::vector<std::thread> threads;
    auto startTime = std::chrono::steady_clock::now();

    // 스레드 시드 미리 생성
    std::random_device rd;
    std::vector<unsigned int> seeds(threadCount);
    for (int i = 0; i < threadCount; i++) seeds[i] = rd();

    for (int tid = 0; tid < threadCount; tid++)
    {
        threads.emplace_back([&, tid]()
            {
                std::mt19937 gen(seeds[tid]);
                std::uniform_int_distribution<> batchDis(1, 32);

                // 각 스레드가 로컬로 보유하는 노드 목록
                std::vector<TestPayload*> localNodes;
                localNodes.reserve(256);

                uint32_t localSeq = 0;

                for (uint64_t i = 0; i < opsPerThread; i++)
                {
                    totalProgress++;

                    int batchSize = batchDis(gen);

                    // Alloc 단계
                    for (int j = 0; j < batchSize; j++)
                    {
                        TestPayload* p = freeList.Alloc();
                        TEST_ASSERT(p != nullptr, "MT Alloc 실패");
                        p->Init((uint32_t)tid, localSeq++);
                        localNodes.push_back(p);
                        totalAllocs++;
                    }

                    // 보유 노드가 일정 수 이상이면 절반 Free
                    if (localNodes.size() > 128)
                    {
                        size_t freeCount = localNodes.size() / 2;
                        for (size_t j = 0; j < freeCount; j++)
                        {
                            TEST_ASSERT(localNodes.back()->Verify(),
                                "MT Free 전 데이터 손상: tid=" + std::to_string(tid));
                            freeList.Free(localNodes.back());
                            localNodes.pop_back();
                            totalFrees++;
                        }
                    }
                }

                // 스레드 종료 전 잔여 노드 모두 Free
                for (auto* p : localNodes)
                {
                    TEST_ASSERT(p->Verify(),
                        "MT 최종 Free 전 데이터 손상: tid=" + std::to_string(tid));
                    freeList.Free(p);
                    totalFrees++;
                }
            });
    }

    for (auto& t : threads) t.join();

    pauseProgress = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    running = false;
    progressThread.join();

    auto endTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    // 최종 검증
    std::cout << "\n========================================" << std::endl;
    std::cout << "[최종 검증]" << std::endl;
    std::cout << "========================================" << std::endl;

    uint64_t finalAllocs = totalAllocs.load();
    uint64_t finalFrees = totalFrees.load();

    TEST_ASSERT(finalAllocs == finalFrees,
        "Alloc/Free 개수 불일치: Alloc=" + std::to_string(finalAllocs) +
        " Free=" + std::to_string(finalFrees));
    std::cout << "  > Alloc/Free 개수 일치: " << finalAllocs << " 개" << std::endl;

    // FreeList에 모든 노드가 반환되었는지 확인
    INT64 allocCount = freeList.GetAllocCount();
    INT64 freeListSize = freeList.GetFreeListSize();
    TEST_ASSERT(allocCount == freeListSize,
        "노드 반환 불일치: AllocCount=" + std::to_string(allocCount) +
        " FreeListSize=" + std::to_string(freeListSize));
    std::cout << "  > 모든 노드 반환 완료 (AllocCount == FreeListSize == " << allocCount << ")" << std::endl;
    std::cout << "  > 소요 시간: " << elapsed << " ms" << std::endl;

    if (elapsed > 0)
        std::cout << "  > 처리량: " << (finalAllocs * 1000 / elapsed) << " allocs/sec" << std::endl;

    std::cout << "\n[PASS] " << threadCount << "개 스레드 Alloc/Free 정합성 완료 (소요: "
        << elapsed / 1000.0 << "초)" << std::endl;
    std::cout << "========================================" << std::endl;

    g_testCount++;
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

void FreeList_Test_MT_AllocFree()
{
    std::vector<std::pair<int, std::string>> threadConfigs = {
        {2,  "2개 스레드"},
        {4,  "4개 스레드"},
        {8,  "8개 스레드"},
        {16, "16개 스레드"},
        {32, "32개 스레드"}
    };

    std::vector<std::string> completedLines;

    for (size_t i = 0; i < threadConfigs.size(); i++)
    {
        int threadCount = threadConfigs[i].first;
        std::string runningLine = "[" + threadConfigs[i].second + "] Alloc/Free 정합성 테스트 진행 중..";

        RunFreeListMTTest(
            threadCount,
            TestConfig::MT_ALLOC_FREE_OPS_PER_THREAD,
            completedLines,
            runningLine
        );

        completedLines.push_back("[" + threadConfigs[i].second + "] Alloc/Free 정합성 테스트 완료");
    }

#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
    std::cout << "========================================" << std::endl;
    for (const auto& line : completedLines)
        std::cout << line << std::endl;
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 2-1] 모든 스레드 조합 테스트 완료!" << std::endl;
    std::cout << "  - 총 " << threadConfigs.size() << "가지 조합 성공" << std::endl;
    std::cout << "========================================" << std::endl;
}


//=============================================================================
// Phase 2-2: 멀티스레드 - 고빈도 경합 + 데이터 corruption 감지
// 절반 스레드는 Alloc → 글로벌 큐에 Push, 절반은 Pop → 검증 → Free
// ABA 문제, 메모리 corruption 탐지
//=============================================================================
void RunFreeListHighContentionTest(
    int threadCount,
    uint64_t opsPerThread,
    const std::vector<std::string>& completedLines,
    const std::string& runningLine)
{
    LockFree::CInternalFreeList<TestPayload, false, true> freeList;

    // 스레드 간 노드 전달을 위한 공유 스택 (lock 기반 - 테스트 인프라용)
    std::mutex sharedMtx;
    std::vector<TestPayload*> sharedPool;
    sharedPool.reserve(65536);

    std::atomic<uint64_t> allocCount(0);
    std::atomic<uint64_t> freeCount(0);
    std::atomic<uint64_t> verifyCount(0);
    std::atomic<uint64_t> totalProgress(0);

    // 진행률 출력
    std::atomic<bool> running(true);
    std::atomic<bool> pauseProgress(false);
    std::thread progressThread([&]() {
        while (running)
        {
            if (!pauseProgress)
            {
#ifdef _WIN32
                system("cls");
#else
                system("clear");
#endif
                std::cout << "========================================" << std::endl;
                std::cout << "[Phase 2-2] FreeList 고빈도 경합 + corruption 감지" << std::endl;
                std::cout << "========================================" << std::endl;
                for (const auto& line : completedLines)
                    std::cout << line << std::endl;
                std::cout << runningLine << std::endl;

                uint64_t total = opsPerThread * threadCount;
                uint64_t current = totalProgress.load();
                double progress = (double)current / total * 100.0;

                std::cout << "[진행률] " << current << " / " << total
                    << " (" << progress << "%)" << std::endl;
                std::cout << "  - Alloc: " << allocCount.load()
                    << "  Verify+Free: " << freeCount.load() << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        });

    int producerCount = threadCount / 2;
    int consumerCount = threadCount - producerCount;
    std::atomic<bool> allProducersDone(false);

    std::vector<std::thread> threads;
    auto startTime = std::chrono::steady_clock::now();

    // Producer 스레드: Alloc → magic/checksum 기록 → 공유 풀에 Push
    for (int tid = 0; tid < producerCount; tid++)
    {
        threads.emplace_back([&, tid]()
            {
                uint32_t seq = 0;
                for (uint64_t i = 0; i < opsPerThread; i++)
                {
                    totalProgress++;

                    TestPayload* p = freeList.Alloc();
                    TEST_ASSERT(p != nullptr, "고빈도 Alloc 실패");
                    p->Init((uint32_t)tid, seq++);
                    allocCount++;

                    {
                        std::lock_guard<std::mutex> lock(sharedMtx);
                        sharedPool.push_back(p);
                    }
                }
            });
    }

    // Consumer 스레드: 공유 풀에서 Pop → magic/checksum 검증 → Free
    for (int tid = 0; tid < consumerCount; tid++)
    {
        threads.emplace_back([&, tid]()
            {
                for (uint64_t i = 0; i < opsPerThread; i++)
                {
                    totalProgress++;
                    TestPayload* p = nullptr;

                    // 풀에서 꺼내기
                    while (true)
                    {
                        {
                            std::lock_guard<std::mutex> lock(sharedMtx);
                            if (!sharedPool.empty())
                            {
                                p = sharedPool.back();
                                sharedPool.pop_back();
                            }
                        }

                        if (p != nullptr)
                            break;

                        // Producer가 모두 종료되었는데 풀이 비었으면 종료
                        if (allProducersDone)
                        {
                            std::lock_guard<std::mutex> lock(sharedMtx);
                            if (sharedPool.empty())
                                return;
                        }

                        YieldProcessor();
                    }

                    // 데이터 무결성 검증
                    TEST_ASSERT(p->Verify(),
                        "corruption 감지: magic=0x" + std::to_string(p->magic) +
                        " tid=" + std::to_string(p->threadId) +
                        " seq=" + std::to_string(p->sequence));
                    verifyCount++;

                    freeList.Free(p);
                    freeCount++;
                }
            });
    }

    // Producer 완료 대기
    for (int i = 0; i < producerCount; i++)
        threads[i].join();
    allProducersDone = true;

    // Consumer 완료 대기
    for (int i = producerCount; i < (int)threads.size(); i++)
        threads[i].join();

    // 잔여 노드 처리
    uint64_t drainCount = 0;
    for (auto* p : sharedPool)
    {
        TEST_ASSERT(p->Verify(), "잔여 노드 corruption 감지");
        freeList.Free(p);
        freeCount++;
        drainCount++;
    }
    sharedPool.clear();

    pauseProgress = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    running = false;
    progressThread.join();

    auto endTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    // 최종 검증
    std::cout << "\n========================================" << std::endl;
    std::cout << "[최종 검증]" << std::endl;
    std::cout << "========================================" << std::endl;

    uint64_t finalAlloc = allocCount.load();
    uint64_t finalFree = freeCount.load();

    TEST_ASSERT(finalAlloc == finalFree,
        "Alloc/Free 불일치: Alloc=" + std::to_string(finalAlloc) +
        " Free=" + std::to_string(finalFree));
    std::cout << "  > Alloc/Free 개수 일치: " << finalAlloc << " 개" << std::endl;
    std::cout << "  > 검증 통과: " << verifyCount.load() << " 개 (잔여 회수: " << drainCount << ")" << std::endl;

    INT64 finalAllocCount = freeList.GetAllocCount();
    INT64 finalFreeListSize = freeList.GetFreeListSize();
    TEST_ASSERT(finalAllocCount == finalFreeListSize,
        "노드 반환 불일치: AllocCount=" + std::to_string(finalAllocCount) +
        " FreeListSize=" + std::to_string(finalFreeListSize));
    std::cout << "  > 모든 노드 FreeList 반환 완료" << std::endl;
    std::cout << "  > 소요 시간: " << elapsed << " ms" << std::endl;

    if (elapsed > 0)
        std::cout << "  > 처리량: " << ((finalAlloc + finalFree) * 1000 / elapsed) << " ops/sec" << std::endl;

    std::cout << "\n[PASS] " << threadCount << "개 스레드 고빈도 경합 + corruption 감지 완료 (소요: "
        << elapsed / 1000.0 << "초)" << std::endl;
    std::cout << "========================================" << std::endl;

    g_testCount++;
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

void FreeList_Test_HighContention()
{
    std::vector<int> threadCounts = { 2, 4, 8, 16, 32 };
    std::vector<std::string> completedLines;

    for (size_t i = 0; i < threadCounts.size(); i++)
    {
        int threadCount = threadCounts[i];
        std::string runningLine = "[" + std::to_string(threadCount) + "개 스레드] 고빈도 경합 테스트 진행 중..";

        RunFreeListHighContentionTest(
            threadCount,
            TestConfig::HIGH_CONTENTION_OPS_PER_THREAD,
            completedLines,
            runningLine
        );

        completedLines.push_back("[" + std::to_string(threadCount) + "개 스레드] 고빈도 경합 테스트 완료");
    }

#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
    std::cout << "========================================" << std::endl;
    for (const auto& line : completedLines)
        std::cout << line << std::endl;
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Phase 2-2] 모든 고빈도 경합 테스트 완료!" << std::endl;
    std::cout << "  - 총 " << threadCounts.size() << "가지 스레드 조합 성공" << std::endl;
    std::cout << "========================================" << std::endl;
}


//=============================================================================
//=============================================================================
//
//  LockFreeStack 테스트
//
//=============================================================================
//=============================================================================


//=============================================================================
// Stack Phase 1-1: 싱글 스레드 - 데이터 무결성
// Push → Pop 반복, magic/checksum 검증 + LIFO 순서 확인
//=============================================================================
void Stack_Test_DataIntegrity()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Stack 1-1] 데이터 무결성 테스트" << std::endl;
    std::cout << "  목표: " << TestConfig::STACK_SINGLE_ITERATIONS / 1'000'000 << "백만 번 반복" << std::endl;
    std::cout << "========================================" << std::endl;

    const uint64_t ITERATIONS = TestConfig::STACK_SINGLE_ITERATIONS;
    LockFree::CLockFreeStack<TestPayload, true> stack;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> batchDis(1, 64);

    uint32_t globalSeq = 0;
    auto startTime = std::chrono::steady_clock::now();
    const auto TIMEOUT = std::chrono::minutes(5);

    for (uint64_t i = 0; i < ITERATIONS; i++)
    {
        g_totalIterations = i;
        PrintProgress("Stack 무결성", i, ITERATIONS);

        int batchSize = batchDis(gen);

        // Push
        for (int j = 0; j < batchSize; j++)
        {
            TestPayload payload;
            payload.Init(0, globalSeq++);
            bool pushed = stack.Push(payload);
            TEST_ASSERT(pushed, "Stack Push 실패");
        }

        // Pop + LIFO 순서 검증 (역순으로 나와야 함)
        uint32_t expectedSeq = globalSeq;
        for (int j = 0; j < batchSize; j++)
        {
            expectedSeq--;
            TestPayload popped;
            bool success = stack.Pop(&popped);
            TEST_ASSERT(success, "Stack Pop 실패");
            TEST_ASSERT(popped.Verify(), "Stack Pop 데이터 손상");
            TEST_ASSERT(popped.sequence == expectedSeq,
                "LIFO 순서 위반: expected=" + std::to_string(expectedSeq) +
                " actual=" + std::to_string(popped.sequence));
        }

        // 모든 Pop 후 시퀀스 복원 확인
        globalSeq = expectedSeq;

        TEST_ASSERT(stack.IsEmpty(), "배치 후 스택이 비어있지 않음");

        if (std::chrono::steady_clock::now() - startTime > TIMEOUT)
        {
            std::cout << "[ERROR] 타임아웃 발생! (5분 초과)" << std::endl;
            Crash();
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startTime).count();

    std::cout << "\n[PASS] Stack 데이터 무결성 테스트 완료!" << std::endl;
    std::cout << "  - 총 반복: " << ITERATIONS << " 회" << std::endl;
    std::cout << "  - 소요 시간: " << elapsed << " 초" << std::endl;

    g_testCount++;
}


//=============================================================================
// Stack Phase 1-2: 싱글 스레드 - 방어 로직 + 불변 조건
// 빈 스택 Pop, ApproxSize 정합성, 대량 Push/Pop 스트레스
//=============================================================================
void Stack_Test_Defense()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Stack 1-2] 방어 로직 + 불변 조건 테스트" << std::endl;
    std::cout << "========================================" << std::endl;

    // 1. 빈 스택에서 Pop
    {
        LockFree::CLockFreeStack<TestPayload, true> stack;
        TestPayload dummy;
        bool result = stack.Pop(&dummy);
        TEST_ASSERT(result == false, "빈 스택 Pop 성공됨");
        TEST_ASSERT(stack.IsEmpty(), "빈 스택 IsEmpty 실패");
        TEST_ASSERT(stack.GetApproxSize() == 0, "빈 스택 ApproxSize != 0");
        std::cout << "  > 빈 스택 Pop 방어: OK" << std::endl;
    }

    // 2. ApproxSize 불변 조건: Push +1, Pop -1
    {
        LockFree::CLockFreeStack<TestPayload, true> stack;
        const int COUNT = 10000;

        for (int i = 0; i < COUNT; i++)
        {
            TestPayload p;
            p.Init(0, (uint32_t)i);
            stack.Push(p);
            TEST_ASSERT(stack.GetApproxSize() == i + 1,
                "Push 후 ApproxSize 불일치: expected=" + std::to_string(i + 1));
        }

        for (int i = COUNT - 1; i >= 0; i--)
        {
            TestPayload p;
            stack.Pop(&p);
            TEST_ASSERT(stack.GetApproxSize() == i,
                "Pop 후 ApproxSize 불일치: expected=" + std::to_string(i));
        }

        TEST_ASSERT(stack.IsEmpty(), "전체 Pop 후 비어있지 않음");
        std::cout << "  > ApproxSize 불변 조건: OK (Push/Pop " << COUNT << "회)" << std::endl;
    }

    // 3. 대량 Push/Pop 스트레스 + LIFO 재검증
    {
        LockFree::CLockFreeStack<TestPayload, true> stack;
        auto startTime = std::chrono::steady_clock::now();

        for (uint64_t i = 0; i < TestConfig::DEFENSE_ITERATIONS; i++)
        {
            g_totalIterations = i;
            PrintProgress("Stack 스트레스", i, TestConfig::DEFENSE_ITERATIONS);

            TestPayload p;
            p.Init(0, (uint32_t)i);
            stack.Push(p);

            TestPayload out;
            bool success = stack.Pop(&out);
            TEST_ASSERT(success, "스트레스 Pop 실패");
            TEST_ASSERT(out.Verify(), "스트레스 데이터 손상");
            TEST_ASSERT(out.sequence == (uint32_t)i, "스트레스 LIFO 위반");
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startTime).count();
        std::cout << "  > Push/Pop 스트레스: OK (" << TestConfig::DEFENSE_ITERATIONS / 1'000'000 << "백만 번, " << elapsed << "초)" << std::endl;
    }

    std::cout << "\n[PASS] Stack 방어 로직 테스트 완료!" << std::endl;
    g_testCount++;
}


//=============================================================================
// Stack Phase 2-1: 멀티스레드 - Push/Pop 정합성
// N개 스레드가 고유 숫자를 Push, 모든 숫자가 정확히 1번만 Pop되는지 전수 검증
//=============================================================================
void RunStackMTTest(
    int producerCount,
    int consumerCount,
    int64_t numbersPerThread,
    const std::vector<std::string>& completedLines,
    const std::string& runningLine)
{
    const int64_t TOTAL_NUMBERS = numbersPerThread * producerCount;
    std::atomic<uint64_t> totalPushed(0);
    std::atomic<uint64_t> totalPopped(0);
    std::atomic<bool> allProducersDone(false);

    LockFree::CLockFreeStack<int, true> stack;

    auto dequeueCheck = std::make_unique<std::atomic<int>[]>(TOTAL_NUMBERS);
    for (int64_t i = 0; i < TOTAL_NUMBERS; i++)
        dequeueCheck[i] = 0;

    std::atomic<bool> running(true);
    std::atomic<bool> pauseProgress(false);
    std::thread progressThread([&]() {
        while (running)
        {
            if (!pauseProgress)
            {
#ifdef _WIN32
                system("cls");
#else
                system("clear");
#endif
                std::cout << "========================================" << std::endl;
                std::cout << "[Stack 2-1] Push/Pop 정합성 테스트" << std::endl;
                std::cout << "========================================" << std::endl;
                for (const auto& line : completedLines)
                    std::cout << line << std::endl;
                std::cout << runningLine << std::endl;
                double pushRate = (double)totalPushed / TOTAL_NUMBERS * 100.0;
                double popRate = (double)totalPopped / TOTAL_NUMBERS * 100.0;
                std::cout << "[진행률] Push: " << pushRate << "%, Pop: " << popRate << "%\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        });

    std::random_device rd;
    std::vector<unsigned int> producerSeeds(producerCount);
    std::vector<unsigned int> consumerSeeds(consumerCount);
    for (int i = 0; i < producerCount; i++) producerSeeds[i] = rd();
    for (int i = 0; i < consumerCount; i++) consumerSeeds[i] = rd();

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    auto startTime = std::chrono::steady_clock::now();

    for (int threadId = 0; threadId < producerCount; threadId++)
    {
        producers.emplace_back([&, threadId]()
            {
                std::mt19937 gen(producerSeeds[threadId]);
                std::uniform_int_distribution<> batchDis(1, 32);

                int64_t startNum = (int64_t)threadId * numbersPerThread;
                int64_t endNum = startNum + numbersPerThread;
                int64_t currentNum = startNum;

                while (currentNum < endNum)
                {
                    int64_t batchSize = (std::min)((int64_t)batchDis(gen), endNum - currentNum);

                    for (int64_t i = 0; i < batchSize; i++)
                    {
                        bool pushed = stack.Push(static_cast<int>(currentNum + i));
                        TEST_ASSERT(pushed, "MT Stack Push 실패");
                    }

                    currentNum += batchSize;
                    totalPushed += batchSize;
                }
            });
    }

    for (int consumerId = 0; consumerId < consumerCount; consumerId++)
    {
        consumers.emplace_back([&, consumerId]()
            {
                while (true)
                {
                    if (allProducersDone && totalPopped >= (uint64_t)TOTAL_NUMBERS)
                        break;

                    int value;
                    if (stack.Pop(&value))
                    {
                        TEST_ASSERT(value >= 0 && value < TOTAL_NUMBERS, "범위 초과");
                        int expected = 0;
                        bool success = dequeueCheck[value].compare_exchange_strong(expected, 1);
                        TEST_ASSERT(success, "중복 Pop 발견! value=" + std::to_string(value));
                        totalPopped++;
                    }
                }
            });
    }

    for (auto& t : producers) t.join();
    allProducersDone = true;
    for (auto& t : consumers) t.join();

    pauseProgress = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    running = false;
    progressThread.join();

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startTime).count();

    // 최종 검증
    std::cout << "\n========================================" << std::endl;
    std::cout << "[최종 검증]" << std::endl;
    std::cout << "========================================" << std::endl;

    TEST_ASSERT(totalPushed == (uint64_t)TOTAL_NUMBERS, "Push 개수 불일치");
    TEST_ASSERT(totalPopped == (uint64_t)TOTAL_NUMBERS, "Pop 개수 불일치");
    std::cout << "  > Push/Pop 개수 일치: " << TOTAL_NUMBERS << " 개" << std::endl;

    int missingCount = 0;
    for (int64_t i = 0; i < TOTAL_NUMBERS; i++)
    {
        if (dequeueCheck[i] == 0)
        {
            missingCount++;
            if (missingCount <= 10)
                std::cout << "  [ERROR] 누락된 숫자: " << i << std::endl;
        }
    }
    TEST_ASSERT(missingCount == 0, "누락된 숫자 " + std::to_string(missingCount) + "개 발견");
    std::cout << "  > 모든 숫자 정확히 1번씩 Pop 완료" << std::endl;

    TEST_ASSERT(stack.IsEmpty(), "스택이 완전히 비워지지 않음");
    std::cout << "  > 스택 완전히 비워짐" << std::endl;

    std::cout << "\n[PASS] Producer " << producerCount << " / Consumer " << consumerCount
        << " 완료 (소요: " << elapsed << "초)" << std::endl;
    std::cout << "========================================" << std::endl;

    g_testCount++;
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

void Stack_Test_MT_PushPop()
{
    std::vector<std::pair<int, int>> threadConfigs = {
        {1, 1}, {2, 2}, {4, 4}, {8, 8},
        {1, 8}, {8, 1}, {2, 6}, {6, 2}
    };

    std::vector<std::string> completedLines;

    for (size_t i = 0; i < threadConfigs.size(); i++)
    {
        int pc = threadConfigs[i].first;
        int cc = threadConfigs[i].second;
        std::string runningLine = "[" + std::to_string(pc) + "-" + std::to_string(cc) + "] 조합 테스트 진행 중..";

        RunStackMTTest(pc, cc, TestConfig::NUMBERS_PER_THREAD, completedLines, runningLine);
        completedLines.push_back("[" + std::to_string(pc) + "-" + std::to_string(cc) + "] 조합 테스트 완료");
    }

#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
    std::cout << "========================================" << std::endl;
    for (const auto& line : completedLines)
        std::cout << line << std::endl;
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Stack 2-1] 모든 조합 테스트 완료!" << std::endl;
    std::cout << "  - 총 " << threadConfigs.size() << "가지 조합 성공" << std::endl;
    std::cout << "========================================" << std::endl;
}


//=============================================================================
// Stack Phase 2-2: 멀티스레드 - 고빈도 경합 + corruption 감지
// 짝수 스레드: Push(magic+checksum), 홀수 스레드: Pop+검증
//=============================================================================
void RunStackHighContentionTest(
    int threadCount,
    uint64_t opsPerThread,
    const std::vector<std::string>& completedLines,
    const std::string& runningLine)
{
    LockFree::CLockFreeStack<TestPayload, true> stack;
    std::atomic<uint64_t> pushCount(0);
    std::atomic<uint64_t> popCount(0);
    std::atomic<uint64_t> totalAttempts(0);

    std::atomic<bool> running(true);
    std::atomic<bool> pauseProgress(false);
    std::thread progressThread([&]() {
        while (running)
        {
            if (!pauseProgress)
            {
#ifdef _WIN32
                system("cls");
#else
                system("clear");
#endif
                std::cout << "========================================" << std::endl;
                std::cout << "[Stack 2-2] 고빈도 경합 + corruption 감지" << std::endl;
                std::cout << "========================================" << std::endl;
                for (const auto& line : completedLines)
                    std::cout << line << std::endl;
                std::cout << runningLine << std::endl;

                uint64_t total = opsPerThread * threadCount;
                uint64_t current = totalAttempts.load();
                double progress = (double)current / total * 100.0;

                std::cout << "[진행률] " << current << " / " << total
                    << " (" << progress << "%)" << std::endl;
                std::cout << "  - Push 성공: " << pushCount.load()
                    << "  Pop 성공: " << popCount.load() << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        });

    std::vector<std::thread> threads;
    auto startTime = std::chrono::steady_clock::now();

    for (int i = 0; i < threadCount; i++)
    {
        threads.emplace_back([&, i]() {
            uint32_t seq = 0;
            for (uint64_t j = 0; j < opsPerThread; j++)
            {
                totalAttempts++;

                if (i % 2 == 0)
                {
                    TestPayload pkt;
                    pkt.Init(static_cast<uint32_t>(i), seq++);
                    if (stack.Push(pkt))
                        pushCount++;
                }
                else
                {
                    TestPayload pkt;
                    if (stack.Pop(&pkt))
                    {
                        TEST_ASSERT(pkt.Verify(),
                            "Stack corruption: magic=0x" + std::to_string(pkt.magic) +
                            " tid=" + std::to_string(pkt.threadId) +
                            " seq=" + std::to_string(pkt.sequence));
                        popCount++;
                    }
                }
            }
            });
    }

    for (auto& t : threads) t.join();

    // 잔여 데이터 회수 + 검증
    TestPayload drainPkt;
    uint64_t drainCount = 0;
    while (stack.Pop(&drainPkt))
    {
        TEST_ASSERT(drainPkt.Verify(), "Stack 잔여 패킷 corruption");
        drainCount++;
    }
    popCount += drainCount;

    pauseProgress = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    running = false;
    progressThread.join();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();

    std::cout << "\n========================================" << std::endl;
    std::cout << "[최종 검증]" << std::endl;
    std::cout << "========================================" << std::endl;

    uint64_t finalPush = pushCount.load();
    uint64_t finalPop = popCount.load();

    TEST_ASSERT(finalPop == finalPush, "Push/Pop 개수 불일치");
    TEST_ASSERT(stack.IsEmpty(), "스택이 비워지지 않음");

    std::cout << "  > Push: " << finalPush << ", Pop: " << finalPop << " (잔여 회수: " << drainCount << ")" << std::endl;
    std::cout << "  > 소요 시간: " << elapsed << " ms" << std::endl;

    if (elapsed > 0)
        std::cout << "  > 처리량: " << (finalPush * 1000 / elapsed) << " ops/sec" << std::endl;

    std::cout << "\n[PASS] " << threadCount << "개 스레드 Stack 고빈도 경합 완료 (소요: "
        << elapsed / 1000.0 << "초)" << std::endl;
    std::cout << "========================================" << std::endl;

    g_testCount++;
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

void Stack_Test_HighContention()
{
    std::vector<int> threadCounts = { 2, 4, 8, 16, 32 };
    std::vector<std::string> completedLines;

    for (size_t i = 0; i < threadCounts.size(); i++)
    {
        int tc = threadCounts[i];
        std::string runningLine = "[" + std::to_string(tc) + "개 스레드] Stack 고빈도 경합 진행 중..";

        RunStackHighContentionTest(tc, TestConfig::HIGH_CONTENTION_OPS_PER_THREAD, completedLines, runningLine);
        completedLines.push_back("[" + std::to_string(tc) + "개 스레드] Stack 고빈도 경합 완료");
    }

#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
    std::cout << "========================================" << std::endl;
    for (const auto& line : completedLines)
        std::cout << line << std::endl;
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Stack 2-2] 모든 고빈도 경합 테스트 완료!" << std::endl;
    std::cout << "  - 총 " << threadCounts.size() << "가지 조합 성공" << std::endl;
    std::cout << "========================================" << std::endl;
}


//=============================================================================
//=============================================================================
//
//  LockFreeQueue 테스트
//
//=============================================================================
//=============================================================================


//=============================================================================
// Queue Phase 1-1: 싱글 스레드 - FIFO 순서 검증 + 데이터 무결성
// Enqueue → Dequeue 반복, 순서가 FIFO로 유지되는지 검증
//=============================================================================
void Queue_Test_FIFOIntegrity()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Queue 1-1] FIFO 순서 + 데이터 무결성 테스트" << std::endl;
    std::cout << "  목표: " << TestConfig::QUEUE_SINGLE_ITERATIONS / 1'000'000 << "백만 번 반복" << std::endl;
    std::cout << "========================================" << std::endl;

    const uint64_t ITERATIONS = TestConfig::QUEUE_SINGLE_ITERATIONS;
    LockFree::CLockFreeQueue<TestPayload, false, true> queue;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> batchDis(1, 64);

    uint32_t writeSeq = 0;
    uint32_t readSeq = 0;
    auto startTime = std::chrono::steady_clock::now();
    const auto TIMEOUT = std::chrono::minutes(5);

    for (uint64_t i = 0; i < ITERATIONS; i++)
    {
        g_totalIterations = i;
        PrintProgress("Queue FIFO", i, ITERATIONS);

        int batchSize = batchDis(gen);

        // Enqueue
        for (int j = 0; j < batchSize; j++)
        {
            TestPayload payload;
            payload.Init(0, writeSeq++);
            bool enqueued = queue.Enqueue(payload);
            TEST_ASSERT(enqueued, "Queue Enqueue 실패");
        }

        // Dequeue + FIFO 순서 검증
        for (int j = 0; j < batchSize; j++)
        {
            TestPayload dequeued;
            bool success = queue.Dequeue(&dequeued);
            TEST_ASSERT(success, "Queue Dequeue 실패");
            TEST_ASSERT(dequeued.Verify(), "Queue Dequeue 데이터 손상");
            TEST_ASSERT(dequeued.sequence == readSeq,
                "FIFO 순서 위반: expected=" + std::to_string(readSeq) +
                " actual=" + std::to_string(dequeued.sequence));
            readSeq++;
        }

        TEST_ASSERT(queue.IsEmpty(), "배치 후 큐가 비어있지 않음");

        if (std::chrono::steady_clock::now() - startTime > TIMEOUT)
        {
            std::cout << "[ERROR] 타임아웃 발생! (5분 초과)" << std::endl;
            Crash();
        }
    }

    TEST_ASSERT(readSeq == writeSeq, "총 Enqueue/Dequeue 개수 불일치");

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startTime).count();

    std::cout << "\n[PASS] Queue FIFO 순서 + 데이터 무결성 테스트 완료!" << std::endl;
    std::cout << "  - 총 반복: " << ITERATIONS << " 회" << std::endl;
    std::cout << "  - 총 Enqueue/Dequeue: " << writeSeq << " 개" << std::endl;
    std::cout << "  - 소요 시간: " << elapsed << " 초" << std::endl;

    g_testCount++;
}


//=============================================================================
// Queue Phase 1-2: 싱글 스레드 - 방어 로직 + 불변 조건
// 빈 큐 Dequeue, ApproxSize 정합성, Clear 검증
//=============================================================================
void Queue_Test_Defense()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Queue 1-2] 방어 로직 + 불변 조건 테스트" << std::endl;
    std::cout << "========================================" << std::endl;

    // 1. 빈 큐에서 Dequeue
    {
        LockFree::CLockFreeQueue<TestPayload, false, true> queue;
        TestPayload dummy;
        bool result = queue.Dequeue(&dummy);
        TEST_ASSERT(result == false, "빈 큐 Dequeue 성공됨");
        TEST_ASSERT(queue.IsEmpty(), "빈 큐 IsEmpty 실패");
        TEST_ASSERT(queue.GetApproxSize() == 0, "빈 큐 ApproxSize != 0");
        std::cout << "  > 빈 큐 Dequeue 방어: OK" << std::endl;
    }

    // 2. ApproxSize 불변 조건: Enqueue +1, Dequeue -1
    {
        LockFree::CLockFreeQueue<TestPayload, false, true> queue;
        const int COUNT = 10000;

        for (int i = 0; i < COUNT; i++)
        {
            TestPayload p;
            p.Init(0, (uint32_t)i);
            queue.Enqueue(p);
            TEST_ASSERT(queue.GetApproxSize() == i + 1,
                "Enqueue 후 ApproxSize 불일치: expected=" + std::to_string(i + 1));
        }

        for (int i = 0; i < COUNT; i++)
        {
            TestPayload p;
            queue.Dequeue(&p);
            TEST_ASSERT(queue.GetApproxSize() == COUNT - 1 - i,
                "Dequeue 후 ApproxSize 불일치: expected=" + std::to_string(COUNT - 1 - i));
        }

        TEST_ASSERT(queue.IsEmpty(), "전체 Dequeue 후 비어있지 않음");
        std::cout << "  > ApproxSize 불변 조건: OK (Enqueue/Dequeue " << COUNT << "회)" << std::endl;
    }

    // 3. Clear 검증
    {
        LockFree::CLockFreeQueue<TestPayload, false, true> queue;
        for (int i = 0; i < 1000; i++)
        {
            TestPayload p;
            p.Init(0, (uint32_t)i);
            queue.Enqueue(p);
        }

        TEST_ASSERT(!queue.IsEmpty(), "Clear 전 큐가 비어있음");
        queue.Clear();
        TEST_ASSERT(queue.IsEmpty(), "Clear 후 큐가 비어있지 않음");
        TEST_ASSERT(queue.GetApproxSize() == 0, "Clear 후 ApproxSize != 0");
        std::cout << "  > Clear 검증: OK" << std::endl;
    }

    // 4. Push/Pop 스트레스 + FIFO 재검증
    {
        LockFree::CLockFreeQueue<TestPayload, false, true> queue;
        auto startTime = std::chrono::steady_clock::now();

        for (uint64_t i = 0; i < TestConfig::DEFENSE_ITERATIONS; i++)
        {
            g_totalIterations = i;
            PrintProgress("Queue 스트레스", i, TestConfig::DEFENSE_ITERATIONS);

            TestPayload p;
            p.Init(0, (uint32_t)i);
            queue.Enqueue(p);

            TestPayload out;
            bool success = queue.Dequeue(&out);
            TEST_ASSERT(success, "스트레스 Dequeue 실패");
            TEST_ASSERT(out.Verify(), "스트레스 데이터 손상");
            TEST_ASSERT(out.sequence == (uint32_t)i, "스트레스 FIFO 위반");
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startTime).count();
        std::cout << "  > Enqueue/Dequeue 스트레스: OK (" << TestConfig::DEFENSE_ITERATIONS / 1'000'000 << "백만 번, " << elapsed << "초)" << std::endl;
    }

    std::cout << "\n[PASS] Queue 방어 로직 테스트 완료!" << std::endl;
    g_testCount++;
}


//=============================================================================
// Queue Phase 2-1: 멀티스레드 - Producer-Consumer 정합성
// 여러 Producer가 고유 숫자 Enqueue, 여러 Consumer가 Dequeue → 전수 검증
//=============================================================================
void RunQueueProducerConsumerTest(
    int producerCount,
    int consumerCount,
    int64_t numbersPerThread,
    const std::vector<std::string>& completedLines,
    const std::string& runningLine)
{
    const int64_t TOTAL_NUMBERS = numbersPerThread * producerCount;
    std::atomic<uint64_t> totalEnqueued(0);
    std::atomic<uint64_t> totalDequeued(0);
    std::atomic<bool> allProducersDone(false);

    LockFree::CLockFreeQueue<int, false, true> queue;

    auto dequeueCheck = std::make_unique<std::atomic<int>[]>(TOTAL_NUMBERS);
    for (int64_t i = 0; i < TOTAL_NUMBERS; i++)
        dequeueCheck[i] = 0;

    std::atomic<bool> running(true);
    std::atomic<bool> pauseProgress(false);
    std::thread progressThread([&]() {
        while (running)
        {
            if (!pauseProgress)
            {
#ifdef _WIN32
                system("cls");
#else
                system("clear");
#endif
                std::cout << "========================================" << std::endl;
                std::cout << "[Queue 2-1] Producer-Consumer 정합성 테스트" << std::endl;
                std::cout << "========================================" << std::endl;
                for (const auto& line : completedLines)
                    std::cout << line << std::endl;
                std::cout << runningLine << std::endl;
                double enqRate = (double)totalEnqueued / TOTAL_NUMBERS * 100.0;
                double deqRate = (double)totalDequeued / TOTAL_NUMBERS * 100.0;
                std::cout << "[진행률] Enqueue: " << enqRate << "%, Dequeue: " << deqRate << "%\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        });

    std::random_device rd;
    std::vector<unsigned int> producerSeeds(producerCount);
    std::vector<unsigned int> consumerSeeds(consumerCount);
    for (int i = 0; i < producerCount; i++) producerSeeds[i] = rd();
    for (int i = 0; i < consumerCount; i++) consumerSeeds[i] = rd();

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    auto startTime = std::chrono::steady_clock::now();

    for (int threadId = 0; threadId < producerCount; threadId++)
    {
        producers.emplace_back([&, threadId]()
            {
                std::mt19937 gen(producerSeeds[threadId]);
                std::uniform_int_distribution<> batchDis(1, 32);

                int64_t startNum = (int64_t)threadId * numbersPerThread;
                int64_t endNum = startNum + numbersPerThread;
                int64_t currentNum = startNum;

                while (currentNum < endNum)
                {
                    int64_t batchSize = (std::min)((int64_t)batchDis(gen), endNum - currentNum);

                    for (int64_t i = 0; i < batchSize; i++)
                    {
                        bool enqueued = queue.Enqueue(static_cast<int>(currentNum + i));
                        TEST_ASSERT(enqueued, "MT Queue Enqueue 실패");
                    }

                    currentNum += batchSize;
                    totalEnqueued += batchSize;
                }
            });
    }

    for (int consumerId = 0; consumerId < consumerCount; consumerId++)
    {
        consumers.emplace_back([&, consumerId]()
            {
                uint64_t spinCount = 0;
                while (true)
                {
                    if (allProducersDone && totalDequeued >= (uint64_t)TOTAL_NUMBERS)
                        break;

                    // 노드 유실/큐 오염 시 영원히 대기하는 대신 명시적 FAIL (4096회마다 시계 확인)
                    if ((++spinCount & 0xFFF) == 0 &&
                        std::chrono::steady_clock::now() - startTime > std::chrono::minutes(5))
                    {
                        std::cout << "\n[CRASH] Consumer 타임아웃(5분): Dequeue 진행 불가 (dequeued="
                            << totalDequeued << " / " << TOTAL_NUMBERS << ")" << std::endl;
                        Crash();
                    }

                    int value;
                    if (queue.Dequeue(&value))
                    {
                        TEST_ASSERT(value >= 0 && value < TOTAL_NUMBERS,
                            "범위 초과 숫자 발견: " + std::to_string(value));
                        int expected = 0;
                        bool success = dequeueCheck[value].compare_exchange_strong(expected, 1);
                        TEST_ASSERT(success, "중복 Dequeue 발견! value=" + std::to_string(value));
                        totalDequeued++;
                    }
                }
            });
    }

    for (auto& t : producers) t.join();
    allProducersDone = true;
    for (auto& t : consumers) t.join();

    pauseProgress = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    running = false;
    progressThread.join();

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startTime).count();

    // 최종 검증
    std::cout << "\n========================================" << std::endl;
    std::cout << "[최종 검증]" << std::endl;
    std::cout << "========================================" << std::endl;

    TEST_ASSERT(totalEnqueued == (uint64_t)TOTAL_NUMBERS, "Enqueue 개수 불일치");
    TEST_ASSERT(totalDequeued == (uint64_t)TOTAL_NUMBERS, "Dequeue 개수 불일치");
    std::cout << "  > Enqueue/Dequeue 개수 일치: " << TOTAL_NUMBERS << " 개" << std::endl;

    int missingCount = 0;
    int duplicateCount = 0;
    for (int64_t i = 0; i < TOTAL_NUMBERS; i++)
    {
        int status = dequeueCheck[i];
        if (status == 0)
        {
            missingCount++;
            if (missingCount <= 10)
                std::cout << "  [ERROR] 누락된 숫자: " << i << std::endl;
        }
        else if (status > 1)
        {
            duplicateCount++;
            if (duplicateCount <= 10)
                std::cout << "  [ERROR] 중복 처리: " << i << " (횟수: " << status << ")" << std::endl;
        }
    }

    TEST_ASSERT(missingCount == 0, "누락된 숫자 " + std::to_string(missingCount) + "개 발견");
    TEST_ASSERT(duplicateCount == 0, "중복 처리된 숫자 " + std::to_string(duplicateCount) + "개 발견");
    std::cout << "  > 모든 숫자 정확히 1번씩 처리 완료" << std::endl;

    TEST_ASSERT(queue.IsEmpty(), "큐가 완전히 비워지지 않음");
    std::cout << "  > 큐 완전히 비워짐" << std::endl;

    std::cout << "\n[PASS] Producer " << producerCount << " / Consumer " << consumerCount
        << " 완료 (소요: " << elapsed << "초)" << std::endl;
    std::cout << "========================================" << std::endl;

    g_testCount++;
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

void Queue_Test_ProducerConsumer()
{
    std::vector<std::pair<int, int>> threadConfigs = {
        {1, 1}, {2, 2}, {4, 4}, {8, 8},
        {1, 8}, {8, 1}, {2, 6}, {6, 2}
    };

    std::vector<std::string> completedLines;

    for (size_t i = 0; i < threadConfigs.size(); i++)
    {
        int pc = threadConfigs[i].first;
        int cc = threadConfigs[i].second;
        std::string runningLine = "[" + std::to_string(pc) + "-" + std::to_string(cc) + "] 조합 테스트 진행 중..";

        RunQueueProducerConsumerTest(pc, cc, TestConfig::NUMBERS_PER_THREAD, completedLines, runningLine);
        completedLines.push_back("[" + std::to_string(pc) + "-" + std::to_string(cc) + "] 조합 테스트 완료");
    }

#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
    std::cout << "========================================" << std::endl;
    for (const auto& line : completedLines)
        std::cout << line << std::endl;
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Queue 2-1] 모든 조합 테스트 완료!" << std::endl;
    std::cout << "  - 총 " << threadConfigs.size() << "가지 조합 성공" << std::endl;
    std::cout << "========================================" << std::endl;
}


//=============================================================================
// Queue Phase 2-2: 멀티스레드 - 고빈도 경합 + corruption 감지
// 짝수 스레드: Enqueue(magic+checksum), 홀수 스레드: Dequeue+검증
//=============================================================================
void RunQueueHighContentionTest(
    int threadCount,
    uint64_t opsPerThread,
    const std::vector<std::string>& completedLines,
    const std::string& runningLine)
{
    LockFree::CLockFreeQueue<TestPayload, false, true> queue;
    std::atomic<uint64_t> enqueueCount(0);
    std::atomic<uint64_t> dequeueCount(0);
    std::atomic<uint64_t> totalAttempts(0);

    std::atomic<bool> running(true);
    std::atomic<bool> pauseProgress(false);
    std::thread progressThread([&]() {
        while (running)
        {
            if (!pauseProgress)
            {
#ifdef _WIN32
                system("cls");
#else
                system("clear");
#endif
                std::cout << "========================================" << std::endl;
                std::cout << "[Queue 2-2] 고빈도 경합 + corruption 감지" << std::endl;
                std::cout << "========================================" << std::endl;
                for (const auto& line : completedLines)
                    std::cout << line << std::endl;
                std::cout << runningLine << std::endl;

                uint64_t total = opsPerThread * threadCount;
                uint64_t current = totalAttempts.load();
                double progress = (double)current / total * 100.0;

                std::cout << "[진행률] " << current << " / " << total
                    << " (" << progress << "%)" << std::endl;
                std::cout << "  - Enqueue 성공: " << enqueueCount.load()
                    << "  Dequeue 성공: " << dequeueCount.load() << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        });

    std::vector<std::thread> threads;
    auto startTime = std::chrono::steady_clock::now();

    for (int i = 0; i < threadCount; i++)
    {
        threads.emplace_back([&, i]() {
            uint32_t seq = 0;
            for (uint64_t j = 0; j < opsPerThread; j++)
            {
                totalAttempts++;

                if (i % 2 == 0)
                {
                    TestPayload pkt;
                    pkt.Init(static_cast<uint32_t>(i), seq++);
                    if (queue.Enqueue(pkt))
                        enqueueCount++;
                }
                else
                {
                    TestPayload pkt;
                    if (queue.Dequeue(&pkt))
                    {
                        TEST_ASSERT(pkt.Verify(),
                            "Queue corruption: magic=0x" + std::to_string(pkt.magic) +
                            " tid=" + std::to_string(pkt.threadId) +
                            " seq=" + std::to_string(pkt.sequence));
                        dequeueCount++;
                    }
                }
            }
            });
    }

    for (auto& t : threads) t.join();

    // 잔여 데이터 회수 + 검증
    TestPayload drainPkt;
    uint64_t drainCount = 0;
    while (queue.Dequeue(&drainPkt))
    {
        TEST_ASSERT(drainPkt.Verify(), "Queue 잔여 패킷 corruption");
        drainCount++;
    }
    dequeueCount += drainCount;

    pauseProgress = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    running = false;
    progressThread.join();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();

    std::cout << "\n========================================" << std::endl;
    std::cout << "[최종 검증]" << std::endl;
    std::cout << "========================================" << std::endl;

    uint64_t finalEnq = enqueueCount.load();
    uint64_t finalDeq = dequeueCount.load();

    TEST_ASSERT(finalDeq == finalEnq, "Enqueue/Dequeue 개수 불일치");
    TEST_ASSERT(queue.IsEmpty(), "큐가 비워지지 않음");

    std::cout << "  > Enqueue: " << finalEnq << ", Dequeue: " << finalDeq << " (잔여 회수: " << drainCount << ")" << std::endl;
    std::cout << "  > 소요 시간: " << elapsed << " ms" << std::endl;

    if (elapsed > 0)
        std::cout << "  > 처리량: " << (finalEnq * 1000 / elapsed) << " ops/sec" << std::endl;

    std::cout << "\n[PASS] " << threadCount << "개 스레드 Queue 고빈도 경합 완료 (소요: "
        << elapsed / 1000.0 << "초)" << std::endl;
    std::cout << "========================================" << std::endl;

    g_testCount++;
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

void Queue_Test_HighContention()
{
    std::vector<int> threadCounts = { 2, 4, 8, 16, 32 };
    std::vector<std::string> completedLines;

    for (size_t i = 0; i < threadCounts.size(); i++)
    {
        int tc = threadCounts[i];
        std::string runningLine = "[" + std::to_string(tc) + "개 스레드] Queue 고빈도 경합 진행 중..";

        RunQueueHighContentionTest(tc, TestConfig::HIGH_CONTENTION_OPS_PER_THREAD, completedLines, runningLine);
        completedLines.push_back("[" + std::to_string(tc) + "개 스레드] Queue 고빈도 경합 완료");
    }

#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
    std::cout << "========================================" << std::endl;
    for (const auto& line : completedLines)
        std::cout << line << std::endl;
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Queue 2-2] 모든 고빈도 경합 테스트 완료!" << std::endl;
    std::cout << "  - 총 " << threadCounts.size() << "가지 조합 성공" << std::endl;
    std::cout << "========================================" << std::endl;
}


//=============================================================================
//=============================================================================
//
//  보존 불변식 (Conservation) 테스트 — Stack / Queue 공용
//
//=============================================================================
//=============================================================================
//
// 원리:
//   1) 자료구조에 PREFILL개(sequence 1..N)를 미리 채운다.
//   2) T개 스레드가 "꺼내기 성공 → 즉시 같은 값 되넣기"만 반복한다.
//      → 각 스레드가 손에 든 값은 최대 1개. 내부에는 항상 PREFILL−T개 이상
//        남으므로 논리적으로 절대 비지 않는다.
//   3) 따라서 꺼내기가 false를 1번이라도 반환하면 그 자체로 "허위 empty" 결함 입증.
//      (Producer-Consumer 테스트는 큐가 진짜 빌 수 있어 false를 의심할 수 없음)
//   4) 진행 카운터가 CONSERVATION_HANG_SECONDS 동안 멈추면 HANG 판정 → 명시적 FAIL.
//   5) 종료 후 전량 회수해 개수·sequence 합계를 대조 → 유실/중복/파손 검출.
//
// USE_RACE_HOOK 빌드와 조합하면 낡은-스냅샷 경합류 회귀를 수 초 안에 잡는다.
//=============================================================================
template <typename TryTakeFn, typename PutBackFn>
void RunConservationTest(const char* name, int threadCount,
    TryTakeFn tryTake, PutBackFn putBack)
{
    const int64_t PREFILL = TestConfig::CONSERVATION_PREFILL;
    const int64_t EXPECT_SUM = PREFILL * (PREFILL + 1) / 2;

    std::cout << "\n[" << name << " 보존 불변식] 스레드 " << threadCount
        << "개, " << TestConfig::CONSERVATION_SECONDS << "초 (불변식: 내부 잔량 >= "
        << PREFILL - threadCount << " > 0)" << std::endl;

    // 1) 프리필 (sequence 1..PREFILL)
    for (int64_t i = 1; i <= PREFILL; i++)
    {
        TestPayload p;
        p.Init(0, (uint32_t)i);
        TEST_ASSERT(putBack(p), "Conservation 프리필 실패");
    }

    std::atomic<uint64_t> ops(0);
    std::atomic<uint64_t> falseEmpty(0);
    std::atomic<bool> stop(false);

    // 2) 꺼내면 즉시 되넣기 (꺼낸 뒤에는 stop 여부와 무관하게 반드시 되넣어 보존 유지)
    std::vector<std::thread> threads;
    for (int t = 0; t < threadCount; t++)
    {
        threads.emplace_back([&]() {
            TestPayload p;
            while (!stop.load(std::memory_order_relaxed))
            {
                if (tryTake(&p))
                {
                    TEST_ASSERT(p.Verify(), "Conservation 페이로드 손상");
                    TEST_ASSERT(putBack(p), "Conservation 되넣기 실패");
                }
                else
                {
                    // 불변식상 불가능 — 1회라도 나오면 허위 empty 결함
                    falseEmpty.fetch_add(1, std::memory_order_relaxed);
                }
                ops.fetch_add(1, std::memory_order_relaxed);
            }
            });
    }

    // 3) 워치독: 진행 카운터가 일정 시간 멈추면 HANG 판정 (조용한 멈춤 → 명시적 FAIL)
    auto startTime = std::chrono::steady_clock::now();
    uint64_t lastOps = 0;
    int stallTicks = 0;
    const int HANG_TICKS = TestConfig::CONSERVATION_HANG_SECONDS * 2;   // 500ms 단위

    while (std::chrono::steady_clock::now() - startTime
        < std::chrono::seconds(TestConfig::CONSERVATION_SECONDS))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        uint64_t cur = ops.load();
        if (cur == lastOps)
        {
            if (++stallTicks >= HANG_TICKS)
            {
                std::cout << "\n[CRASH] " << name << " Conservation HANG: "
                    << TestConfig::CONSERVATION_HANG_SECONDS << "초간 진행 없음 (ops="
                    << cur << ", falseEmpty=" << falseEmpty.load() << ")" << std::endl;
                Crash();
            }
        }
        else
            stallTicks = 0;
        lastOps = cur;
    }

    stop = true;
    for (auto& t : threads) t.join();

    // 4) 전량 회수 → 보존 검증 (개수·합계 대조. 진단을 위해 판정 전에 수치 출력)
    TestPayload p;
    int64_t cnt = 0;
    int64_t sum = 0;
    while (tryTake(&p))
    {
        TEST_ASSERT(p.Verify(), "Conservation 회수 페이로드 손상");
        cnt++;
        sum += p.sequence;
        TEST_ASSERT(cnt <= PREFILL, "Conservation 회수 개수 초과 (노드 중복/순환 의심)");
    }

    uint64_t fe = falseEmpty.load();
    std::cout << "  > 총 op: " << ops.load()
        << ", 허위 empty: " << fe
        << ", 회수: " << cnt << "/" << PREFILL
        << ", 합계: " << sum << "/" << EXPECT_SUM << std::endl;

    TEST_ASSERT(fe == 0, "허위 empty " + std::to_string(fe) + "회: 비어있지 않은데 false 반환");
    TEST_ASSERT(cnt == PREFILL, "회수 개수 불일치 (노드 유실)");
    TEST_ASSERT(sum == EXPECT_SUM, "회수 합계 불일치 (중복/파손)");

    g_testCount++;
}

void Stack_Test_Conservation()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Stack 2-3] 보존 불변식 테스트" << std::endl;
    std::cout << "========================================" << std::endl;

    // 40스레드 = 물리 코어 대비 과구독 → 연산 한복판 선점을 강제해 경합 창을 연다
    for (int threadCount : { 8, 40 })
    {
        LockFree::CLockFreeStack<TestPayload, true> stack;
        RunConservationTest("Stack", threadCount,
            [&](TestPayload* p) { return stack.Pop(p); },
            [&](const TestPayload& v) { return stack.Push(v); });

        TEST_ASSERT(stack.IsEmpty(), "회수 후 스택이 비어있지 않음");
        TEST_ASSERT(stack.GetApproxSize() == 0, "회수 후 ApproxSize != 0");
    }

    std::cout << "\n[PASS] Stack 보존 불변식 테스트 완료!" << std::endl;
}

void Queue_Test_Conservation()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Queue 2-3] 보존 불변식 테스트" << std::endl;
    std::cout << "========================================" << std::endl;

    // 40스레드 = 물리 코어 대비 과구독 → 연산 한복판 선점을 강제해 경합 창을 연다
    for (int threadCount : { 8, 40 })
    {
        LockFree::CLockFreeQueue<TestPayload, false, true> queue;
        RunConservationTest("Queue", threadCount,
            [&](TestPayload* p) { return queue.Dequeue(p); },
            [&](const TestPayload& v) { return queue.Enqueue(v); });

        TEST_ASSERT(queue.IsEmpty(), "회수 후 큐가 비어있지 않음");
        TEST_ASSERT(queue.GetApproxSize() == 0, "회수 후 ApproxSize != 0");
    }

    std::cout << "\n[PASS] Queue 보존 불변식 테스트 완료!" << std::endl;
}


//=============================================================================
// Queue Phase 2-4: 멀티스레드 - 생산자별 FIFO(순서 보존) 반증 테스트  [Q-E1]
//
// 왜 필요한가:
//   기존 MT 큐 테스트(ProducerConsumer/HighContention/Conservation)는 개수·중복·
//   누락·데이터 파손만 본다. Q-E1 결함(재활용된 private tail 노드에 낡은 Enqueue가
//   링크)은 개수·중복은 보존하고 "생산자별 순서"만 뒤집으므로, 순서 축의 검사가
//   있어야만 잡힌다.
//
// 불변식(올바른 FIFO 큐 + 단일 소비자):
//   각 생산자는 단일 스레드로 seq=0,1,2,..를 순서대로 넣고, FIFO는 생산자별 순서를
//   보존하며, 소비자가 하나뿐이라 전량을 Dequeue 순서 그대로 관측한다.
//   => 어떤 생산자의 seq가 (직전 관측값 + 1)이 아니면 그 자체로 순서 위반 = 결함.
//   이 불변식은 타이밍과 무관하게 리스트 구조로 성립하므로, 위반은 위양성이 아니다.
//
// 재현 설계:
//   - outstanding 상한으로 큐 깊이를 제한 → 스냅샷된 tail 노드가 곧 Dequeue·Free·
//     재활용되어 Enqueue 증폭 훅(C/D)이 여는 창과 시간상 겹친다.
//   - 상한을 (Enqueue 스톨 동안 재활용 가능한 선에서) 넉넉히 잡아, 피해 생산자가
//     "추월 아이템(s+1)"을 막힘없이 넣게 한다. s는 큐 밖에 매달려 있으므로 리스트에는
//     s+1이 먼저 들어가고, 소비자는 s+1을 s보다 먼저 관측 → 연속성 위반 확정.
//   - 값에 (producerId, seq) 인코딩. 첫 위반에서 증거를 남기고 중단.
//=============================================================================
static bool RunQueuePerProducerFIFO(int producerCount, int outstandingCap, int durationSeconds)
{
    std::cout << "\n[Queue 2-4] 생산자별 FIFO 반증: 생산자 " << producerCount
              << ", outstanding<=" << outstandingCap << ", " << durationSeconds << "초"
#ifdef USE_RACE_HOOK
              << "  (RACE_HOOK ON)"
#else
              << "  (RACE_HOOK OFF - 대조군)"
#endif
              << std::endl;

    LockFree::CLockFreeQueue<uint64_t> queue;
    const uint64_t SEQ_SPAN = 1'000'000'000'000ULL;   // producerId 분리 (seq < 1e12 가정)

    std::atomic<int64_t>  outstanding(0);
    std::atomic<bool>     stop(false);
    std::atomic<uint64_t> totalDeq(0);

    // 위반 기록 (단일 소비자가 기록, join 뒤 메인이 판정)
    std::atomic<bool> violated(false);
    int      vProducer = -1;
    int64_t  vExpected = -1, vGot = -1;

    std::vector<uint64_t> produced(producerCount, 0);

    // 생산자: 각자 seq=0,1,2,.. 를 (p, seq)로 인코딩해 Enqueue
    std::vector<std::thread> producers;
    for (int p = 0; p < producerCount; ++p)
    {
        producers.emplace_back([&, p]()
        {
            uint64_t seq = 0;
            while (!stop.load(std::memory_order_relaxed))
            {
                // 큐를 얕게 유지: 미결 건수가 상한 이상이면 스핀 대기(잠들면 경합이 식음)
                while (outstanding.load(std::memory_order_relaxed) >= outstandingCap
                       && !stop.load(std::memory_order_relaxed))
                    YieldProcessor();
                if (stop.load(std::memory_order_relaxed)) break;

                uint64_t v = (uint64_t)p * SEQ_SPAN + seq;
                if (queue.Enqueue(v))
                {
                    outstanding.fetch_add(1, std::memory_order_relaxed);
                    ++seq;
                }
            }
            produced[p] = seq;
        });
    }

    // 단일 소비자: 관측 순서 = Dequeue 순서. 생산자별 연속성(strict +1) 검사
    std::vector<int64_t> lastSeq(producerCount, -1);
    std::thread consumer([&]()
    {
        uint64_t v;
        while (!stop.load(std::memory_order_relaxed))
        {
            if (!queue.Dequeue(&v))
                continue;
            outstanding.fetch_sub(1, std::memory_order_relaxed);

            int     p = (int)(v / SEQ_SPAN);
            int64_t s = (int64_t)(v % SEQ_SPAN);
            if (p < 0 || p >= producerCount)         // 인코딩 파손 방어
            {
                vProducer = p; vExpected = -777; vGot = s;
                violated = true; stop = true; break;
            }
            if (s != lastSeq[p] + 1)                 // ★ 생산자별 연속성 위반 = Q-E1
            {
                vProducer = p; vExpected = lastSeq[p] + 1; vGot = s;
                violated = true; stop = true; break;
            }
            lastSeq[p] = s;
            totalDeq.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // 시간 예산 + 1초 하트비트
    auto start = std::chrono::steady_clock::now();
    while (!stop.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(durationSeconds))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        std::cout << "  .. 경과 "
                  << std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now() - start).count()
                  << "초, dequeued=" << totalDeq.load()
                  << ", outstanding=" << outstanding.load() << std::endl;
    }
    stop = true;

    for (auto& t : producers) t.join();
    consumer.join();

    if (violated.load())
    {
        std::cout << "\n[REPRO] *** 생산자별 FIFO 위반 재현 - Q-E1 확정" << std::endl;
        std::cout << "        producer=" << vProducer
                  << ", expected_seq=" << vExpected
                  << ", got_seq=" << vGot << std::endl;
        std::cout << "        해석: 이 생산자의 이전 아이템(seq=" << vExpected
                  << ")이 큐 밖 노드에 매달려, 이후 아이템(seq=" << vGot
                  << ")이 먼저 관측됨." << std::endl;
        std::cout << "        위반 전 정상 dequeued=" << totalDeq.load() << std::endl;
        return true;
    }

    // 위반 없이 끝난 경우: 잔여 드레인하며 연속성·개수 보존까지 확인 (대조군 신뢰도)
    uint64_t v;
    while (queue.Dequeue(&v))
    {
        int     p = (int)(v / SEQ_SPAN);
        int64_t s = (int64_t)(v % SEQ_SPAN);
        if (p < 0 || p >= producerCount || s != lastSeq[p] + 1)
        {
            std::cout << "\n[REPRO] *** (드레인 중) 생산자별 FIFO 위반 - Q-E1 확정" << std::endl;
            std::cout << "        producer=" << p << ", expected_seq=" << (lastSeq[p] + 1)
                      << ", got_seq=" << s << std::endl;
            return true;
        }
        lastSeq[p] = s;
        totalDeq.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t producedTotal = 0;
    for (int p = 0; p < producerCount; ++p) producedTotal += produced[p];
    bool conserved = (totalDeq.load() == producedTotal);
    std::cout << "  > 위반 미검출. dequeued=" << totalDeq.load()
              << " / produced=" << producedTotal
              << (conserved ? "  (개수 보존 OK)" : "  ★개수 불일치(유실)!") << std::endl;
    return !conserved;   // 개수 불일치도 결함 신호
}

void Queue_Test_PerProducerFIFO()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "[Queue 2-4] 생산자별 FIFO(순서 보존) 반증 테스트" << std::endl;
    std::cout << "========================================" << std::endl;

    bool repro = RunQueuePerProducerFIFO(8, 64, 60);
    std::cout << "\n[RESULT] " << (repro ? "순서 위반/유실 검출됨 (결함 재현)"
                                         : "이번 실행에서는 위반 미검출") << std::endl;
    g_testCount++;
}


//=============================================================================
// 메뉴 출력
//=============================================================================
void PrintMenu()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "  LockFree 자료구조 통합 테스트 시스템" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  1. InternalFreeList 전체 테스트" << std::endl;
    std::cout << "  2. LockFreeStack 전체 테스트" << std::endl;
    std::cout << "  3. LockFreeQueue 전체 테스트" << std::endl;
    std::cout << "  4. 전체 통합 테스트 (FreeList + Stack + Queue)" << std::endl;
    std::cout << "  5. 보존 불변식 빠른 검증 (Stack + Queue, 약 1분)" << std::endl;
    std::cout << "  6. Queue 생산자별 FIFO 반증 (Q-E1, 약 1분)" << std::endl;
    std::cout << "  0. 종료" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "선택: ";
}

//=============================================================================
// Main
//=============================================================================
int main(int argc, char** argv)
{
    std::cout << "========================================" << std::endl;
    std::cout << "  LockFree 자료구조 통합 테스트 시스템" << std::endl;
    std::cout << "  목표: 100% 안전성 확보" << std::endl;
    std::cout << "========================================" << std::endl;

    // Headless 진입: 인자가 있으면 해당 테스트만 실행하고 종료 (메뉴/cls 없이 선형 출력)
    //   예) TestCode.exe repro [producers] [outstandingCap] [seconds]
    //   종료코드: 0=위반 미검출, 2=결함 재현, 1=인자 오류
    if (argc >= 2)
    {
        std::string mode = argv[1];
        if (mode == "repro")
        {
            int producers = (argc >= 3) ? std::atoi(argv[2]) : 8;
            int cap       = (argc >= 4) ? std::atoi(argv[3]) : 64;
            int secs      = (argc >= 5) ? std::atoi(argv[4]) : 60;
            bool r = RunQueuePerProducerFIFO(producers, cap, secs);
            std::cout << "\n[EXIT] repro " << (r ? "= 위반 검출(결함 재현)" : "= 위반 미검출") << std::endl;
            return r ? 2 : 0;
        }
        if (mode == "cons")
        {
            // 보존 불변식(허위 empty·HANG·유실/중복) 회귀 검증. 선형 출력(cls 없음).
            // 실패 시 내부에서 Crash()로 비정상 종료하므로, 여기 도달=PASS.
            Stack_Test_Conservation();
            Queue_Test_Conservation();
            std::cout << "\n[EXIT] cons = PASS (크래시 없이 완주)" << std::endl;
            return 0;
        }
        std::cout << "알 수 없는 모드: " << mode << std::endl;
        return 1;
    }

    while (true)
    {
        PrintMenu();

        int choice;
        std::cin >> choice;

        if (choice == 0)
        {
            std::cout << "\n테스트를 종료합니다." << std::endl;
            break;
        }

        auto totalStart = std::chrono::steady_clock::now();
        g_testCount = 0;

        try
        {
            switch (choice)
            {
            case 1:
                std::cout << "\n[InternalFreeList 전체 테스트]" << std::endl;
                FreeList_Test_DataIntegrity();
                FreeList_Test_Invariants();
                FreeList_Test_PlacementNew();
                FreeList_Test_Defense();
                FreeList_Test_MT_AllocFree();
                FreeList_Test_HighContention();
                break;

            case 2:
                std::cout << "\n[LockFreeStack 전체 테스트]" << std::endl;
                Stack_Test_DataIntegrity();
                Stack_Test_Defense();
                Stack_Test_MT_PushPop();
                Stack_Test_HighContention();
                Stack_Test_Conservation();
                break;

            case 3:
                std::cout << "\n[LockFreeQueue 전체 테스트]" << std::endl;
                Queue_Test_FIFOIntegrity();
                Queue_Test_Defense();
                Queue_Test_ProducerConsumer();
                Queue_Test_HighContention();
                Queue_Test_Conservation();
                break;

            case 4:
                std::cout << "\n[전체 통합 테스트]" << std::endl;
                FreeList_Test_DataIntegrity();
                FreeList_Test_Invariants();
                FreeList_Test_PlacementNew();
                FreeList_Test_Defense();
                FreeList_Test_MT_AllocFree();
                FreeList_Test_HighContention();
                Stack_Test_DataIntegrity();
                Stack_Test_Defense();
                Stack_Test_MT_PushPop();
                Stack_Test_HighContention();
                Stack_Test_Conservation();
                Queue_Test_FIFOIntegrity();
                Queue_Test_Defense();
                Queue_Test_ProducerConsumer();
                Queue_Test_HighContention();
                Queue_Test_Conservation();
                break;

            case 5:
                std::cout << "\n[보존 불변식 빠른 검증]" << std::endl;
                Stack_Test_Conservation();
                Queue_Test_Conservation();
                break;

            case 6:
                std::cout << "\n[Queue 생산자별 FIFO 반증]" << std::endl;
                Queue_Test_PerProducerFIFO();
                break;

            default:
                std::cout << "\n잘못된 선택입니다." << std::endl;
                continue;
            }
        }
        catch (...)
        {
            std::cout << "\n[EXCEPTION] 예외 발생!" << std::endl;
            continue;
        }

        auto totalEnd = std::chrono::steady_clock::now();
        auto totalElapsed = std::chrono::duration_cast<std::chrono::seconds>(totalEnd - totalStart).count();

        std::cout << "\n========================================" << std::endl;
        std::cout << "  테스트 완료!" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "  - 완료된 테스트: " << g_testCount << " 개" << std::endl;
        std::cout << "  - 총 소요 시간: " << totalElapsed << " 초 ("
            << totalElapsed / 60 << " 분)" << std::endl;
        std::cout << "  - 결과: 100% PASS" << std::endl;
        std::cout << "========================================" << std::endl;
    }

    return 0;
}
