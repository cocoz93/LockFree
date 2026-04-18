// 원본
#pragma once


#ifndef ____LOCKFREE_STACK_H____
#define ____LOCKFREE_STACK_H____

#include <atomic>
#include "PoolFreeList.h"

namespace LockFree
{

template<typename T, bool UseApproxSize = false>
class CLockFreeStack
{
	struct NODE
	{
		NODE*	pNextNode;
		T		Data;
	};

	struct TopNODE
	{
		NODE* volatile	pNode;
		volatile INT64	UniqueCount;
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


public:
	explicit CLockFreeStack(void)
	{
		_pFreeList = nullptr;
		_pTopNode = nullptr;
		_Initialized = false;
		if constexpr (UseApproxSize)
			_UseSize.store(0, std::memory_order_relaxed);

		Init();
	}

	CLockFreeStack(const CLockFreeStack&) = delete;
	CLockFreeStack& operator=(const CLockFreeStack&) = delete;
	CLockFreeStack(CLockFreeStack&&) = delete;
	CLockFreeStack& operator=(CLockFreeStack&&) = delete;

	bool Init()
	{
		if (_Initialized)
			return true;

		_pFreeList = new(std::nothrow) CFreeList<NODE>;
		if (_pFreeList == nullptr)
			return false;

		_pTopNode = (TopNODE*)_aligned_malloc(64, 64);
		if (_pTopNode == nullptr)
		{
			delete _pFreeList;
			_pFreeList = nullptr;
			return false;
		}

		_pTopNode->pNode = nullptr;
		_pTopNode->UniqueCount = 0;

		_Initialized = true;
		return true;
	}

	~CLockFreeStack(void)
	{
		if (_Initialized == false)
			return;

		NODE* pfNode;

		while (this->_pTopNode->pNode != nullptr)
		{
			pfNode = this->_pTopNode->pNode;
			this->_pTopNode->pNode = this->_pTopNode->pNode->pNextNode;
			_pFreeList->Free(pfNode);
		}

		_aligned_free((void*)this->_pTopNode);
		delete this->_pFreeList;
	}

public:
	bool IsEmpty(void)
	{
		if constexpr (UseApproxSize)
			return (_UseSize.load(std::memory_order_relaxed) == 0);

		return this->_pTopNode->pNode == nullptr;
	}

	INT64 GetApproxSize(void) const
	{
		if constexpr (UseApproxSize)
			return _UseSize.load(std::memory_order_relaxed);

		return 0;
	}

	bool push(const T& Data)
	{
		// NewNode 
		NODE* nNode = _pFreeList->Alloc();
		if (nullptr == nNode)
			return false;

		// Data backup
		nNode->Data = Data;

		// backup TopNode 
		TopNODE bTopNode;

		//_______________________________________________________________________________________
		// 
		// DCAS 버전. CAS로 가능하다면 DCAS할필요 X
		//_______________________________________________________________________________________

		/*
		LONG64 nUniqueCount = InterlockedIncrement64((LONG64*)&this->_pTopNode->UniqueCount);
		while (true)
		{
			bTopNode.UniqueCount = this->_pTopNode->UniqueCount;
			bTopNode.pNode = this->_pTopNode->pNode;
			nNode->pNextNode = this->_pTopNode->pNode;
			if (false == InterlockedCompareExchange128
			(
				(LONG64*)this->_pTopNode,
				(LONG64)nUniqueCount,
				(LONG64)nNode,
				(LONG64*)&bTopNode
			))
			{
				//DCAS 실패
				continue;
			}
			else
			{
				//DCAS 성공
				break;
			}
		}*/
		//_______________________________________________________________________________________


		//_______________________________________________________________________________________
		// 
		// CAS 버전
		//_______________________________________________________________________________________
		CASBackoff backoff;

		while (true)
		{
			bTopNode.pNode = this->_pTopNode->pNode;
			nNode->pNextNode = bTopNode.pNode;

			NODE* pNode = (NODE*)InterlockedCompareExchangePointer
			(
				(volatile PVOID*)&this->_pTopNode->pNode,
				(PVOID)nNode,
				(PVOID)bTopNode.pNode
			);

			if (pNode != bTopNode.pNode)
			{
				backoff.Pause();
				continue;
			}
			else
				break;
		}
		//_______________________________________________________________________________________

		if constexpr (UseApproxSize)
			_UseSize.fetch_add(1, std::memory_order_relaxed);

		//_______________________________________________________________________________________
		//
		// Empty()하여 POP이 가능하다고했는데, 누군가 빼버렸을수있으나 상관X
		// 멀티스레딩 환경에서 이부분을 완전히 보장은 불가
		// 사용하는입장에서 감안하고 사용
		//_______________________________________________________________________________________
		return true;
	}


	bool pop(T* pOutData)
	{
		TopNODE bTopNode;
		CASBackoff backoff;

		while (true)
		{
			bTopNode.UniqueCount = this->_pTopNode->UniqueCount;
			bTopNode.pNode = this->_pTopNode->pNode;

			// 스택이 비어있으면 실패 (호출자 책임)
			if (bTopNode.pNode == nullptr)
				return false;

			_mm_prefetch((const char*)bTopNode.pNode, _MM_HINT_T0);

			//CAS를 덜 호출하기위해, 이미 자료구조가 바뀌었다면 다시시도.
			if (bTopNode.UniqueCount != this->_pTopNode->UniqueCount)
				continue;

			if (false == InterlockedCompareExchange128
			(
				(volatile INT64*)this->_pTopNode,
				(INT64)(bTopNode.UniqueCount + 1),
				(INT64)bTopNode.pNode->pNextNode,
				(INT64*)&bTopNode
			))
			{
				// DCAS 실패
				backoff.Pause();
				continue;
			}
			else
			{
				// DCAS 성공
				break;
			}
		}

		// CAS128()은 성공실패 여부와 관계없이 Comp쪽으로 원래(이전)노드를 뱉음
		*pOutData = bTopNode.pNode->Data;
		this->_pFreeList->Free(bTopNode.pNode);
		if constexpr (UseApproxSize)
			_UseSize.fetch_sub(1, std::memory_order_relaxed);

		return true;
	}

private:
	CFreeList<NODE>*	_pFreeList;
	TopNODE*			_pTopNode;
	bool				_Initialized;
	alignas(64) std::atomic<INT64> _UseSize;
};
}

#endif
