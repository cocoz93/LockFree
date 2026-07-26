# LockFree 자료구조 결함 테스트

Lock-free 자료구조(`InternalFreeList`, `LockFreeStack`, `LockFreeQueue`, `CExternalTlsFreeList`)의 무결성을 검증하는 테스트 프로그램.

```
테스트 대상         싱글 스레드 (기능)           멀티스레드 (경합)
─────────────────────────────────────────────────────────────────
InternalFreeList    1-1 데이터 무결성            2-1 Alloc/Free 정합성
                    1-2 불변 조건                2-2 소유권 이전 + corruption
                    1-3 PlacementNew             (+ fl 모드: 이중 배부/유실)
                    1-4 방어 로직
─────────────────────────────────────────────────────────────────
LockFreeStack       1-1 LIFO 순서 + 무결성      2-1 전수 검증 (8가지 조합)
                    1-2 방어 + ApproxSize        2-2 고빈도 경합 + corruption
                                                 2-3 보존 불변식 (허위 empty/HANG)
─────────────────────────────────────────────────────────────────
LockFreeQueue       1-1 FIFO 순서 + 무결성      2-1 전수 검증 (8가지 조합)
                    1-2 방어 + Clear             2-2 고빈도 경합 + corruption
                                                 2-3 보존 불변식 (허위 empty/HANG)
                                                 2-4 생산자별 FIFO 반증 (Q-E1)
                                                 (+ qbuf 모드: 실패 시 버퍼 오염)
─────────────────────────────────────────────────────────────────
반복 횟수           50만 회                      스레드당 1천만 회 (2~32T)
```

> 위 표의 "반복 횟수"는 바깥 루프 기준이다. Phase 1·2-1은 한 번 돌 때마다 1~64개를
> 묶어 처리하므로 실제 Alloc/Push 호출은 그 배수(평균 약 16배)가 된다.

---

## 검증 방식

- **TestPayload** (16바이트): `magic(0xDEADBEEF)` + `threadId` + `sequence` + `checksum(XOR)` — 데이터 corruption 감지용
- **LifecycleTracker**: atomic 카운터로 생성자/소멸자 호출 횟수 추적 — PlacementNew 검증용
- **TEST_ASSERT**: 조건 실패 시 즉시 크래시(널 역참조)로 프로세스를 죽인다. 자동화에서는
  종료코드(0xC0000005)로 실패를 잡는다. `Crash/CrashDump.h`는 이 실행 파일에 포함돼 있지
  않으므로 **덤프 파일은 생성되지 않는다** — 덤프가 필요하면 그 헤더를 include할 것

---

## 테스트 구성

### InternalFreeList (6개 Phase)

| Phase | 구분 | 내용 |
|-------|------|------|
| 1-1 | 싱글, 50만 회 | 랜덤 배치(1~64개) Alloc → checksum 기록 → 검증 → Free. Alloc/Free 실패, checksum 불일치 감지 |
| 1-2 | 싱글, 50만 회 | 랜덤으로 Alloc/Free/검사를 수행하며 `AllocCount == InUse + FreeListSize` 불변 조건 상시 검증 |
| 1-3 | 싱글, 50만 회 | PlacementNew=true에서 생성자/소멸자 호출 횟수가 예상값과 정확히 일치하는지 검증 |
| 1-4 | 싱글 | nullptr Free 방어, LIFO 재활용, 주소 고유성(1만 개), Alloc/Free 스트레스 |
| 2-1 | MT, 2~32 스레드 | 각 스레드가 독립 Alloc/Free 반복(스레드당 1천만 회). 종료 후 Alloc 수 == Free 수, AllocCount == FreeListSize 검증 |
| 2-2 | MT, 2~32 스레드 | Producer(Alloc→공유풀 Push) / Consumer(Pop→checksum 검증→Free). 스레드 간 노드 소유권 이전 시 corruption 감지 |

### LockFreeStack (5개 Phase)

| Phase | 구분 | 내용 |
|-------|------|------|
| 1-1 | 싱글, 50만 회 | 랜덤 배치 Push → 전부 Pop하며 LIFO 역순 + checksum 검증 |
| 1-2 | 싱글 | 빈 스택 Pop 방어, ApproxSize +1/-1 정합성(1만 회), Push/Pop 스트레스 |
| 2-1 | MT, 8가지 조합 | Producer가 고유 숫자 Push, Consumer가 Pop. 모든 숫자가 정확히 1번만 Pop되었는지 전수 검증 |
| 2-2 | MT, 2~32 스레드 | 짝수 스레드 Push(checksum 기록), 홀수 스레드 Pop(checksum 검증). 잔여 drain 후 Push 수 == Pop 수 확인 |
| 2-3 | MT, {8, 40} 스레드 | 보존 불변식. 아래 "보존 불변식" 절 참고 (`cons` 모드) |

### LockFreeQueue (6개 Phase)

