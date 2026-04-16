#pragma once

#ifndef ____TLS_FREE_LIST_H____
#define ____TLS_FREE_LIST_H____

#include "PoolFreeList.h"
//#include "SListFreeList.h"

namespace LockFree
{

inline volatile LONG64 g_Config = 0;

template<typename T>
class CTlsFreeList
{
public:
	struct ChunkNODE;
	struct ChunkDATA
	{
		T Data;
		ChunkNODE* pMyChunkNode;
		//LONG64 DataConfig;
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
			Initailize();
		}
	public:
		void Initailize()
		{
			//청크 클래스 구분
			this->Config = InterlockedIncrement64(&g_Config);	// [결함 E] 원자적 증가

			for (int i = 0; i < CHUNK_SIZE; ++i)
			{
				this->DataArr[i].pMyChunkNode = this;
				//this->DataArr[i].DataConfig = this->Config;
			}
		}

	public:
		// FreeCount는 multi-thread 접근 → 독립 캐시 라인 분리 (false sharing 방지)
		alignas(64) volatile SHORT FreeCount;
		// DataArr가 FreeCount와 AllocCount 사이에 위치하여 물리적 캐시 라인 분리
		ChunkDATA DataArr[CHUNK_SIZE];
		// AllocCount는 TLS 소유 스레드만 접근 → volatile 불필요, FreeCount와 별도 캐시 라인
		SHORT AllocCount;
		LONG64 Config;
											
	};


public:
	explicit CTlsFreeList(bool IsPlacementNew = false)
	{
		// ChunkNode의 생성 및 생성자 여부결정
		// Config와 this(찾아갈주소)는 한번 박아놓으면 바뀔일 없으므로 PlacementNew = false (기본값)
		this->_ChunkFreeList = nullptr;
		//this->_ChunkFreeList = new CSListFreeList<ChunkNODE>;

		// <T>자료형 자체에 대한 생성자 호출 여부
		this->_IsPlacementNew = IsPlacementNew;

		this->TlsIndex = TLS_OUT_OF_INDEXES;
		this->_Initialized = false;

		Init();
	}

	~CTlsFreeList()
	{
		if (this->TlsIndex != TLS_OUT_OF_INDEXES)
			TlsFree(this->TlsIndex);

		delete this->_ChunkFreeList;
	}

	bool Init()
	{
		if (this->_Initialized)
			return true;

		this->_ChunkFreeList = new CPoolFreeList<ChunkNODE>;
		if (this->_ChunkFreeList == nullptr)
			return false;

		this->TlsIndex = TlsAlloc();
		if (this->TlsIndex == TLS_OUT_OF_INDEXES)
			return false;

		// 저단편화를 막아보자
		ChunkNODE* pChunkNode[16];

		for (int i = 0; i < _countof(pChunkNode); ++i)
			pChunkNode[i] = this->_ChunkFreeList->Alloc();

		for (int i = 0; i < _countof(pChunkNode); ++i)
			this->_ChunkFreeList->Free(pChunkNode[i]);

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
			{
				// placment new 생성자호출
				if (this->_IsPlacementNew)
					new(&pChunkNode->DataArr[pChunkNode->AllocCount].Data) T;

				return (T*)(&pChunkNode->DataArr[pChunkNode->AllocCount--].Data);
			}

			//마지막 인자 인 경우, 새청크를 SetValue해놓고 마지막인자를 반환해줌
			// if (pChunkNode->AllocCount == 0)

			TlsSetValue(this->TlsIndex, 0);

			// placment new 생성자호출
			if (this->_IsPlacementNew)
				new(&pChunkNode->DataArr[0].Data) T;

			return &(pChunkNode->DataArr[0].Data);
		}

		//  해당스레드에서 TlsGetValue()가 최초 호출된 경우 (또는 청크 소진 후)
		pChunkNode = this->_ChunkFreeList->Alloc();
		if (pChunkNode == nullptr)
			return nullptr;

		pChunkNode->AllocCount = CHUNK_SIZE - 2; //(반환할거 포함 마이너스)
		pChunkNode->FreeCount = CHUNK_SIZE;

		TlsSetValue(this->TlsIndex, pChunkNode);

