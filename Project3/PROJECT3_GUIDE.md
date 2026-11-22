# Project 3: Virtual Memory — 구현 및 서버 검증 안내

## 현재 상태와 기준

- 기준: `Project3/Proj3.pptx`와 `Project3/document.docx`, 제공 Pintos 문서·소스·테스트.
- 구현: SPT, 지연 적재, 스택 확장, Second Chance, 스왑, 파일 매핑, 시스템 콜 연동, 종료 정리.
- 로컬 검증: WSL Ubuntu / GCC 13.3.0 / GNU Make 4.3에서 `vm`, `userprog`, `threads` 빌드. 변경 C 파일의 VM 사용·미사용 구성에 경고를 오류로 취급하는 문법 검사 수행.
- **서버 실행 검증 완료:** 구현 커밋 `2d1cdad`를 서버에 반영하고 clean·재빌드한 뒤 `vm` 구성의 `make check`에서 **`All 113 tests passed.`**를 확인했다. 지정 16개와 mmap·userprog·filesys/base 테스트를 포함한다. 별도 threads 커널의 실행 결과는 포함하지 않는다.
- 완성 보고서 본문: `Project3/20221599.docx`. 표지는 기존 Project2 보고서의 이수영 / 20221599 / 박성용을 사용했다. 원본 `document.docx`는 보존했다.
- **보고서 IV.C에 서버 113개 통과 결과를 반영했다.** 실제 결과 화면 캡처 두 장은 사용자가 표시된 위치에 삽입한다.

## 1. 구현 범위와 파일

커널 변경은 기존 6개 파일과 새 VM 소스·헤더 2개로 제한했다. 기존 채점 파일과 테스트는 변경하지 않았다. `#ifdef VM`으로 비 VM 커널의 기존 적재 방식을 유지한다.

| 파일 (`pintos/src/` 기준) | 변경 내용 |
|---|---|
| `vm/vm.c` (추가) | 페이지 해시, 프레임 목록, Clock, 스왑, 스택 판정, 매핑·종료 정리 |
| `vm/vm.h` (추가) | 프로세스·예외·시스템 콜이 사용하는 작은 공개 인터페이스 |
| `Makefile.build` | `vm/vm.c` 빌드 등록 |
| `threads/init.c` | 파일 시스템·스왑 블록 장치 초기화 후 `vm_init()` |
| `threads/thread.h` | 프로세스별 VM 공간과 사용자 ESP 포인터 |
| `userprog/process.c` | SPT 초기화, 실행 파일 지연 등록, 초기 스택 pin, VM 해제 |
| `userprog/exception.c` | 유효한 미상주 페이지 복구, 권한 위반·잘못된 접근 종료 |
| `userprog/syscall.c` | SPT 기반 검사, 페이지 단위 pin·복사·I/O, `mmap`/`munmap` |

### SPT와 지연 적재 — 명세서 7~12슬라이드

프로세스마다 `vm_space`가 페이지 해시와 매핑 목록을 가진다. `vm_page`는 가상 페이지 주소, 프레임, 소유자, 쓰기 권한, pin 수, 파일 원본과 오프셋·유효 바이트, 스왑 슬롯, private 수정 이력을 저장한다.

실행 파일 적재 시에는 페이지를 즉시 읽지 않고 `vm_add_file()`로 등록한다. CPU가 미상주 페이지에 접근하면 CR2의 주소와 오류 코드를 확인하고 `vm_pin()` → `claim()`으로 복구한다. 프레임에 내용을 채우고 PTE를 연결한 뒤 예외에서 복귀하여 같은 명령을 재실행한다. 읽기 전용 페이지 쓰기는 적재로 해결하지 않고 `exit(-1)` 처리한다.

초기 스택은 기존 인자 전달을 위해 즉시 적재하고, 인자를 쓰는 동안 고정한다. 실행 파일은 지연 적재 원본이므로 프로세스 종료까지 열어 두고 쓰기를 금지한다.

### Second Chance와 스왑 — 명세서 4·12~13슬라이드

전역 상주 페이지 목록에 Clock 포인터를 둔다. pin된 페이지는 건너뛴다. 사용자 매핑 또는 커널 별칭의 accessed bit가 1이면 두 별칭의 bit를 지우고 다음 후보로 이동한다. A bit가 0인 교체 가능한 후보를 고른다.

| 페이지 상태 | 교체 시 보존 방식 |
|---|---|
| 변경 없고 원본에서 재구성 가능한 실행 파일·0 페이지 | 프레임 내용을 버리고 원본 메타데이터 유지 |
| 수정됐거나 과거 수정 이력이 있는 private 페이지 | 스왑 슬롯에 전체 페이지 저장 |
| dirty `mmap` 페이지 | 원본 파일의 유효 바이트만 기록 |
| clean `mmap` 페이지 | 쓰기 없이 버리고 필요할 때 파일에서 재적재 |

