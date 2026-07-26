#pragma once


#ifndef ____LOCKFREE_QUEUE_H____
#define ____LOCKFREE_QUEUE_H____

#include <atomic>
#include <type_traits>
#include "InternalFreeList.h"

// 경합 창 증폭 지점 (검증 훅)
// 평소 빌드에서는 빈 매크로라 코드에서 완전히 사라진다(비용 0).
// 테스트 빌드가 이 매크로를 "확률적 지연"으로 재정의하면 스냅샷 읽기 사이의
// 나노초 틈이 마이크로초로 벌어져, OS 선점으로만 드물게 터지던 경합이
// 수천 배 빨리 재현된다. 락프리는 어느 지점에서 얼마나 멈춰도 옳아야 하므로
// 지연을 넣어 깨진다면 원래 있던 결함이다. (TestCode.cpp의 USE_RACE_HOOK 참고)
#ifndef LF_RACE_HOOK
#define LF_RACE_HOOK()
#endif

// Enqueue 전용 창 증폭 훅. Enqueue의 결함(재활용된 tail 노드에 낡은 스냅샷이 링크)은
// tail 노드가 Free→재활용되기까지 수 µs가 걸려야 열리므로, 일반 LF_RACE_HOOK(수백 ns)보다
// 더 길게 멈추도록 테스트가 별도로 재정의한다. 평소 빌드에선 빈 매크로라 비용 0.
#ifndef LF_RACE_HOOK_ENQ
#define LF_RACE_HOOK_ENQ()
#endif

namespace LockFree
{

template<typename T, bool PlacementNew = false, bool UseApproxSize = false>
class CLockFreeQueue
{
	// Dequeue는 DCAS 승리 전에 Data를 복사한다(패배 시 폐기 — 재활용 중인 노드를 읽을 수 있음).
	// 찢긴/미구성 메모리를 복사해도 안전해야 하므로 trivially copyable T만 허용.
	static_assert(std::is_trivially_copyable_v<T>,
		"CLockFreeQueue<T>: T must be trivially copyable");

	//-----------------------------------------------------
	struct NODE;

	// 노드의 next를 (포인터+태그) 128비트 counted pointer로 만들어 링크를 DCAS로 설치한다.
	// 노드가 재활용될 때마다(Enqueue 재사용 초기화) Tag가 단조 증가하므로, 낡은
	// (null, 옛태그) 스냅샷을 든 Enqueue의 링크 DCAS는 반드시 실패한다 → null-ABA(Q-E1) 차단.
	// cmpxchg16b 요건: &NODE::Next가 16바이트 정렬이어야 함(아래 alignas(16)로 보장).
	struct NextRef
	{
		NODE* pNode;   // low  64
		INT64 Tag;     // high 64
	};

	struct NODE
	{
		// Tag는 물리 노드의 재사용 전체에 걸쳐 단조 증가해야 하므로, 최초 HeapAlloc 시
		// 1회만 {null,0}으로 초기화하고(멤버 초기자) 재사용에선 보존한다. (프리리스트를
		// PlacementNew=false로 고정해 Alloc마다 재구성으로 리셋되는 것을 막는다 — _pFreeList 참고)
		alignas(16) NextRef Next{ nullptr, 0 };
		T Data;
	};

	struct TopNODE
	{
		NODE* pNode;
		INT64 UniqueCount;
	};

	// Intel oneTBB atomic_backoff 방식: Spin(pause 지수증가) → Yield(SwitchToThread)
	struct CASBackoff
	{
		static constexpr int LOOPS_BEFORE_YIELD = 32;
		int _count = 1;

		__forceinline void Pause()
		{
			if (_count <= LOOPS_BEFORE_YIELD)
			{
				for (int i = 0; i < _count; ++i)
					YieldProcessor();
				_count <<= 1;
			}
			else
			{
				SwitchToThread();
			}
		}
	};
	//-----------------------------------------------------



public:
	//최초 더미생성
	explicit CLockFreeQueue()
	{
		_pFreeList = nullptr;
		_phead = nullptr;
		_ptail = nullptr;
		_Initialized = false;
		if constexpr (UseApproxSize)
			_UseSize = 0;

		Init();
	}

	CLockFreeQueue(const CLockFreeQueue&) = delete;
	CLockFreeQueue& operator=(const CLockFreeQueue&) = delete;
	CLockFreeQueue(CLockFreeQueue&&) = delete;
	CLockFreeQueue& operator=(CLockFreeQueue&&) = delete;