		// placment new 생성자호출
		if (this->_IsPlacementNew)
			new(&pChunkNode->DataArr[CHUNK_SIZE - 1].Data) T;

		return &(pChunkNode->DataArr[CHUNK_SIZE - 1].Data);
	}

	void Free(volatile T* Data)
	{
		// 내 메모리풀에서 나간게 맞는가?
		//if (((ChunkDATA*)Data)->DataConfig != (((ChunkNODE*)((ChunkDATA*)Data)->pMyChunkNode)->Config))
		//{
		//	int* Crash = 0;
		//	*Crash = 0;
		//}

		ChunkNODE* pChunkNode = ((ChunkDATA*)Data)->pMyChunkNode;

		// 소멸자 호출 (free list 반환 전에 호출해야 use-after-free 방지)
		if (this->_IsPlacementNew)
			((T*)Data)->~T();

		// 청크 Free카운트를 감소시키고, 모두 반납된경우 프리리스트로 반납한다.
		if (0 == InterlockedDecrement16(&pChunkNode->FreeCount))
		{
			// 프리리스트 반환
			this->_ChunkFreeList->Free(pChunkNode);
		}
	}


public:
	// [결함 F] ChunkAlloc 수정 (-> 연산자, FreeCount 초기값 수정, 실패 처리)
	bool ChunkAlloc(ChunkNODE** ppChunkNode)
	{
		ChunkNODE* pNewChunkNode = this->_ChunkFreeList->Alloc();
		if (pNewChunkNode == nullptr)
			return false;

		pNewChunkNode->FreeCount = CHUNK_SIZE;
		pNewChunkNode->AllocCount = CHUNK_SIZE - 1;

		TlsSetValue(this->TlsIndex, pNewChunkNode);

		*ppChunkNode = pNewChunkNode;
		return true;
	}

public:
	CPoolFreeList<ChunkNODE>* _ChunkFreeList;
	//CSListFreeList<ChunkNODE>* _ChunkFreeList;

private:
	int TlsIndex;
	bool _IsPlacementNew;		// 데이터 <T>에 대한 생성자 여부결정
	bool _Initialized;
};

template<typename T>
using CTLS_LockFree_FreeList = CTlsFreeList<T>;

}

#endif //____TLS_FREE_LIST_H____




