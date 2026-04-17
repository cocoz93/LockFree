#pragma once


#ifndef ____LOCKFREE_QUEUE_H____
#define ____LOCKFREE_QUEUE_H____

#include <atomic>
#include "PoolFreeList.h"

namespace LockFree
{

template<typename T, bool PlacementNew = false, bool UseApproxSize = false>
class CLockFreeQ
{
	//-----------------------------------------------------
	struct NODE
	{
		NODE* pNextNode;
		T Data;
	};

	struct TopNODE
	{
		NODE* pNode;
		INT64 UniqueCount;
	};
	//-----------------------------------------------------



public:
	//최초 더미생성
	explicit CLockFreeQ()
	{
		_pFreeList = nullptr;
		_phead = nullptr;
		_ptail = nullptr;
		_Initialized = false;
		if constexpr (UseApproxSize)
			_UseSize.store(0, std::memory_order_relaxed);
		_HeadUniqueCount = 0;
		_TailUniqueCount = 0;

		Init();
	}

	CLockFreeQ(const CLockFreeQ&) = delete;
	CLockFreeQ& operator=(const CLockFreeQ&) = delete;
	CLockFreeQ(CLockFreeQ&&) = delete;
	CLockFreeQ& operator=(CLockFreeQ&&) = delete;

	bool Init()
	{
		if (_Initialized)
			return true;

		_pFreeList = new(std::nothrow) CFreeList<NODE, PlacementNew>();
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
		pDummy->pNextNode = nullptr;

		_phead->pNode = pDummy;
		_phead->UniqueCount = 0;

		_ptail->pNode = pDummy;
		_ptail->UniqueCount = 0;

		if constexpr (UseApproxSize)
			_UseSize.store(0, std::memory_order_relaxed);
		_HeadUniqueCount = 0;
		_TailUniqueCount = 0;

		_Initialized = true;
		return true;
	}

	~CLockFreeQ()
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

		while (this->_phead->pNode->pNextNode != nullptr)
		{
			pfNode = this->_phead->pNode->pNextNode;
			this->_phead->pNode->pNextNode = this->_phead->pNode->pNextNode->pNextNode;
			_pFreeList->Free(pfNode);
		}

		_phead->UniqueCount = 0;
		_ptail->UniqueCount = 0;
		_ptail->pNode = _phead->pNode;

		if constexpr (UseApproxSize)
			_UseSize.store(0, std::memory_order_relaxed);
		_HeadUniqueCount = 0;
		_TailUniqueCount = 0;
	}

	// 락프리 특성상 정확한 사이즈 보장 불가 (관측용 대략값)
	bool IsEmpty(void)
	{
		if constexpr (UseApproxSize)
			return (_UseSize.load(std::memory_order_relaxed) == 0);

		if (_Initialized == false || _phead == nullptr)
			return true;

		return (_phead->pNode->pNextNode == nullptr);
	}

	// 모니터링/디버깅용 대략 사이즈
	INT64 ApproxSize(void) const
	{
		if constexpr (UseApproxSize)
			return _UseSize.load(std::memory_order_relaxed);

		return 0;
	}