| Phase | 구분 | 내용 |
|-------|------|------|
| 1-1 | 싱글, 50만 회 | 랜덤 배치 Enqueue → 전부 Dequeue하며 FIFO 순서 + checksum 검증 |
| 1-2 | 싱글 | 빈 큐 Dequeue 방어, ApproxSize +1/-1 정합성(1만 회), Clear 검증, Enqueue/Dequeue 스트레스 |
| 2-1 | MT, 8가지 조합 | Producer가 고유 숫자 Enqueue, Consumer가 Dequeue. 모든 숫자가 정확히 1번만 Dequeue되었는지 전수 검증 |
| 2-2 | MT, 2~32 스레드 | 짝수 스레드 Enqueue(checksum 기록), 홀수 스레드 Dequeue(checksum 검증). 잔여 drain 후 Enqueue 수 == Dequeue 수 확인 |
| 2-3 | MT, {8, 40} 스레드 | 보존 불변식. 아래 "보존 불변식" 절 참고 (`cons` 모드) |
| 2-4 | MT, 시간예산 | 생산자별 FIFO 반증. 각 생산자가 순번을 순서대로 넣고 단일 소비자가 관측 → 순번이 건너뛰면 Q-E1 결함 (`repro` 모드) |

### 보존 불변식 (Stack / Queue 공용 — `cons` 모드)

미리 5만 개를 채우고, 각 스레드가 "꺼내면 즉시 되넣기"만 반복한다. 스레드가 손에 쥐는 값은
최대 1개라 내부에는 항상 여유가 남으므로 **논리적으로 절대 비지 않는다**. 따라서

- 꺼내기가 한 번이라도 실패하면 그 자체로 "비지 않았는데 실패" 결함
- 진행이 멈추면 멈춤으로 판정 — 감시는 **스레드별**로 한다. 합산 카운터 하나만 보면
  한 스레드가 영영 멈춰도 나머지가 숫자를 올려 끝까지 못 잡는다
- 종료 후 전량 회수해 개수·합계를 대조 → 유실/중복/손상 검출

### CExternalTlsFreeList (헤드리스 2종)

| 모드 | 구분 | 내용 |
|------|------|------|
| `tls` | MT, 시간예산 | 생산자→소비자 크로스스레드 Alloc/Free. "나가 있는 슬롯" 집합으로 이중 배부·이중 free·데이터 손상·보존(Alloc==Free)을 검사하고, 끝났을 때 회수되지 않은 청크 수가 예산을 넘으면 위반으로 판정 |
| `tlsleak` | MT, 관측 | 각 스레드가 청크를 소진하지 않고 몇 개만 Alloc→전부 Free 후 종료. 미소진 청크가 회수되지 않아 누적되는지 관측(알려진 설계 한계라 정상 실행은 항상 0) |

---

## 반복 횟수 설계 근거

- **Phase 1 (싱글 스레드)**: 50만 회. 기능 검증이므로 로직 결함은 수천 회 이내에 발견됨. 과도한 반복 불필요.
- **Phase 2 (멀티스레드)**: 스레드당 1천만 회. CAS 경합의 race window 노출에는 높은 반복이 필요. 32스레드 기준 총 3.2억 회 경합.

---

## 실행 방법

인자 없이 실행하면 메뉴가 뜬다:
1. InternalFreeList 전체 테스트
2. LockFreeStack 전체 테스트
3. LockFreeQueue 전체 테스트
4. 전체 통합 테스트 (FreeList + Stack + Queue)
5. 보존 불변식 빠른 검증 (Stack + Queue, 약 1분)
6. Queue 생산자별 FIFO 반증 (Q-E1, 약 1분)

> 메뉴 1~4는 Phase 1·2 전체를 도는 경로라 조합당 수십 분이 걸린다(32스레드 구성 하나가
> Alloc 약 53억 회). 자동화·회귀에는 아래 헤드리스 모드를 쓸 것.

### 헤드리스 모드 (CLI 인자)

종료코드로 결과를 받는 회귀/자동화용 모드. **0=무결, 2=위반 검출, 1=인자 오류**.
인자는 전부 1 이상이어야 하며, 실제 작업량이 0건이면 통과시키지 않고 실패로 처리한다.

```
TestCode.exe cons                                  # 보존 불변식: 허위 empty·유실·멈춤 (크래시=실패)
TestCode.exe repro [prod=8] [cap=64] [secs=60]     # 생산자별 FIFO 반증 (Q-E1)
TestCode.exe qbuf  [cons=8] [secs=10]              # Dequeue 실패 시 출력버퍼 보존 (1=검증경로 미도달)
TestCode.exe fl    [threads=8] [secs=30]           # 프리리스트 이중 배부·유실
TestCode.exe tls   [prod=4] [cons=4] [secs=30]     # 외부풀 크로스스레드 정합성 + 청크 회수
TestCode.exe tlsleak [rounds=20] [tpr=4] [apt=5]   # 스레드 종료 누수 관측 (알려진 한계, 정상 실행은 0)
```

경합 창을 인위적으로 벌린 빌드(`/DUSE_RACE_HOOK`)로 같이 돌리면 드물게만 열리는 창을
수천 배 빨리 노출시킨다. 평소 빌드에서 이 훅은 빈 매크로라 비용이 없다.

```
cl /O2 /DNDEBUG /DUSE_RACE_HOOK /std:c++17 /EHsc TestCode.cpp
```
