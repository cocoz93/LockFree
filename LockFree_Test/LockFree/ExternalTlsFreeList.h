#pragma once

#ifndef ____EXTERNAL_TLS_FREE_LIST_H____
#define ____EXTERNAL_TLS_FREE_LIST_H____

#include "InternalFreeList.h"
#include <cassert>

namespace LockFree
{

inline volatile LONG64 g_Config = 0;

template<typename T>
class CExternalTlsFreeList
{
public:
	struct ChunkNODE;
	struct ChunkDATA
	{
		T Data;
		ChunkNODE* pMyChunkNode;
		LONG64 DataConfig;
	};

	// 목표 청크 크기(256KB)에 맞춰 sizeof(T) 기반으로 청크 원소 수를 컴파일 타임 산출
	static constexpr size_t TARGET_CHUNK_BYTES = 256 * 1024;
	static constexpr int    MIN_CHUNK_COUNT    = 100;
	static constexpr int    MAX_CHUNK_COUNT    = 2000;

	static constexpr int CalcChunkSize()
	{
		int count = static_cast<int>(TARGET_CHUNK_BYTES / sizeof(ChunkDATA));
		if (count < MIN_CHUNK_COUNT) return MIN_CHUNK_COUNT;
		if (count > MAX_CHUNK_COUNT) return MAX_CHUNK_COUNT;
		return count;
	}

	static constexpr int CHUNK_SIZE = CalcChunkSize();


	struct ChunkNODE
	{
		explicit ChunkNODE()
		{
			//FreeList안에서 최초할당시에만 생성자 호출됨
			Initialize();
		}
	public:
		void Initialize()
		{
			// 청크마다 유일한 식별자 부여. 멀티스레드 동시 생성 가능하므로 원자적 증가.
			this->Config = InterlockedIncrement64(&g_Config);

			for (int i = 0; i < CHUNK_SIZE; ++i)
			{
				this->DataArr[i].pMyChunkNode = this;
				this->DataArr[i].DataConfig = this->Config;
			}
		}

	public:
		// FreeCount는 multi-thread 접근 → 독립 캐시 라인 분리 (false sharing 방지)
		// NOTE: alignas(64)는 struct 내 오프셋만 보장. HeapAlloc은 16바이트 정렬까지만 지원하므로
		//       인스턴스 절대 주소의 64바이트 정렬은 미보장. 다만 ChunkNODE 크기가 수십~수백KB이므로
		//       인접 인스턴스의 FreeCount끼리 같은 캐시라인에 올 가능성은 물리적으로 없음.
		alignas(64) volatile SHORT FreeCount;
		// [false sharing 수정] DataArr에 alignas(64)를 줘 오프셋 64에서 시작시킨다. 이러면
		// FreeCount(offset 0, 2바이트)는 뒤따르는 62바이트 패딩과만 라인을 공유하고 DataArr[0]은
		// 다음 캐시라인으로 밀려난다. 이전에는 DataArr[0]이 FreeCount와 같은 라인이라, 다른 스레드의
		// Free가 InterlockedDecrement16으로 그 라인을 무효화할 때마다 슬롯0 소유 스레드의 DataArr[0]
		// 접근이 cross-core 캐시미스로 떨어졌다. (오프셋 차 64면 절대주소가 16정렬이어도 서로 다른 라인)
		alignas(64) ChunkDATA DataArr[CHUNK_SIZE];
		// AllocCount는 TLS 소유 스레드만 접근 → volatile 불필요, FreeCount와 별도 캐시 라인
		SHORT AllocCount;
		LONG64 Config;
		// 이 청크를 소유한 CExternalTlsFreeList(this) 신원. Free에서 크로스풀 오용 fail-fast용.
		// (Config/DataConfig 자기일관성은 전역 g_Config 기반이라 남의 풀을 못 걸러 → 인스턴스 신원으로 보강)
		LONG64 OwnerPoolId;

	};