	bool Enqueue(T Data)
	{
		TopNODE bTopTailNode;						// backup TailTopNode;
		NODE* pbTailNextNode;						// backupTailNext Node;
		NODE* pnNode = this->_pFreeList->Alloc();	// NewNode;

		pnNode->Data = Data;
		pnNode->pNextNode = nullptr;				// Enqueue는 pNext가 nullptr일 경우에만 

		INT64 lTailUniqueCount = InterlockedIncrement64(&this->_TailUniqueCount);
		int backoff = 1;

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

			//tail의 Next백업
			pbTailNextNode = bTopTailNode.pNode->pNextNode;

			//_______________________________________________________________________________________
			// 
			//	tail뒤에 노드가 존재하는 경우 - 밀어준다.
			//_______________________________________________________________________________________
			if (nullptr != pbTailNextNode)
			{
				lTailUniqueCount = InterlockedIncrement64(&this->_TailUniqueCount);

				InterlockedCompareExchange128
				(
					(volatile INT64*)_ptail,
					(INT64)lTailUniqueCount,
					(INT64)pbTailNextNode,
					(INT64*)&bTopTailNode
				);
				continue;
			}
			//_______________________________________________________________________________________

			//_______________________________________________________________________________________
			// 
			//	Enqueue시도 (CAS)
			//_______________________________________________________________________________________
			else
			{
				if (nullptr == InterlockedCompareExchangePointer
				(
					(volatile PVOID*)&bTopTailNode.pNode->pNextNode,
					(PVOID)pnNode,
					(PVOID)pbTailNextNode
				))
				{
					// Enqueue 성공 
					// tail 밀어준다 (성공여부 판단x)
					InterlockedCompareExchange128
					(
						(volatile INT64*)_ptail,
						(INT64)lTailUniqueCount,
						(INT64)pnNode,
						(INT64*)&bTopTailNode
					);
					break;
				}

				for (int i = 0; i < backoff; ++i)
					YieldProcessor();
				if (backoff < 16)
					backoff <<= 1;
			}
			//_______________________________________________________________________________________
		}

		if constexpr (UseApproxSize)
			this->_UseSize.fetch_add(1, std::memory_order_relaxed);
		return true;
	}


	bool Dequeue(T* pOutData)
	{
		INT64 lHeadUniqueCount = InterlockedIncrement64(&this->_HeadUniqueCount);
		INT64 lTailUniqueCount;

		TopNODE	 bTopHeadNode;
		TopNODE	 bTopTailNode;
		NODE* bHeadNextNode;
		int backoff = 1;

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

			bHeadNextNode = bTopHeadNode.pNode->pNextNode;

			// 큐가 비어있으면 tail 읽기 없이 즉시 반환 (호출자 책임)
			if (bHeadNextNode == nullptr)
				return false;

			//_______________________________________________________________________________________
			// 
			//	head==tail 판별을 위해 tail 읽기 — 필요한 경우에만 접근
			//_______________________________________________________________________________________

			// tail백업
			bTopTailNode.UniqueCount = this->_ptail->UniqueCount;
			bTopTailNode.pNode = this->_ptail->pNode;

			// head==tail: Enqueue 직후 tail이 안 밀린 상태 — tail push 후 재시도 (댕글링 방지)
			if (bTopHeadNode.pNode == bTopTailNode.pNode)
			{
				lTailUniqueCount = InterlockedIncrement64((volatile INT64*)&this->_TailUniqueCount);

				InterlockedCompareExchange128
				(
					(volatile INT64*)_ptail,
					(INT64)lTailUniqueCount,
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

			*pOutData = bHeadNextNode->Data;

			if (false == InterlockedCompareExchange128
			(
				(volatile INT64*)this->_phead,
				(INT64)lHeadUniqueCount,
				(INT64)bHeadNextNode,
				(INT64*)&bTopHeadNode
			))
			{
				// DCAS 실패
				for (int i = 0; i < backoff; ++i)
					YieldProcessor();
				if (backoff < 16)
					backoff <<= 1;
				continue;
			}
			else
			{
				// DCAS 성공
				break;
			}
			//____________________________________________________________________
		}

		// CAS128()가 Comp쪽으로 뱉어준 원래노드를 해제
		this->_pFreeList->Free(bTopHeadNode.pNode);
		if constexpr (UseApproxSize)
			this->_UseSize.fetch_sub(1, std::memory_order_relaxed);

		return true;
	}
private:
	CFreeList<NODE, PlacementNew>* _pFreeList;
	volatile TopNODE* _phead;
	volatile TopNODE* _ptail;
	bool _Initialized;
	alignas(64) std::atomic<INT64> _UseSize;
	alignas(64) volatile INT64 _HeadUniqueCount;
	alignas(64) volatile INT64 _TailUniqueCount;
};

}


#endif