`private_data`는 중요한 불변식이다. 수정된 페이지를 swap-out → swap-in한 뒤 다시 쓰지 않았더라도, 원래 실행 파일이나 0 페이지로 복구할 수는 없다. 따라서 수정 이력을 유지하고 재교체 때에도 스왑에 보존한다.

스왑 슬롯은 4 KiB이며 512바이트 섹터 8개를 읽고 쓴다. 슬롯 비트맵은 실제 swap-out 때 할당하고, swap-in과 PTE 연결에 성공한 뒤 반환한다. 종료 시 미상주 페이지의 슬롯도 반환한다. 스왑 장치가 없으면 원본으로 복구 가능한 clean 페이지만 버릴 수 있다. 스왑이 가득 차면 다른 후보를 찾고, 확보할 수 없으면 할당 실패를 반환한다.

### 스택 확장 — 명세서 14~16슬라이드

SPT에 없는 주소에 대해 아래 조건을 모두 검사한다.

1. 첫 페이지를 제외한 사용자 주소 공간에 있다.
2. `PHYS_BASE - 8 MiB` 이상이다.
3. 유효한 사용자 ESP가 있고, `fault_addr >= user_esp - 32`이다.

32바이트 허용은 PUSH/PUSHA의 ESP 아래 접근을 처리하기 위한 것이다. ESP를 먼저 크게 낮추고 그 위의 지역변수를 사용하는 경우도 허용한다. 페이지는 실제로 접근할 때 추가하므로 미접근 구간을 일괄 할당하지 않는다. **8 MiB는 구현에서 선택한 상한이며 명세서가 강제한 채점값이라고 확정한 것이 아니다.** 스택 축소는 구현하지 않았다.

### 시스템 콜과 잠금

시스템 콜 진입 시 인자부터 읽기 전에 사용자 ESP를 보관한다. 커널 모드에서 발생한 사용자 버퍼의 폴트에는 이 값을 사용한다.

사용자 버퍼를 검증할 때 현재 PTE가 없다는 이유만으로 종료하지 않는다. SPT와 스택 확장 가능성을 확인하고, 실제 복사·I/O 시 한 페이지씩 적재·고정한다. 큰 버퍼 전체를 pin하지 않아 물리 메모리보다 큰 버퍼도 순차 처리할 수 있다. 페이지 경계에 걸친 시스템 콜 인자도 나누어 복사한다.

파일 I/O는 `pin → 파일 시스템 잠금 → I/O → 파일 시스템 잠금 해제 → unpin` 순서다. VM 내부의 잠금 순서는 VM → 파일 시스템으로 통일한다. ELF 헤더 적재 중에도 VM 호출 전에는 파일 잠금을 해제하여 순환 대기를 피한다. dirty bit 확인과 희생자 PTE 해제 사이에는 인터럽트를 잠시 꺼 사용자 쓰기와의 경쟁을 막고, 블록 I/O 전에는 다시 켠다.

모든 프레임이 고정되어 있으면 조건변수로 기다리고 unpin/해제 시 깨운다. 전역 VM 잠금은 I/O 중에도 유지하므로 구현이 단순하지만 여러 프로세스의 폴트 처리량에는 제한이 있다. 짧은 콘솔 출력은 512바이트 커널 버퍼로 복사한 뒤 한 번에 출력한다.

### `mmap` / `munmap`

필수 목록의 `page-merge-mm`이 실제 `mmap()`을 호출하므로 포함했다. 주소 정렬·사용자 범위·빈 파일·기존 SPT와의 중복을 검사하고, 실패 시 이번 호출이 추가한 항목만 롤백한다. 파일을 재개방하므로 원래 FD가 닫히거나 파일 이름이 제거되어도 매핑은 유지된다.

매핑 페이지는 지연 적재하며 교체·해제 시 dirty일 때만 파일에 반영한다. 마지막 페이지의 파일 끝 뒤 0 채움 부분은 기록하지 않는다. 매핑 ID는 프로세스별로 관리하고, 자식 프로세스에 매핑을 상속하지 않는다. 종료 시에도 남은 매핑을 해제한다.

파일 writeback이 실패하면 교체 경로는 기존 PTE를 복구하고 다른 후보를 찾는다. 명시적 munmap은 매핑을 유지한 채 실패를 반환하고 시스템 콜에서 프로세스를 종료한다. 종료 중 재시도도 실패하면 오류를 출력하고 자원을 회수하므로 그 변경의 영속성을 보장할 수 없다.

### 종료와 비용

