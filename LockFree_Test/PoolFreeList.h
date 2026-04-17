#pragma once


#ifndef ____POOL_FREE_LIST_H____
#define ____POOL_FREE_LIST_H____


#include <windows.h>
#include <new>
#include <atomic>
#include <intrin.h>

#define IDENT_VAL 0x6659

namespace LockFree
{
	namespace Internal
	{

		template<typename T, bool PlacementNew = false, bool UseApproxSize = false>
		class CPoolFreeList
		{

			struct NODE
			{
				NODE* pNextNode;
				T		Data;
			};

			__forceinline static NODE* DataToNode(T* data)
			{
				return reinterpret_cast<NODE*>(
					reinterpret_cast<char*>(data) - offsetof(NODE, Data));
			}

			__forceinline static T* NodeToData(NODE* node)
			{
				return &node->Data;
			}

			struct TopNODE
			{
				NODE* volatile pNode;
				volatile INT64 UniqueCount;
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
			explicit CPoolFreeList()
			{
				this->_pTopNode = nullptr;
				hHeap = nullptr;
				this->_Initialized = false;
				this->_AllocCount = 0;
				if constexpr (UseApproxSize)
					_UseSize.store(0, std::memory_order_relaxed);

				Init();
			}

			CPoolFreeList(const CPoolFreeList&) = delete;
			CPoolFreeList& operator=(const CPoolFreeList&) = delete;
			CPoolFreeList(CPoolFreeList&&) = delete;
			CPoolFreeList& operator=(CPoolFreeList&&) = delete;

			bool Init()
			{
				if (this->_Initialized)
					return true;

				this->_pTopNode = (TopNODE*)_aligned_malloc(64, 64);
				if (this->_pTopNode == nullptr)
					return false;

				this->_pTopNode->pNode = nullptr;
				this->_pTopNode->UniqueCount = 0;

				hHeap = HeapCreate(NULL, 0, NULL);
				if (hHeap == nullptr)
				{
					_aligned_free((void*)this->_pTopNode);
					this->_pTopNode = nullptr;
					return false;
				}

				// 저단편화 힙(LFH) 설정
				ULONG HeapInformationValue = 2;
				HeapSetInformation(hHeap, HeapCompatibilityInformation,
					&HeapInformationValue, sizeof(HeapInformationValue));

				this->_Initialized = true;
				return true;
			}

			~CPoolFreeList()
			{
				if (this->_Initialized == false)
					return;

				NODE* pfNode = nullptr;		//DeleteNode

				while (this->_pTopNode->pNode != nullptr)
				{
					pfNode = this->_pTopNode->pNode;
					this->_pTopNode->pNode = this->_pTopNode->pNode->pNextNode;
					// PlacementNew 모드: free list의 노드는 이미 Free()에서 소멸자 호출됨
					if constexpr (!PlacementNew)
						pfNode->Data.~T();

					//delete pfNode;
					HeapFree(hHeap, 0, pfNode);	// HeapFree
				}

				_aligned_free((void*)this->_pTopNode);
				HeapDestroy(hHeap);
			}

		public:
			bool Free(T* Data)
			{
				if (this->_Initialized == false)
					return false;

				if (Data == nullptr)
					return false;

				// Free Node
				NODE* fNode = DataToNode(Data);

				// 잘못된 주소가 전달된 경우
				//if (fNode->IsMine != IDENT_VAL)
					//return false;

				// 소멸자 호출 (free list 반환 전에 호출해야 use-after-free 방지)
				if constexpr (PlacementNew)
					fNode->Data.~T();

				// backup TopNode
				TopNODE bTopNode;
				CASBackoff backoff;

				//_______________________________________________________________________________________
				//  
				//	CAS Version
				//_______________________________________________________________________________________
				while (true)
				{
					bTopNode.pNode = this->_pTopNode->pNode;
					fNode->pNextNode = bTopNode.pNode;

					NODE* pNode = (NODE*)InterlockedCompareExchangePointer
					(
						(volatile PVOID*)&this->_pTopNode->pNode,
						(PVOID)fNode,
						(PVOID)bTopNode.pNode
					);

					if (pNode != bTopNode.pNode)
					{
						backoff.Pause();
						continue;
					}
					else
					{
						break;
					}
				}
				//_______________________________________________________________________________________

				if constexpr (UseApproxSize)
					_UseSize.fetch_add(1, std::memory_order_relaxed);

				return true;
			}


			__declspec(noinline) T* AllocNewNode()
			{
				if (this->_Initialized == false)
					return nullptr;

				// TODO : 더 최적화하고자 한다면 virtualAlloc
				NODE* rNode = (NODE*)HeapAlloc(this->hHeap, FALSE, sizeof(NODE));
				if (rNode == nullptr)
					return nullptr;

				new(&rNode->Data) T;
				rNode->pNextNode = nullptr;
				InterlockedIncrement64(&this->_AllocCount);
				return NodeToData(rNode);
			}

			T* Alloc()
			{
				if (this->_Initialized == false)
					return nullptr;

				TopNODE bTopNode;
				CASBackoff backoff;

				// free list에서 pop 시도 (hot path)
				while (true)
				{
					bTopNode.UniqueCount = this->_pTopNode->UniqueCount;
					bTopNode.pNode = this->_pTopNode->pNode;

					// free list가 비어있으면 새 노드 할당 (cold path)
					if (bTopNode.pNode == nullptr)
						return AllocNewNode();

					_mm_prefetch((const char*)bTopNode.pNode, _MM_HINT_T0);

					//CAS를 덜 호출하기위함
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
						//CAS 실패
						backoff.Pause();
						continue;
					}
					else
					{
						//CAS 성공
						break;
					}
				}

				NODE* rNode = bTopNode.pNode;

				// 생성자 호출
				if constexpr (PlacementNew)
					new (&rNode->Data) T;

				if constexpr (UseApproxSize)
					_UseSize.fetch_sub(1, std::memory_order_relaxed);

				return NodeToData(rNode);
			}
		public:
			// 총 HeapAlloc된 노드 수 (단조 증가, cold path에서만 갱신)
			INT64 GetAllocCount() const { return _AllocCount; }
			INT64 GetApproxSize() const
			{
				if constexpr (UseApproxSize)
					return _UseSize.load(std::memory_order_relaxed);

				return 0;
			}

		private:
			TopNODE* _pTopNode;		//_allinge_malloc()
			HANDLE hHeap;
			bool _Initialized;

			alignas(64) volatile INT64	_AllocCount;	// HeapAlloc된 전체 노드 수
			alignas(64) std::atomic<INT64> _UseSize;
		};

	}

	template<typename T, bool PlacementNew = false, bool UseApproxSize = false>
	using CPoolFreeList = Internal::CPoolFreeList<T, PlacementNew, UseApproxSize>;

	template<typename T, bool PlacementNew = false, bool UseApproxSize = false>
	using CFreeList = CPoolFreeList<T, PlacementNew, UseApproxSize>;

}

#endif //____POOL_FREE_LIST_H____