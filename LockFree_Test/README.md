# LockFree 자료구조 결함 테스트

Lock-free 자료구조(`InternalFreeList`, `LockFreeStack`, `LockFreeQueue`)의 무결성을 검증하는 테스트 프로그램.

```
테스트 대상         싱글 스레드 (기능)           멀티스레드 (경합)
─────────────────────────────────────────────────────────────────
InternalFreeList    1-1 데이터 무결성            2-1 Alloc/Free 정합성
                    1-2 불변 조건                2-2 소유권 이전 + corruption
                    1-3 PlacementNew
                    1-4 방어 로직
─────────────────────────────────────────────────────────────────
LockFreeStack       1-1 LIFO 순서 + 무결성      2-1 전수 검증 (8가지 조합)
                    1-2 방어 + ApproxSize        2-2 고빈도 경합 + corruption
─────────────────────────────────────────────────────────────────
LockFreeQueue       1-1 FIFO 순서 + 무결성      2-1 전수 검증 (8가지 조합)
                    1-2 방어 + Clear             2-2 고빈도 경합 + corruption
─────────────────────────────────────────────────────────────────
반복 횟수           50만 회                      스레드당 1천만 회 (2~32T)
```

---

## 검증 방식

- **TestPayload** (16바이트): `magic(0xDEADBEEF)` + `threadId` + `sequence` + `checksum(XOR)` — 데이터 corruption 감지용
- **LifecycleTracker**: atomic 카운터로 생성자/소멸자 호출 횟수 추적 — PlacementNew 검증용
- **TEST_ASSERT**: 조건 실패 시 즉시 크래시 (덤프 생성)

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

### LockFreeStack (4개 Phase)

| Phase | 구분 | 내용 |
|-------|------|------|
| 1-1 | 싱글, 50만 회 | 랜덤 배치 Push → 전부 Pop하며 LIFO 역순 + checksum 검증 |
| 1-2 | 싱글 | 빈 스택 Pop 방어, ApproxSize +1/-1 정합성(1만 회), Push/Pop 스트레스 |
| 2-1 | MT, 8가지 조합 | Producer가 고유 숫자 Push, Consumer가 Pop. 모든 숫자가 정확히 1번만 Pop되었는지 전수 검증 |
| 2-2 | MT, 2~32 스레드 | 짝수 스레드 Push(checksum 기록), 홀수 스레드 Pop(checksum 검증). 잔여 drain 후 Push 수 == Pop 수 확인 |

### LockFreeQueue (4개 Phase)

| Phase | 구분 | 내용 |
|-------|------|------|
| 1-1 | 싱글, 50만 회 | 랜덤 배치 Enqueue → 전부 Dequeue하며 FIFO 순서 + checksum 검증 |
| 1-2 | 싱글 | 빈 큐 Dequeue 방어, ApproxSize +1/-1 정합성(1만 회), Clear 검증, Enqueue/Dequeue 스트레스 |
| 2-1 | MT, 8가지 조합 | Producer가 고유 숫자 Enqueue, Consumer가 Dequeue. 모든 숫자가 정확히 1번만 Dequeue되었는지 전수 검증 |
| 2-2 | MT, 2~32 스레드 | 짝수 스레드 Enqueue(checksum 기록), 홀수 스레드 Dequeue(checksum 검증). 잔여 drain 후 Enqueue 수 == Dequeue 수 확인 |

---

## 반복 횟수 설계 근거

- **Phase 1 (싱글 스레드)**: 50만 회. 기능 검증이므로 로직 결함은 수천 회 이내에 발견됨. 과도한 반복 불필요.
- **Phase 2 (멀티스레드)**: 스레드당 1천만 회. CAS 경합의 race window 노출에는 높은 반복이 필요. 32스레드 기준 총 3.2억 회 경합.

---

## 실행 방법

실행 시 메뉴에서 선택:
1. InternalFreeList 전체 테스트
2. LockFreeStack 전체 테스트
3. LockFreeQueue 전체 테스트
4. 전체 통합 테스트