// 
//===================================================================================
// 최적화 이전버전
//===================================================================================
//#pragma once
//
//
//#ifndef ____TLS_LOCKFREE_FREELIST_H____
//#define ____TLS_LOCKFREE_FREELIST_H____
//
//#include "LockFree_FreeList.h"
//#include "LockFreeStack.h"
//#include <map>
//
//#define CHUNK_SIZE 1000
//
//static LONG64 g_Config = 0x6656;
//
////이거지금 SendFlag하듯이 해서 오류가 나는지 확인해보기
//
//template<typename T>
//class CTLS_LockFree_FreeList
//{
//public:
//	struct ChunkNODE;
//
//	struct ChunkDATA
//	{
//		T Data;
//		ChunkNODE* pMyChunkNode;
//		LONG64 DataConfig;
//	};
//
//	struct ChunkNODE
//	{
//	public:
//		ChunkDATA DataArr[CHUNK_SIZE];
//		alignas(64) long long AllocCount;
//		alignas(64) long long FreeCount;
//		DWORD ThreadId;
//		bool UseFlag;
//	};
//
//
//public:
//	explicit CTLS_LockFree_FreeList()
//	{
//		this->Config = ++g_Config;
//
//		this->TlsIndex = TlsAlloc();
//		if (TlsIndex == TLS_OUT_OF_INDEXES)
//		{
//			int* a = nullptr;
//			*a = 0;
//		}
//	}	
//	virtual ~CTLS_LockFree_FreeList() {}
//
//
//public:
//	//DataAlloc
//	T* Alloc()
//	{
//		//Tls->Map Debug
//		ChunkNODE* pChunkNode = (ChunkNODE*)TlsGetValue(this->TlsIndex);
//
//		//해당스레드에서 Tls가 최초 호출된 경우
//		if (pChunkNode == nullptr)
//		{
//			//TlsGetValue 에러검사
//			if (pChunkNode == ERROR_SUCCESS)
//			{
//				if (0 != GetLastError())
//				{
//					int* a = nullptr;
//					*a = 0;
//				}
//			}
//
//			ChunkAlloc(&pChunkNode);
//			return &(pChunkNode->DataArr[0].Data);
//		}
//
//		//이미 Tls가 있는 경우
//		else if (pChunkNode != nullptr)
//		{
//			//마지막인자인경우, SetValue를 해놓고 들어간다.
//			if (pChunkNode->AllocCount == CHUNK_SIZE - 1)
//			{
//				++pChunkNode->AllocCount;
//				ChunkNODE* pNewChunkNode = nullptr;
//				ChunkAlloc(&pNewChunkNode);
//				--pNewChunkNode->AllocCount;
//
//				return &(pChunkNode->DataArr[CHUNK_SIZE - 1].Data);
//			}
//
//			//청크를 모두 사용한 경우, 새로운 청크 할당
//			else if (pChunkNode->AllocCount >= CHUNK_SIZE)
//			{
//				ChunkNODE* pNewChunkNode = nullptr;
//				ChunkAlloc(&pNewChunkNode);
//				return &(pNewChunkNode->DataArr[0].Data);
//			}
//
//			//청크가 남아있는 경우
//			else if (pChunkNode->AllocCount < CHUNK_SIZE)
//			{
//				++pChunkNode->AllocCount;
//				return &(pChunkNode->DataArr[pChunkNode->AllocCount - 1].Data);
//			}
//		}
//	}
//
//	void Free(T* Data)
//	{
//		if (((ChunkDATA*)Data)->DataConfig != this->Config)
//		{
//			int* Crash = 0;
//			*Crash = 0;
//		}
//
//		//청크 Free카운트를 증가시키고, 모두 반납된경우 프리리스트로 반납한다.
//		if (CHUNK_SIZE <= InterlockedIncrement64(&(((ChunkNODE*)((ChunkDATA*)Data)->pMyChunkNode)->FreeCount)))
//		{
//			//printf("[%d]\t  Free(%d/%d) \t %X\n", GetCurrentThreadId(), AllocCount, FreeCount, pChunkNode);
//			
//			//프리리스트 반환
//			this->_ChunkFreeList.Free((ChunkNODE*)((ChunkDATA*)Data)->pMyChunkNode);
//			//delete ((ChunkNODE*)((ChunkDATA*)Data)->pMyChunkNode);
//		}
//	}
//
//
//
//
//public:
//	//size단위로 메모리블럭을 확보환다.
//	bool ChunkAlloc(ChunkNODE** ppChunkNode)
//	{
//		//ChunkNODE* pNewChunkNode = new ChunkNODE;
//		ChunkNODE* pNewChunkNode = this->_ChunkFreeList.Alloc();
//		
//		pNewChunkNode->FreeCount = 0;
//		pNewChunkNode->AllocCount = 1;
//
//		for (int i = 0; i < CHUNK_SIZE; ++i)
//		{
//			pNewChunkNode->DataArr[i].pMyChunkNode = pNewChunkNode;
//			pNewChunkNode->DataArr[i].DataConfig = this->Config;
//		}
//
//		if (0 == TlsSetValue(this->TlsIndex, pNewChunkNode))
//		{
//			int* a = nullptr;
//			*a = 0;
//		}
//		
//		*ppChunkNode = pNewChunkNode;
//		
//		//Deubg
//		//printf("[%d]\t Alloc(%d/%d) \t\t %X\n", GetCurrentThreadId(), pNewChunkNode->AllocCount, pNewChunkNode->FreeCount, pNewChunkNode);
//		return true;
//	}
//
//
//	int GetChunkSize()
//	{
//		return this->_ChunkFreeList.GetUseCount();
//	}
//
//
//	void DebugPrint()
//	{
//		printf("ChunkNODE Use / 
//  : [ %d / %d ]\n", _ChunkFreeList.GetUseCount(), _ChunkFreeList.GetAllocCount());
//	}
//
//
//
//	
//private:
//	int TlsIndex;
//	//식별자. ASCII CMC(677767) + Count(0)
//	CLockFree_FreeList<ChunkNODE> _ChunkFreeList;
//	LONG64 Config;
//};
//
//#endif //____TLS_LOCKFREE_FREELIST_H____
//