`vm_destroy()`는 파일 매핑 반영·해제, 상주 프레임과 스왑 슬롯 반환, SPT 해제를 수행한다. 프레임을 해제하기 전에 PTE를 지워 기존 `pagedir_destroy()`가 이중 해제하지 않게 한다. 그 뒤 FD와 실행 파일을 닫고 페이지 디렉터리를 파괴한다.

페이지 수 P, 프레임 수 F, 스왑 슬롯 수 S에 대해 SPT 탐색은 평균 O(1)·최악 O(P), Clock 후보 검사는 한 시도당 최대 O(F), 슬롯 탐색은 최악 O(S)이다. 모든 프레임이 pin된 경우 대기 시간은 별도다. 공유 페이지, Copy-on-Write, 스택 축소는 범위에 넣지 않았다.

## 2. 서버에서 시작하기

아래 명령은 **저장소 루트**에서 Bash로 실행한다. 현재 서버 변경사항이 있으면 먼저 보관한다. 로컬에서 만든 기능 커밋이 원격 `local` 브랜치에 올라온 뒤 갱신한다.

```bash
git status --short
git fetch origin
git switch local
git pull --ff-only origin local
git log -5 --oneline
export PATH="$PWD/pintos/src/utils:$PATH"
command -v gcc make perl pintos qemu-system-i386
```

기존 서버의 Pintos 도구가 별도 경로에 설치되어 있다면 그 환경을 사용해도 된다. `pintos` 스크립트는 보통 `qemu-system-i386`을 사용한다. `qemu-system-x86_64`만 있다면 서버의 기존 실행 설정부터 확인한다.

## 3. 명세서 지정 16개 테스트부터 실행

빌드 캐시와 과거 테스트 결과를 제거하고 시작한다. 저장소 루트에서 실행한다.

```bash
set -o pipefail
make -C pintos/src/vm clean
make -C pintos/src/vm -j4 2>&1 | tee Project3/vm-build.log

tests=(
  pt-grow-stack pt-grow-stk-sc pt-big-stk-obj pt-grow-pusha
  page-linear page-parallel page-shuffle
  page-merge-seq page-merge-par page-merge-mm page-merge-stk
  pt-bad-addr pt-bad-read pt-write-code pt-write-code2 pt-grow-bad
)
selected=$(printf 'tests/vm/%s ' "${tests[@]}")
make -C pintos/src/vm check TESTS="$selected" 2>&1 | tee Project3/vm-required-check.log
```

위 16개만 선택한 실행의 정상 완료 기준은 **`All 16 tests passed.`**이다. 이번에 확인한 결과는 아래 4절의 전체 실행인 **`All 113 tests passed.`**이며, 지정 16개도 포함하여 모두 통과했다. 잘못된 접근 테스트는 자식 프로그램이 `exit(-1)`로 종료되어야 PASS이므로, 프로그램 종료 자체를 실패로 오해하지 않는다.

테스트 규칙은 VM 구성에서 임시 4 MiB 스왑 디스크를 자동으로 붙인다. `make check`에는 별도로 `swap.dsk`를 만들 필요가 없다. 단독으로 Pintos를 실행할 때는 `--swap-size=4` 등 실행 옵션이 필요하다. 병합·셔플 테스트는 다른 테스트보다 오래 걸리며 제공 규칙에 일부 300~600초 제한이 있다.

## 4. 전체 VM 테스트 및 이전 기능 회귀 검사

**16개만 지정했던 `TESTS`를 제거하고 clean부터 다시 실행한다.** 기존 `results` 파일이 다른 목록에 대해 생성된 결과일 수 있으므로 새 빌드에서 확인한다.

```bash
make -C pintos/src/vm clean
make -C pintos/src/vm -j4
make -C pintos/src/vm check 2>&1 | tee Project3/vm-all-check.log
```

이 명령은 VM/mmap뿐 아니라 같은 VM 커널에서 실행하는 userprog와 filesys/base 테스트도 포함한다. 명세서의 지정 16개보다 범위가 넓다. `make grade`의 제공 가중치를 강의 점수로 그대로 단정하지 않는다.

기존 커널 구성도 별도로 검사한다.

```bash
make -C pintos/src/userprog clean
make -C pintos/src/userprog -j4
make -C pintos/src/userprog check 2>&1 | tee Project3/userprog-check.log

make -C pintos/src/threads clean
make -C pintos/src/threads -j4
make -C pintos/src/threads check 2>&1 | tee Project3/threads-check.log
```

## 5. 실패를 확인하고 한 테스트 재실행하기

```bash
cat pintos/src/vm/build/tests/vm/pt-grow-stk-sc.result
cat pintos/src/vm/build/tests/vm/pt-grow-stk-sc.output
cat pintos/src/vm/build/tests/vm/pt-grow-stk-sc.errors
```