	bool Init()
	{
		if (_Initialized)
			return true;

		_pFreeList = new(std::nothrow) CInternalFreeList<NODE, false>();
		if (_pFreeList == nullptr)
			return false;

		_phead = (TopNODE*)_aligned_malloc(64, 64);
		if (_phead == nullptr)
		{
			delete _pFreeList;
			_pFreeList = nullptr;
			return false;
		}

		_ptail = (TopNODE*)_aligned_malloc(64, 64);
		if (_ptail == nullptr)
		{
			_aligned_free((void*)_phead);
			_phead = nullptr;
			delete _pFreeList;
			_pFreeList = nullptr;
			return false;
		}

		NODE* pDummy = _pFreeList->Alloc();
		if (pDummy == nullptr)
		{
			_aligned_free((void*)_ptail);
			_ptail = nullptr;
			_aligned_free((void*)_phead);
			_phead = nullptr;
			delete _pFreeList;
			_pFreeList = nullptr;
			return false;
		}
		pDummy->Next.pNode = nullptr;			// Tag는 멤버 초기자로 0 (최초 alloc)

		// cmpxchg16b는 대상이 16바이트 정렬이어야 함. 프리리스트 노드가 그 정렬을
		// 만족하는지 1회 확인(디버그). 안 맞으면 링크 DCAS가 #GP로 크래시하므로 조기 검출.
		assert((reinterpret_cast<UINT_PTR>(&pDummy->Next) & 15) == 0
			&& "NODE.Next가 16바이트 정렬이 아님 (InterlockedCompareExchange128 요건)");

		_phead->pNode = pDummy;
		_phead->UniqueCount = 0;

		_ptail->pNode = pDummy;
		_ptail->UniqueCount = 0;

		if constexpr (UseApproxSize)
			_UseSize = 0;

		_Initialized = true;
		return true;
	}

	~CLockFreeQueue()
	{
		if (_Initialized == false)
			return;

		Clear();

		_pFreeList->Free(this->_phead->pNode);

		_aligned_free((void*)this->_ptail);
		_aligned_free((void*)this->_phead);

		delete _pFreeList;
	}

	void Clear(void)
	{
		if (_Initialized == false)
			return;

		//모든 노드 삭제
		NODE* pfNode = nullptr;

		while (this->_phead->pNode->Next.pNode != nullptr)
		{
			pfNode = this->_phead->pNode->Next.pNode;
			this->_phead->pNode->Next.pNode = pfNode->Next.pNode;
			_pFreeList->Free(pfNode);
		}

		_phead->UniqueCount = 0;
		_ptail->UniqueCount = 0;
		_ptail->pNode = _phead->pNode;

		if constexpr (UseApproxSize)
			_UseSize = 0;
	}

	// 락프리 특성상 정확한 사이즈 보장 불가 (관측용 대략값)
	bool IsEmpty(void)
	{
		if constexpr (UseApproxSize)
			return (_UseSize == 0);

		if (_Initialized == false || _phead == nullptr)
			return true;

		return (_phead->pNode->Next.pNode == nullptr);
	}

	// 모니터링/디버깅용 대략 사이즈
	INT64 GetApproxSize(void) const
	{
		if constexpr (UseApproxSize)
			return _UseSize;

		return 0;
	}