	// 청크 자체를 담아두는 내부 프리리스트.
	// 세 번째 인자(UseApproxSize)를 켜서 "지금 유휴로 들어 있는 청크 수"를 조회 가능하게 한다.
	// 청크 단위 Alloc/Free는 슬롯 대비 극히 드물어(청크 하나가 CHUNK_SIZE개 슬롯을 덮는다)
	// 카운터 원자연산 비용은 사실상 0이고, 대신 청크 잔류량을 상시 관측할 수 있다.
	using ChunkPool = CInternalFreeList<ChunkNODE, false, true>;


public:
	// warmupChunkCount: 시작 시 미리 만들어둘 청크 수. 보통 동시 사용 스레드 수만큼 주면
	//                   각 스레드의 첫 Alloc이 HeapAlloc 없이 처리된다. 0이면 워밍업 생략(lazy).
	//
	// [수명 계약] 슬롯의 T는 청크가 만들어질 때 1회 생성되고 풀이 소멸할 때 1회 소멸된다.
	// Alloc/Free는 생성자·소멸자를 부르지 않는다 — 재사용이 이 풀의 존재 이유다.
	// 따라서 Alloc이 돌려주는 객체는 이전 사용자가 쓰던 상태 그대로이고, 초기화는 사용자 책임.
	//
	// 내부 풀(CInternalFreeList)의 PlacementNew=true처럼 Alloc/Free마다 생성·소멸시키는 모드는
	// 여기선 제공하지 않는다. 내부 풀은 NODE에 생성자가 없어 "HeapAlloc 후 명시적 placement new"가
	// 유일한 생성 경로라 플래그가 그 경로를 온전히 통제하지만, 여기는 ChunkNODE 생성자가
	// DataArr를 기본 초기화하면서 전 슬롯의 T를 이미 만들어 버린다. 즉 플래그가 통제하지 못하는
	// 생성 경로가 하나 더 있어, 켜면 슬롯마다 이중 생성 + 이중 소멸이 된다(자원 소유 타입이면 힙 손상).
	explicit CExternalTlsFreeList(int warmupChunkCount = 16)
	{
		// Config와 this(찾아갈주소)는 한번 박아놓으면 바뀔일 없으므로 청크 풀은 PlacementNew = false
		this->_ChunkFreeList = nullptr;

		this->TlsIndex = TLS_OUT_OF_INDEXES;
		this->_Initialized = false;
		this->_WarmupChunkCount = warmupChunkCount;

		Init();
	}

	~CExternalTlsFreeList()
	{
		if (this->TlsIndex != TLS_OUT_OF_INDEXES)
			TlsFree(this->TlsIndex);

		delete this->_ChunkFreeList;
	}

	bool Init()
	{
		if (this->_Initialized)
			return true;

		this->_ChunkFreeList = new ChunkPool;
		if (this->_ChunkFreeList == nullptr)
			return false;

		this->TlsIndex = TlsAlloc();
		if (this->TlsIndex == TLS_OUT_OF_INDEXES)
			return false;

		// 워밍업: 청크 N개를 미리 free list에 적재 (일괄 Alloc 후 일괄 Free해야 서로 다른 N개가 쌓임)
		if (this->_WarmupChunkCount > 0)
		{
			ChunkNODE** pWarmup = new(std::nothrow) ChunkNODE*[this->_WarmupChunkCount];
			if (pWarmup != nullptr)
			{
				for (int i = 0; i < this->_WarmupChunkCount; ++i)
					pWarmup[i] = this->_ChunkFreeList->Alloc();

				for (int i = 0; i < this->_WarmupChunkCount; ++i)
					this->_ChunkFreeList->Free(pWarmup[i]);	// nullptr는 Free 내부에서 무시됨

				delete[] pWarmup;
			}
		}

		this->_Initialized = true;
		return true;
	}


public:
	//DataAlloc
	T* Alloc()
	{
		if (this->_Initialized == false)
			return nullptr;

		//Tls->Map Debug
		ChunkNODE* pChunkNode = (ChunkNODE*)TlsGetValue(this->TlsIndex);

		//이미 Tls가 있는 경우
		if (pChunkNode != nullptr)
		{
			//청크가 남아있는 경우
			if (pChunkNode->AllocCount != 0)
				return (T*)(&pChunkNode->DataArr[pChunkNode->AllocCount--].Data);

			//마지막 인자 인 경우, 새청크를 SetValue해놓고 마지막인자를 반환해줌
			// if (pChunkNode->AllocCount == 0)

			TlsSetValue(this->TlsIndex, 0);

			return &(pChunkNode->DataArr[0].Data);
		}

		//  해당스레드에서 TlsGetValue()가 최초 호출된 경우 (또는 청크 소진 후)
		pChunkNode = this->_ChunkFreeList->Alloc();
		if (pChunkNode == nullptr)
			return nullptr;

		// 이 청크는 내(this) 풀 소유임을 표식. 청크는 한 풀 안에서만 재활용되므로 뽑을 때 한 번만 찍으면 된다.
		// (사용자에게 나가는 모든 슬롯은 반드시 이 경로를 거친다)
		pChunkNode->OwnerPoolId = reinterpret_cast<LONG64>(this);

		pChunkNode->AllocCount = CHUNK_SIZE - 2; //(반환할거 포함 마이너스)
		pChunkNode->FreeCount = CHUNK_SIZE;

		TlsSetValue(this->TlsIndex, pChunkNode);

		return &(pChunkNode->DataArr[CHUNK_SIZE - 1].Data);
	}

