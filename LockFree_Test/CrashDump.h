#pragma once
#pragma comment(lib, "DbgHelp")	//MiniDumpWriteDump

#include <stdio.h>
#include <signal.h>		//signal, SIGABRT
#include <new.h>		//_set_new_handler
#include <windows.h>
#include <Psapi.h>		//PROCESS_MEMORY_COUNTERS
#include <DbgHelp.h>	//_MINIDUMP_EXCEPTION_INFORMATION
#include <crtdbg.h>		//_CrtSetReportMode


class CCrashDump
{
public:
	CCrashDump()
	{
		//밑 구문들은 CRT오류 메시지표시를 중단하기 위함.
		//우리는 덤프를 빼고 프로세스를 종료시켜야 한다.

		//-------------------------------------------------------------------------------

		// INVALIDE_PARAMETER 에러핸들러를 우리가 캐치
		// ex) CRT함수에 잘못된 인자전달 (매개변수 가변인자 오류, NULL이 들어갈수 없는곳에 NULL)
		_set_invalid_parameter_handler(myInvalidParameterHandler);

		// pure virtual function called 에러핸들러를 우리가 캐치
		_set_purecall_handler(MyPurecallHandler);

		// C++ 미처리 예외의 최종 종착점 (throw 후 catch 없을 때)
		set_terminate(MyTerminateHandler);

		// operator new 실패 시 핸들러 (bad_alloc 대신 우리가 처리)
		_set_new_handler(MyNewHandler);
		_set_new_mode(1);

		// abort() 호출 시 메시지 박스 / WER 보고를 차단 → 바로 우리 핸들러로 진입
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

		// SIGABRT 시그널 캐치 (abort, assert 등이 발생시킴)
		signal(SIGABRT, MySigAbrtHandler);

		//CRT 디버그 보고 모드를 모두 끔 (경험한 적은 없으나 만약을 위함)
		_CrtSetReportMode(_CRT_WARN, 0);
		_CrtSetReportMode(_CRT_ASSERT, 0);
		_CrtSetReportMode(_CRT_ERROR, 0);
		_CrtSetReportHook(_custom_Report_hook);


		// 핸들링되지 않은 모든 예외를 우리쪽으로 캐치
		// ex) Throw 던졌는데 Catch존재하지않음, 메모리참조 오류, 모든예외를 받는 경우 등
		// 원래는 catch를 못받는 경우 메인까지 튀어나와 SEH로 예외발생
		SetUnhandledExceptionFilter(MyExceptionFilter);

		// CRT가 내부적으로 SetUnhandledExceptionFilter(NULL)을 호출해 우리 핸들러를 덮어쓰는 것을 차단.
		// kernel32.dll의 SetUnhandledExceptionFilter 진입점 첫 바이트를 ret으로 패치한다.
		DisableSetUnhandledExceptionFilter();

		//-------------------------------------------------------------------------------
	}

	CCrashDump(const CCrashDump&) = delete;
	CCrashDump& operator=(const CCrashDump&) = delete;


	// 의도적 크래시. __debugbreak()는 컴파일러가 제거하지 않는 인트린직.
	static void Crash(void)
	{
		__debugbreak();
	}