	bool Enqueue(const T& Data)
	{
		TopNODE bTopTailNode;						// backup TailTopNode;
		NODE* pbTailNextNode;						// backupTailNext Node;
		NODE* pnNode = this->_pFreeList->Alloc();	// NewNode;
		if (nullptr == pnNode)
			return false;

		pnNode->Data = Data;
		// 재사용 초기화: next=null, Tag는 이전 값 +1로 단조 증가시켜 낡은 링크 스냅샷을 무효화.
		// (pnNode는 아직 private. Tag는 최초 alloc 시 멤버 초기자 0, 재사용마다 여기서 +1.
		//  이 노드에 대한 stale DCAS는 Tag가 이미 앞서 있어 모두 실패하므로 이 평문 쓰기는 안전)
		pnNode->Next.pNode = nullptr;
		pnNode->Next.Tag  += 1;

		// [증폭 C] 새 노드가 next=null인 private 상태로 머무는 창을 넓힌다.
		// 이 노드가 직전에 Free→재활용된 tail 노드라면 낡은 스냅샷을 든 Enqueue가 노릴 수 있는
		// 구간 — 이제 아래 재검증+counted-next가 방어하므로, 이 훅은 그 방어를 스트레스한다(Q-E1).
		LF_RACE_HOOK_ENQ();

		CASBackoff backoff;

		// 노드가 추가되면 Enqueue성공 간주. tail밀기 실패는 상관X
		while (true)
		{
			// tail백업
			bTopTailNode.UniqueCount = this->_ptail->UniqueCount;
			bTopTailNode.pNode = this->_ptail->pNode;

			// stale 감지 시 역참조+CAS 회피 (~20-40 cycles 절감)
			if (bTopTailNode.UniqueCount != this->_ptail->UniqueCount)
				continue;

			_mm_prefetch((const char*)bTopTailNode.pNode, _MM_HINT_T0);

			// [증폭 D] tail 스냅샷 ~ next 읽기 창을 넓힌다. 이 사이 스냅샷 노드가 Dequeue로 Free·
			// 재활용되는 상황을 강제해, 아래 "next 읽은 뒤 tail 재검증 + counted-next DCAS"가
			// 재활용을 제대로 걸러내는지 스트레스한다. (수정 전 null-ABA가 터지던 바로 그 창 — Q-E1)
			LF_RACE_HOOK_ENQ();

			//tail의 Next백업 (pNode + Tag 스냅샷)
			pbTailNextNode = bTopTailNode.pNode->Next.pNode;
			INT64 bTailNextTag = bTopTailNode.pNode->Next.Tag;

			// [Q-E1 수정 핵심] next를 읽은 뒤 tail 스냅샷 재검증 (MS 원본의 tail==Q.tail).
			// 스냅샷 tail 노드가 그새 Dequeue·Free·재활용됐다면 tail 태그(UniqueCount)가 이미
			// 바뀌어 여기서 걸러진다(낡은 tail 노드 감지). 이 재검증만으로는 재검증~CAS 사이
			// 잔여 창이 남지만, 아래 링크가 counted-next DCAS라 그 창의 재활용도 next Tag로
			// 감지된다 → 둘이 함께 null-ABA(Q-E1)를 완전히 닫는다.
			if (bTopTailNode.UniqueCount != this->_ptail->UniqueCount)
				continue;

			//_______________________________________________________________________________________
			//
			//	tail뒤에 노드가 존재하는 경우 - 밀어준다.
			//_______________________________________________________________________________________
			if (nullptr != pbTailNextNode)
			{
				// tail 워드 태그 = 관측값+1. 설치가 이 CAS로 직렬화되어 (노드,태그) 쌍이 재발 안 함.
				InterlockedCompareExchange128
				(
					(volatile INT64*)_ptail,
					(INT64)(bTopTailNode.UniqueCount + 1),
					(INT64)pbTailNextNode,
					(INT64*)&bTopTailNode
				);
				continue;
			}
			//_______________________________________________________________________________________

			//_______________________________________________________________________________________
			//
			//	Enqueue시도: tail 노드의 next를 (null, bTailNextTag) → (pnNode, bTailNextTag+1)로 DCAS.
			//	재활용된 노드면 Tag가 이미 증가해 이 DCAS가 실패 → 재시도 (null-ABA / Q-E1 차단).
			//_______________________________________________________________________________________
			else
			{
				NextRef expected;
				expected.pNode = nullptr;			// low  = 관측한 null
				expected.Tag   = bTailNextTag;		// high = 관측한 태그
				if (InterlockedCompareExchange128
				(
					(volatile INT64*)&bTopTailNode.pNode->Next,
					(INT64)(bTailNextTag + 1),		// ExchangeHigh : 새 태그
					(INT64)pnNode,					// ExchangeLow  : 새 next 포인터
					(INT64*)&expected
				))
				{
					// Enqueue 성공 — tail 밀어준다 (성공여부 판단x)
					InterlockedCompareExchange128
					(
						(volatile INT64*)_ptail,
						(INT64)(bTopTailNode.UniqueCount + 1),
						(INT64)pnNode,
						(INT64*)&bTopTailNode
					);
					break;
				}

				backoff.Pause();
			}
			//_______________________________________________________________________________________
		}

		if constexpr (UseApproxSize)
			InterlockedIncrement64(&this->_UseSize);
		return true;
	}