	bool Free(volatile T* Data)
	{
		ChunkNODE* pChunkNode = ((ChunkDATA*)Data)->pMyChunkNode;

		// 크로스풀/오염 포인터는 프리리스트를 조용히 망가뜨려 원인에서 먼 곳에서 죽는다.
		// release에서도 원인 지점에서 즉시 fail-fast로 터뜨린다 (와일드 포인터/double-free는 못 잡음).
		//  - 슬롯 자기일관성: 이 풀 슬롯이면 DataConfig == 소속 청크 Config
		if (((ChunkDATA*)Data)->DataConfig != pChunkNode->Config)
			__fastfail(FAST_FAIL_INVALID_ARG);
		//  - 풀 소유권: 이 청크가 내(this) 풀 소유인지 (전역 g_Config 자기참조로는 못 걸르던 구간)
		if (pChunkNode->OwnerPoolId != reinterpret_cast<LONG64>(this))
			__fastfail(FAST_FAIL_INVALID_ARG);

		// 슬롯 T의 소멸자는 여기서 부르지 않는다 — 풀이 소멸할 때 1회만 부른다(위 수명 계약).

		// 청크 Free카운트를 감소시키고, 모두 반납된경우 프리리스트로 반납한다.
		if (0 == InterlockedDecrement16(&pChunkNode->FreeCount))
		{
			// 프리리스트 반환
			this->_ChunkFreeList->Free(pChunkNode);
		}
		return true;
	}

	// 진단/모니터링용: 지금까지 HeapAlloc된 청크 총수(단조 증가). 스레드가 청크를 소진하지
	// 못하고 종료하면 그 청크는 회수·재사용되지 못하므로, 그런 상황이 쌓이면 이 값이 계속 증가한다.
	INT64 GetChunkAllocCount() const { return _ChunkFreeList->GetAllocCount(); }

	// 진단/모니터링용: 지금 내부 프리리스트에 유휴로 들어 있는(회수 끝난) 청크 수.
	// GetChunkAllocCount() - GetChunkIdleCount() = 지금 스레드들이 붙잡고 있는 청크 수(잔류량).
	// 이 잔류량이 스레드 수 근처에서 안정되면 청크가 정상 회수되는 것이고,
	// 계속 늘어나면 부분 반납 슬롯이 청크를 묶어두고 있다는 뜻이다.
	INT64 GetChunkIdleCount() const { return _ChunkFreeList->GetFreeListSize(); }

private:
	ChunkPool* _ChunkFreeList;

private:
	int TlsIndex;
	bool _Initialized;
	int _WarmupChunkCount;		// 시작 시 미리 만들어둘 청크 수 (보통 동시 사용 스레드 수)
};

}

#endif //____EXTERNAL_TLS_FREE_LIST_H____
