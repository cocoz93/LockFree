#pragma once


#ifndef ____INTERNAL_FREE_LIST_H____
#define ____INTERNAL_FREE_LIST_H____


#include <windows.h>
#include <new>
#include <atomic>
#include <intrin.h>
#include <cassert>

// 경합 창 증폭 훅 (검증 훅) — 상세 설명은 LockFreeQueue.h 동일 블록 참고.
// 평소 빌드에선 빈 매크로라 비용 0. 테스트가 "확률적 지연"으로 재정의하면 Alloc/Free의
// 경합 창이 µs로 벌어져 OS 선점으로만 드물게 터지던 재활용 경합을 수천 배 빨리 노출한다.
#ifndef LF_RACE_HOOK
#define LF_RACE_HOOK()
#endif

namespace LockFree
{
		template<typename T, bool PlacementNew = false, bool UseApproxSize = false>
		class CInternalFreeList
		{

			struct NODE
			{
				NODE* pNextNode;
#ifndef NDEBUG
				LONG64	OwnerId;	// 디버그 빌드에서만 존재. Free 시 소속 풀(this) 소유권 assert용
#endif
				T		Data;
			};

			// 사용자가 돌려준 Data 포인터로, 그것이 들어있던 NODE의 시작주소를 되찾는다.
			// Data는 NODE 안 고정 위치에 있으므로 그 거리(offsetof)만큼 앞으로 빼면 NODE 시작점.
			// 참고: offsetof는 비-standard-layout T에서 표준상 회색지대지만 MSVC x64에선 항상 정확.
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
			explicit CInternalFreeList()
			{
				this->_pTopNode = nullptr;
				hHeap = nullptr;
				this->_Initialized = false;
				this->_AllocCount = 0;
				if constexpr (UseApproxSize)
					_FreeListSize.store(0, std::memory_order_relaxed);

				Init();
			}

			CInternalFreeList(const CInternalFreeList&) = delete;
			CInternalFreeList& operator=(const CInternalFreeList&) = delete;
			CInternalFreeList(CInternalFreeList&&) = delete;
			CInternalFreeList& operator=(CInternalFreeList&&) = delete;

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

			// 아직 Free되지 않은(사용 중) 노드는 free list에 없으므로 회수되지 않고 누수된다.
			~CInternalFreeList()
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

					HeapFree(hHeap, 0, pfNode);
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

				// 릴리즈는 fail-fast(이 풀의 유효 포인터 전제, delete와 동일).
				// 디버그에서만 크로스풀 오용을 assert로 탐지 (와일드 포인터/double-free는 못 잡음).
				assert(fNode->OwnerId == reinterpret_cast<LONG64>(this) && "다른 풀에서 나온 포인터를 Free함");

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

					// [증폭] top 읽기 ~ push CAS 사이 창 (Treiber push: 이 사이 top이 바뀌면 CAS 실패·재시도)
					LF_RACE_HOOK();

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
					_FreeListSize.fetch_add(1, std::memory_order_relaxed);

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
#ifndef NDEBUG
				rNode->OwnerId = reinterpret_cast<LONG64>(this);
#endif
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

					// [증폭] 태그 재확인 ~ DCAS 사이 창 (재활용된 top의 next를 읽어 pop — UniqueCount 태그가 지키는 구간)
					LF_RACE_HOOK();

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
					_FreeListSize.fetch_sub(1, std::memory_order_relaxed);

				return NodeToData(rNode);
			}
		public:
			// 생성자가 삼킨 Init() 실패를 호출자가 확인하는 용도.
			// false면 Alloc()은 nullptr, Free()는 false를 반환한다.
			bool IsInitialized() const { return _Initialized; }

			// 총 HeapAlloc된 노드 수 (단조 증가, cold path에서만 갱신)
			INT64 GetAllocCount() const { return _AllocCount; }
			INT64 GetFreeListSize() const
			{
				if constexpr (UseApproxSize)
					return _FreeListSize.load(std::memory_order_relaxed);

				return 0;
			}

		private:
			TopNODE* _pTopNode;		//_allinge_malloc()
			HANDLE hHeap;
			bool _Initialized;

			alignas(64) volatile INT64	_AllocCount;	// HeapAlloc된 전체 노드 수
			alignas(64) std::atomic<INT64> _FreeListSize; // FreeList가 가지고있는 size
		};

}

#endif //____INTERNAL_FREE_LIST_H____