재실행할 때는 이전 결과를 강제로 갱신한다. 아래 `-B`는 선택한 대상과 의존성을 다시 만든다.

```bash
make -B -C pintos/src/vm/build tests/vm/pt-grow-stk-sc.result
```

다른 테스트는 위 경로의 이름을 바꾼다. 수정 후 전체 판정은 다시 clean → build → check로 확인한다.

| 증상 | 먼저 확인할 내용 |
|---|---|
| `qemu-system-i386: not found` / `pintos: not found` | 서버 도구 설치와 PATH. 커널의 FAIL과 구분 |
| `pt-grow-stk-sc` 실패 | 저장한 사용자 ESP, SPT 판정, read 버퍼 writable 검사 |
| `pt-grow-pusha` / `pt-grow-bad` 실패 | ESP−32 경계와 임의의 낮은 주소 거부 |
| `pt-write-code*` 실패 | 폴트의 쓰기 권한, 시스템 콜이 쓰는 버퍼의 권한 |
| `page-shuffle` 데이터 불일치 | private 수정 이력, swap-in/out 슬롯 수명, 별칭 dirty bit |
| 병렬 병합 테스트 정지 | pin 해제, VM → 파일 시스템 잠금 순서, 스왑 장치 |
| `page-merge-mm` / `mmap-exit` 실패 | 자식 종료 전 dirty 매핑 파일 반영 |
| `mmap-clean` 실패 | 변경 없는 매핑의 불필요한 파일 덮어쓰기 |
| 종료 후 panic / palloc assert | Clock 포인터, PTE 해제와 프레임 중복 반환 |

실패 로그를 공유할 때는 `git rev-parse HEAD`, 실행 명령, `.output`, `.errors`, `.result`를 함께 보관한다. 민감한 서버 접속 정보는 넣지 않는다.

## 6. 보고서와 제출

명세서의 마감은 2026-11-22 23:59이다. 개인 과제이며 구현 80점·보고서 20점이다. 지정 테스트 표의 원점수 합계와 구현 총점은 일치하지 않으므로 환산 점수는 추가 공지 없이 확정하지 않는다.

1. `Project3/20221599.docx`의 IV.C에 서버에서 실제 실행한 명령, 결과 개수, 실패 및 수정 내용을 기록한다.
2. 명세서가 요구한 **실제 `make check` 결과 화면 캡처**를 삽입한다. 로컬 빌드 성공 문구를 실행 테스트 결과로 바꾸어 적지 않는다.
3. 원본 양식의 안내문 대신 채워진 본문과 흐름도, 표지의 이름·학번·교수를 확인한다.
4. 서버에서 빌드·로그를 보관한 뒤 제출용으로 빌드 산출물을 정리한다.

저장소 루트에서 보고서를 옮기고 제공 스크립트를 실행한다.

```bash
cp Project3/20221599.docx pintos/20221599.docx
cd pintos
bash submit.sh
```

입력은 프로젝트 번호 `3`, 학번 `20221599`이다. 기존 `20221599` 디렉터리가 실제로 없는지, DOCX가 해당 위치에 있는지 먼저 확인하고 질문에 답한다. 스크립트는 `make clean`을 실행하고 임시 학번 폴더를 만들었다가 삭제하므로 기존 폴더를 두고 실행하지 않는다.

```bash
tar -tzf os_prj3_20221599.tar.gz
verify_dir=$(mktemp -d)
tar -xzf os_prj3_20221599.tar.gz -C "$verify_dir"
find "$verify_dir/20221599" -maxdepth 2 -type d
ls -l "$verify_dir/20221599/20221599.docx"
```

최상위는 `20221599/`, 그 안에는 `src/`와 `20221599.docx`가 있어야 한다. 저장소 전체, `.git`, 강의 슬라이드, 작업용 로그는 제출물에 포함하지 않는다. 압축 해제본의 소스와 보고서를 직접 확인한 뒤 e-class에 업로드한다. 이 작업에서는 서버 제출·e-class 업로드를 수행하지 않았다.

## 7. 변경 이력

- `daeddbf`: SPT, 지연 적재, 스택 확장, 시스템 콜 페이지 고정.
- `8859e74`: Second Chance, 스왑 슬롯 및 페이지 복구.
- `0ad0bf1`: mmap/munmap, dirty writeback, 매핑 종료 정리 및 경쟁 보완.
- `173247a`: 짧은 콘솔 출력의 원자성 유지, 비 VM 문자열 실패 시 정리 보완.

추가 문서 커밋과 최신 상태는 `git log --oneline`으로 확인한다. 원격 푸시 여부와 실행 테스트 여부는 별개의 상태다.