	/*
	우리가 정의한 MyExceptionFilter는 생성자 내부에서,
	API함수인 SetUnhandledExceptionFilter()의 매개변수로 전달될 함수.

	이 함수의 매개변수 EXCEPTION_POINTERS은 규칙을 따른것.
	ExceptionFilter라는 구조체의 포인터를 여기서 받으면 된다.
	그럼 예외발생시 이 인자로 자동으로 들어올 것이다.
	우리는 여기서 덤프를 여기서 뺀다.
	*/
	static LONG WINAPI MyExceptionFilter(__in PEXCEPTION_POINTERS pExceptionPointer)
	{
		long DumpCount = InterlockedIncrement(&_DumpCount);

		// 동시 다발 크래시 시 첫 번째 스레드만 덤프를 생성한다.
		// 나머지 스레드는 덤프 완료 후 프로세스 종료를 대기.
		if (DumpCount > 1)
		{
			Sleep(INFINITE);
			return EXCEPTION_EXECUTE_HANDLER;
		}


		//----------------------------------------------------------
		// 현재 프로세스 메모리 정보
		//----------------------------------------------------------
		PROCESS_MEMORY_COUNTERS pmc;
		int WorkingMemoryMB = 0;

		if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
		{
			WorkingMemoryMB = (int)(pmc.WorkingSetSize / 1024 / 1024);
		}


		//----------------------------------------------------------
		// 현재 날짜와 시간을 알아온다.
		//----------------------------------------------------------
		SYSTEMTIME NowTime;
		WCHAR filename[MAX_PATH];

		GetLocalTime(&NowTime);
		swprintf_s(filename, MAX_PATH, L"Dump_%d%02d%02d_%02d.%02d.%02d_%d.dmp",
			NowTime.wYear, NowTime.wMonth, NowTime.wDay,
			NowTime.wHour, NowTime.wMinute, NowTime.wSecond, DumpCount);

		wprintf(L"\n\n\n!!! Crash Error!!! %d.%d.%d/%d:%d:%d  (WorkingSet: %dMB)\n",
			NowTime.wYear, NowTime.wMonth, NowTime.wDay,
			NowTime.wHour, NowTime.wMinute, NowTime.wSecond, WorkingMemoryMB);
		wprintf(L"Now Save dump file...\n");

		HANDLE hDumpFile = CreateFile
		(
			filename,
			GENERIC_WRITE,
			FILE_SHARE_WRITE,		//쓰기모드
			NULL,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL
		);


		if (hDumpFile != INVALID_HANDLE_VALUE)
		{
			//덤프파일 정보 설정
			_MINIDUMP_EXCEPTION_INFORMATION MinidumpExceptionInformation;

			MinidumpExceptionInformation.ThreadId = ::GetCurrentThreadId();
			MinidumpExceptionInformation.ExceptionPointers = pExceptionPointer;	 //인자로 들어온 예외포인터
			MinidumpExceptionInformation.ClientPointers = FALSE;
			//msdn상에서는 외부디버거에서 해당 덤프를 뺄때 설정이라고 명시되어있음.
			//TRUE/FALSE에 대한 차이점을 찾지못함.


			/*
			MiniDumpWriteDump는 실질적으로 메인이 되는 함수로,
			호출 시 전달된 파일(핸들)을 대상으로 write가 시작된다.
			*/
			MINIDUMP_TYPE dumpType = (MINIDUMP_TYPE)(
				MiniDumpWithFullMemory          |	// 전체 프로세스 메모리
				MiniDumpWithFullMemoryInfo       |	// 메모리 영역별 상세 정보 (주소, 크기, 보호 속성)
				MiniDumpWithHandleData           |	// 열린 핸들 목록 (파일, 소켓, 이벤트 등)
				MiniDumpWithThreadInfo           |	// 스레드 시작 주소, 선호도, 우선순위 등
				MiniDumpWithUnloadedModules      |	// 언로드된 DLL 목록 (DLL 언로드 후 해당 코드 크래시 추적용)
				MiniDumpWithProcessThreadData);		// TLS, PEB, TEB 등 프로세스/스레드 내부 데이터

			MiniDumpWriteDump
			(
				GetCurrentProcess(),
				GetCurrentProcessId(),
				hDumpFile,
				dumpType,
				&MinidumpExceptionInformation, //우리가 정의한 미니덤프의 예외정보. 
				NULL,
				NULL
			);
			CloseHandle(hDumpFile);
			wprintf(L"CrashDumpSaveFinish!");
		}

		return EXCEPTION_EXECUTE_HANDLER;
		//파일에 덤프를 모두 저장 후 리턴.
		//해당값을 리턴해 예외처리가 끝났다고 알려, 예외창이 뜨는것을 막는다.
	}



	static void myInvalidParameterHandler(const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned int line, uintptr_t pReserved)
	{
		Crash();
	}

	static int _custom_Report_hook(int ireposttype, char* message, int* returnval)
	{
		Crash();
		return 1;
	}

	static void MyPurecallHandler(void)
	{
		Crash();
	}

	static void __cdecl MyTerminateHandler(void)
	{
		Crash();
	}

	static int __cdecl MyNewHandler(size_t size)
	{
		Crash();
		return 0;
	}

	static void MySigAbrtHandler(int sig)
	{
		// SIGABRT 재진입 방지: 핸들러 안에서 다시 abort가 호출될 수 있으므로 재등록
		signal(SIGABRT, MySigAbrtHandler);
		Crash();
	}

private:
	// CRT 내부가 SetUnhandledExceptionFilter(NULL)을 호출해 우리 핸들러를 제거하는 것을 차단.
	// 함수 진입점의 첫 바이트들을 "xor eax, eax; ret" 또는 "ret 4"(x86)로 패치하여 무력화.
	static void DisableSetUnhandledExceptionFilter(void)
	{
		void* pFunc = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetUnhandledExceptionFilter");
		if (pFunc == nullptr)
			return;

		DWORD dwOldProtect;
#ifdef _WIN64
		// x64: xor eax, eax (31 C0) + ret (C3) = 3바이트
		constexpr BYTE patchBytes[] = { 0x31, 0xC0, 0xC3 };
		constexpr SIZE_T patchSize = sizeof(patchBytes);
#else
		// x86 __stdcall: mov eax, 0 (B8 00 00 00 00) + ret 4 (C2 04 00) = 8바이트
		constexpr BYTE patchBytes[] = { 0xB8, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x04, 0x00 };
		constexpr SIZE_T patchSize = sizeof(patchBytes);
#endif

		if (VirtualProtect(pFunc, patchSize, PAGE_EXECUTE_READWRITE, &dwOldProtect))
		{
			memcpy(pFunc, patchBytes, patchSize);
			VirtualProtect(pFunc, patchSize, dwOldProtect, &dwOldProtect);
		}
	}


	inline static long _DumpCount = 0;
};

// 전역 인스턴스. inline으로 다중 TU에서 include해도 단일 정의 보장 (C++17)
inline CCrashDump CrashDump;