	bool Dequeue(T* pOutData)
	{
		TopNODE	 bTopHeadNode;
		TopNODE	 bTopTailNode;
		NODE* bHeadNextNode;
		CASBackoff backoff;

		while (true)
		{
			//_______________________________________________________________________________________
			// 
			//	head를 먼저 읽어 빈 큐 판별 — tail 캐시라인 접근 회피 (fast path)
			//_______________________________________________________________________________________

			// head 백업
			bTopHeadNode.UniqueCount = this->_phead->UniqueCount;
			bTopHeadNode.pNode = this->_phead->pNode;

			// stale 감지 시 역참조+CAS 회피 (~20-40 cycles 절감)
			if (bTopHeadNode.UniqueCount != this->_phead->UniqueCount)
				continue;

			_mm_prefetch((const char*)bTopHeadNode.pNode, _MM_HINT_T0);

			LF_RACE_HOOK();		// [증폭 A] head 스냅샷 ~ next 읽기 사이 창 (D2 재검증이 지키는 구간)

			bHeadNextNode = bTopHeadNode.pNode->Next.pNode;

			LF_RACE_HOOK();		// [증폭 B] next 읽기 ~ tail 접근/CAS 사이 창 (D1 재검증이 지키는 구간)

			// 큐가 비어있으면 tail 읽기 없이 즉시 반환 (호출자 책임)
			if (bHeadNextNode == nullptr)
			{
				// [D2 수정] next==null 읽은 뒤에도 head 스냅샷이 낡았을 수 있음(재활용된 head면
				//           데이터 있는데 empty 오판). head 태그 재확인 후에만 empty 확정.
				if (bTopHeadNode.UniqueCount != this->_phead->UniqueCount)
					continue;
				return false;
			}

			//_______________________________________________________________________________________
			// 
			//	head==tail 판별을 위해 tail 읽기 — 필요한 경우에만 접근
			//_______________________________________________________________________________________

			// tail백업
			bTopTailNode.UniqueCount = this->_ptail->UniqueCount;
			bTopTailNode.pNode = this->_ptail->pNode;

			// [D1 수정] tail 읽은 뒤 head 스냅샷 재검증. 낡은 head가 재활용돼 tail 자리로
			//           돌아오면 낡은 next를 tail에 심어 큐 오염(전역 HANG). (MS 원본 D6 단계)
			if (bTopHeadNode.UniqueCount != this->_phead->UniqueCount)
				continue;

			// head==tail: Enqueue 직후 tail이 안 밀린 상태 — tail push 후 재시도 (댕글링 방지)
			if (bTopHeadNode.pNode == bTopTailNode.pNode)
			{
				InterlockedCompareExchange128
				(
					(volatile INT64*)_ptail,
					(INT64)(bTopTailNode.UniqueCount + 1),
					(INT64)bHeadNextNode,
					(INT64*)&bTopTailNode
				);
				continue;
			}
			//_______________________________________________________________________________________

			//_______________________________________________________________________________________
			// 
			//	Dequeue (head != tail 확정)
			//_______________________________________________________________________________________

			_mm_prefetch((const char*)bHeadNextNode, _MM_HINT_T0);

			// 복사는 DCAS 승리 전에 해야 한다(승리 후엔 이 노드가 새 더미가 되어
			// 다른 스레드가 곧바로 Dequeue·Free·재활용할 수 있음). 다만 곧장 *pOutData에
			// 쓰면, DCAS 실패 → 재시도 → 빈 큐로 false를 반환하는 경로에서 호출자 버퍼가
			// 이미 오염된 채 남는다. 지역 임시에 담아두고 승리한 뒤에만 넘긴다.
			T copied = bHeadNextNode->Data;

			// 태그 = 관측값+1 (지역) — Enqueue tail 태그와 동일 원리
			if (false == InterlockedCompareExchange128
			(
				(volatile INT64*)this->_phead,
				(INT64)(bTopHeadNode.UniqueCount + 1),
				(INT64)bHeadNextNode,
				(INT64*)&bTopHeadNode
			))
			{
				// DCAS 실패
				backoff.Pause();
				continue;
			}
			else
			{
				// DCAS 성공 — 이 시점부터 노드 소유가 확정되므로 호출자에게 넘긴다
				*pOutData = copied;
				break;
			}
			//____________________________________________________________________
		}

		// CAS128()가 Comp쪽으로 뱉어준 원래노드를 해제
		this->_pFreeList->Free(bTopHeadNode.pNode);
		if constexpr (UseApproxSize)
			InterlockedDecrement64(&this->_UseSize);

		return true;
	}
private:
	// PlacementNew는 항상 false로 고정한다: NODE.Next.Tag가 재사용마다 단조 증가해야 하는데,
	// PlacementNew=true면 프리리스트가 Alloc마다 노드를 재구성(placement new)해 Tag를 0으로
	// 리셋 → null-ABA 재발. T는 trivially copyable이라 재구성은 어차피 무의미하므로 false로
	// 고정해도 관측 동작은 동일하다. (그래서 클래스의 PlacementNew 템플릿 인자는 여기서 미사용)
	CInternalFreeList<NODE, false>* _pFreeList;
	volatile TopNODE* _phead;
	volatile TopNODE* _ptail;
	bool _Initialized;
	alignas(64) volatile INT64 _UseSize;
};

}


#endif