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

	// PlacementNew 인자는 쓰이지 않는다(아래 _pFreeList 주석 — Tag 보존 때문에 내부
	// 프리리스트를 false로 못 박았다). 받아만 두면 켠 줄 알므로 컴파일 단계에서 막는다.
	static_assert(!PlacementNew,
		"CLockFreeQueue<T, PlacementNew>: PlacementNew는 지원하지 않는다 (노드 Tag 보존을 위해 "
		"내부 프리리스트가 false로 고정됨). 인자를 빼거나 false로 둘 것");

	//-----------------------------------------------------
	struct NODE;

	// 노드의 next를 (포인터+태그) 128비트 counted pointer로 만들어 링크를 DCAS로 설치한다.
	// 노드가 재활용될 때마다(Enqueue 재사용 초기화) Tag가 단조 증가하므로, 낡은
	// (null, 옛태그) 스냅샷을 든 Enqueue의 링크 DCAS는 반드시 실패한다 → null-ABA(Q-E1) 차단.
	// cmpxchg16b 요건: &NODE::Next가 16바이트 정렬이어야 함(아래 alignas(16)로 보장).
	//
	// 두 필드는 여러 스레드가 동시에 읽고 쓰는 공유 상태라 volatile로 선언한다.
	// 이 파일은 Next를 128비트 원자 로드로 읽지 않고 두 필드를 따로 읽으므로, "태그를 값보다
	// 먼저" 라는 접근 순서 자체가 정확성의 전제다. volatile이 그 순서를 못 박는다.
	// (평문으로 두면 MSVC가 두 로드를 뒤집지는 않았지만 인접 문장이 그 사이로 끼어든다.
	//  즉 "지금 우연히 맞는 코드"를 "규칙상 항상 맞는 코드"로 만드는 값이고, 비용은 명령 2개다)
	struct NextRef
	{
		NODE* volatile pNode;   // low  64
		volatile INT64 Tag;     // high 64
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

	// initialCapacity: 노드를 미리 만들어둘 개수(프리리스트로 전달). 생성자가 이미 Init()을
	//   부르므로, 나중에 Init(n)을 다시 부르면 초기화는 건너뛰고 n개 사전 적재만 수행한다.
	bool Init(int initialCapacity = 0)
	{
		if (_Initialized)
			return _pFreeList->Init(initialCapacity);

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
		return _pFreeList->Init(initialCapacity);
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

		if (_Initialized == false)		// 초기화됐다면 _phead는 반드시 non-null (Init 실패 경로가 되돌려 놓는다)
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
		// 재사용 초기화. 순서가 정확성의 일부다 — Tag를 "먼저" 올려 낡은 링크 스냅샷을
		// 무효화한 뒤 next를 비운다. 반대로 하면 {null, 옛Tag}라는 과도상태가 잠깐 노출되고,
		// 그 조합을 스냅샷한 낡은 Enqueue의 링크 DCAS가 아직 큐에 들어가지도 않은 이 사설
		// 노드에 성공한다(Enqueue는 true를 반환하는데 원소는 큐에서 도달 불가).
		// 위 순서에서 나오는 과도상태는 {옛next, 새Tag}인데, 낡은 DCAS의 expected는 항상
		// pNode==null이라 절대 일치하지 않는다. (Tag가 volatile이라 순서가 뒤집히지 않는다)
		pnNode->Next.Tag  += 1;
		// [증폭 C] 두 store 사이 — 위에서 말한 과도상태 창이 열리는 자리.
		// 순서가 올바르면 여기서 아무리 오래 멈춰도 낡은 DCAS가 걸릴 조합이 없어야 한다.
		LF_RACE_HOOK_ENQ();
		pnNode->Next.pNode = nullptr;

		// [증폭 C2] 새 노드가 next=null인 private 상태로 머무는 창을 넓힌다.
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

			// 스냅샷이 찢겼으면 조기 탈출. 정확성은 아래 재검증과 DCAS가 책임지므로 선택적이고,
			// 성능 이득도 측정 한계 아래다(옛 주석의 "20-40 cycles 절감"은 실측으로 반증됨).
			if (bTopTailNode.UniqueCount != this->_ptail->UniqueCount)
				continue;

			// [증폭 D] tail 스냅샷 ~ next 읽기 창을 넓힌다. 이 사이 스냅샷 노드가 Dequeue로 Free·
			// 재활용되는 상황을 강제해, 아래 "next 읽은 뒤 tail 재검증 + counted-next DCAS"가
			// 재활용을 제대로 걸러내는지 스트레스한다. (수정 전 null-ABA가 터지던 바로 그 창 — Q-E1)
			LF_RACE_HOOK_ENQ();

			//tail의 Next백업 — 태그를 "먼저" 읽는다(128비트 원자 로드를 안 쓰는 대신).
			// Tag가 [여기 ~ 아래 링크 DCAS] 내내 불변이면 그 사이 링크도 재활용도 없었다는
			// 뜻이므로(둘 다 Tag를 올린다), 뒤이어 읽은 next가 그 태그의 짝임이 보장된다.
			// 순서를 뒤집으면 (옛 pNode=null, 새 Tag)라는 실존한 적 없는 조합이 만들어지고,
			// 그게 재활용 노드의 상태와 우연히 일치하면 링크 DCAS가 잘못 성공한다.
			INT64 bTailNextTag = bTopTailNode.pNode->Next.Tag;
			// [증폭 E] 두 로드 사이 — 이 사이 다른 스레드가 링크를 걸어도 위 논리가 성립해야 한다.
			LF_RACE_HOOK_ENQ();
			pbTailNextNode     = bTopTailNode.pNode->Next.pNode;

			// [Q-E1 수정 핵심] next를 읽은 뒤 tail 스냅샷 재검증 (MS 원본의 tail==Q.tail).
			// 스냅샷 tail 노드가 그새 Dequeue·Free·재활용됐다면 tail 태그(UniqueCount)가 이미
			// 바뀌어 여기서 걸러진다(낡은 tail 노드 감지). 이 재검증만으로는 재검증~CAS 사이
			// 잔여 창이 남지만, 아래 링크가 counted-next DCAS라 그 창의 재활용도 next Tag로
			// 감지된다. 즉 null-ABA(Q-E1)를 닫는 것은 —
			//   (1) 재사용 초기화의 Tag 선행  (2) 여기 tail 재검증  (3) counted-next DCAS.
			// 실제로 짐을 지는 것은 (2)와 (3)이고, 둘 중 하나만 빠져도 뚫린다. (1)은 지금
			// 구조에선 중복이다 — 노드가 Free되려면 반드시 링크를 거치고 링크가 Tag를 올리므로
			// 낡은 스냅샷의 Tag는 재활용 시점에 이미 지나가 있어 (3)이 걸러낸다.
			// 그래도 명령 1개라 남겨둔다: 스냅샷을 Tag 선행으로 읽지 않게 바뀌면 (1)이 유일한
			// 방어가 된다.
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
				// [증폭 F] 재검증 통과 ~ 링크 DCAS 사이 잔여 창. 이 사이 스냅샷 tail 노드가
				// Free→재활용되면 counted-next(Tag 비교)가 마지막 방어선이다(위 (3)).
				// 이 훅이 없으면 이 창이 수 ns라, (3)을 무력화한 회귀를 증폭 빌드가 못 잡는다.
				LF_RACE_HOOK_ENQ();
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

			// 스냅샷이 찢겼으면 조기 탈출. 정확성은 아래 재검증과 DCAS가 책임지므로 선택적이고,
			// 성능 이득도 측정 한계 아래다(옛 주석의 "20-40 cycles 절감"은 실측으로 반증됨).
			if (bTopHeadNode.UniqueCount != this->_phead->UniqueCount)
				continue;

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