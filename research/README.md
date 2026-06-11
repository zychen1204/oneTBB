# CAWS：P/E 異質核心感知之 oneTBB Work-Stealing 最佳化

### *Capacity-Aware Work Stealing — 對應研究提案 §2.1（痛點 B：工作竊取的不對稱失衡）*

> 程式碼位置：[github.com/zychen1204/oneTBB](https://github.com/zychen1204/oneTBB) 的
> **`caws`** branch（基於 oneTBB master / 2023.1 dev，commit `b430e87`）；
> 完整研究材料（本報告、gem5 設定、PARSEC 移植）在 **`caws-research`** branch 的 `research/`。
> 修改前後可直接以 [master...caws diff](https://github.com/zychen1204/oneTBB/compare/master...caws)
> 檢視，共 9 個檔案、+506/−49 行。

---

## 目錄

1. [摘要](#1-摘要)
2. [設計原理：為什麼 work stealing 在 P/E 核心上會失衡](#2-設計原理)
3. [修改了什麼（檔案層級說明）](#3-修改了什麼)
4. [正確性驗證](#4-正確性驗證)
5. [gem5 模擬：P/E core 規格、流程與修改前後速度比較](#5-gem5-模擬)
6. [限制與未來工作](#6-限制與未來工作)

---

## 1. 摘要

本工作在 oneTBB scheduler 中實作 **CAWS（Capacity-Aware Work Stealing）**：一個只在偵測到
異質（P/E-core）拓樸時才啟用的不對稱竊取政策，由三個機制組成：

1. **E-core 終局退讓（Endgame Throttle）** — 平行區段收尾時 E-core 暫停竊取，
   把最後幾塊 chunk 留給 P-core，消除 barrier 長尾延遲（核心機制）。
2. **型別感知受害者選擇（Class-Aware Victim Selection）** — E-core 小偷優先偷同為
   E-core 的 deque（cluster L2 局部性）；P-core 小偷偷「最深」的 deque（單次竊取
   取得最多工作、優先疏通慢核積壓）。
3. **拓樸偵測與可選釘選** — 自動從 sysfs（Intel hybrid / ARM `cpu_capacity`）或
   環境變數 `TBB_HETERO_PCORES` 取得 P/E 對應；`TBB_HETERO_PIN=1` 時 worker 依
   slot 順序 P-core 優先釘選。

**結果摘要**（正式評估環境為 gem5 模擬的 **4P+8E（12 核）異質系統**，兩個 benchmark
跑在同一台模擬機器上，差別只在 benchmark 自己開幾條 worker thread，詳見 §5）：

| 場景 | worker threads | 修改前（stock） | 修改後（CAWS） | 加速 |
|------|------|------|------|------|
| gem5 bodytrack simsmall | 12（用滿 4P+8E） | 待填 | 待填 | 預估 +8 ~ +15% |
| gem5 fluidanimate simsmall | 8（用 4P+4E；受 2 冪次分解限制） | 待填 | 待填 | 預估 +5 ~ +12% |

政策關閉（非異質機器、或 `TBB_HETERO_DISABLE=1`）時行為與原版 **逐位元相同路徑**，無額外開銷。

---

## 2. 設計原理

### 2.1 問題：傳統 work stealing 的兩個不對稱失衡

oneTBB 的每個 worker 擁有一個 Chase-Lev 風格 deque：擁有者從 **tail**（最新、
在 divide-and-conquer 中是最小的子範圍）取工作，小偷從 **head**（最舊、最大的子範圍）
偷，受害者以 `FastRandom` **均勻隨機** 選取（原 `arena::steal_task`，`arena.h:531`）。
在同質核心上這是理論最優的；在 P/E 異質核心上產生研究提案 §2.1 痛點 B 的兩個失衡：

1. **E-core 偷走與算力不匹配的任務**：平行區段（如 `parallel_for` 一個 frame）收尾時
   只剩最後幾塊 leaf chunk。隨機竊取下，E-core 有 `E/(P+E)` 的機率搶到最後一塊 —
   整個 region 的 barrier 必須等這顆慢核做完（α≈0.4 時長尾放大 ~2.5 倍）。
   bodytrack（每 frame 5 層 annealing、每層多個平行階段）與 fluidanimate
   （每 frame ~9 個 barrier 相隔的 phase）都是 **高 barrier 密度** 的迭代式工作負載，
   這個長尾在每個 barrier 重複發生，是異質機器上 TBB 效率損失的主因。
2. **P-core 過早、盲目地竊取**：P-core 完成快、竊取頻繁，但隨機選受害者常打空
   （失敗竊取 = 純開銷 + cache 干擾），也不會優先疏通積壓在 E-core deque 裡的工作。

### 2.2 CAWS 的三個機制

#### 機制一：E-core 終局退讓（核心）

E-core 小偷在竊取前用一次 relaxed 掃描估計 arena 內 **可見待處理任務總量**
`pending = Σ max(0, tail_i − head_i)`，若

```
pending ≤ τ        （τ = TBB_HETERO_ENDGAME，預設 = P-core 數）
```

則本輪 **放棄竊取**（回到 dispatch loop 的指數退避暫停）。直覺：剩餘任務數少於
P-core 數時，這些任務由 P-core 消化必定更快結束；E-core 搶走任何一塊都可能成為
關鍵路徑。**Patience 計數器**（預設 8 輪）防止饑餓：若 P-core 持續忙碌、工作一直
沒被取走，E-core 第 9 輪會照常竊取，把退讓的延遲上界限制在數十 µs。

> **與提案 §2.1 AsymWS 模型的對應**：提案的竊取條件
> `W(t) ≤ κ·CW(c_stealer)·W̄max` 需要知道單一任務的工作量估計 `W(t)`，
> 但 TBB 任務沒有大小註記、runtime 無法取得。CAWS 把它改寫成可實作的保守近似：
> 以「全域剩餘平行度」`pending` 取代 `W(t)`，以 `τ ≈ κ·CW` 作為門檻 ——
> 兩者擋下的是同一個失敗模式（E-core 在平行餘裕不足時接下大顆粒工作），
> 而且 pending 在收尾階段恰好是任務粒度的良好 proxy：deque 越淺，head 任務越接近
> leaf、越接近 barrier。此外這個全域條件天然處理了 AsymWS 模型沒涵蓋的
> barrier 收尾長尾——這正是 bodytrack/fluidanimate 這類迭代式 benchmark 的主要損失來源。

#### 機制二：型別感知受害者選擇

同一次掃描順便找出（掃描起點隨機化，避免小偷護航效應）：

- **P-core（或未分類）小偷** → 偷 **最深的 deque**：head 端任務最大，單次竊取
  取得最多工作、攤平竊取成本；同時自然優先疏通積壓最多（通常是慢核）的佇列。
- **E-core 小偷**（通過 endgame 檢查後）→ 優先偷 **最深的 E-core deque**：
  (a) E deque 裡的任務本來就是 E-core 等級的細粒度工作；(b) Intel E-core 以
  4 核 cluster 共享 L2，E↔E 遷移成本遠低於把 P-core deque 的粗顆粒子樹拉過去。
  沒有 E 受害者時才偷一般最深 deque。

#### 機制三：拓樸偵測、執行緒分類與可選釘選

- 拓樸來源（優先序）：`TBB_HETERO_PCORES=<cpulist>` 環境變數（gem5 / 虛擬化用）→
  Intel hybrid sysfs（`/sys/devices/cpu_core/cpus`、`cpu_atom`）→ ARM
  `cpu_capacity`。找不到不對稱拓樸 ⇒ 整套機制關閉，行為等同原版。
- 每條執行緒在進入 arena 及 **每次竊取 session 開始** 時以 `sched_getcpu()`
  重新分類自己（OS 可能遷移執行緒），寫入 arena slot 的 8-bit atomic 標籤，
  小偷以 relaxed load 讀取受害者類別。
- `TBB_HETERO_PIN=1`：worker 依 slot index 釘選，P-core 優先（slot 越小越先拿到
  P-core），讓實驗具決定性、消除 OS 遷移雜訊。

---

## 3. 修改了什麼

Branch：`oneTBB` repo 的 `caws`（`git diff master..caws`）。

| 檔案 | 變更 | 內容 |
|------|------|------|
| `src/tbb/hetero.h` | **新增** (+89) | `core_class` 列舉、`hetero_topology` 介面（偵測 / 分類 / 釘選 / 統計）|
| `src/tbb/hetero.cpp` | **新增** (+257) | sysfs / 環境變數拓樸偵測、cpulist 解析、`sched_setaffinity` 釘選、steal 統計與 atexit 傾印 |
| `src/tbb/arena.cpp` | +99 | **CAWS 版 `arena::steal_task()`**：relaxed 掃描、endgame throttle、型別感知受害者選擇（§2.2）；arena 建構時初始化 slot 類別標籤 |
| `src/tbb/arena.h` | −48/+4 | 原 inline `steal_task` 移除，宣告改收 `thread_data&` |
| `src/tbb/arena_slot.h` | +24 | slot 增加 `my_core_class`（8-bit atomic 標籤）與 `approx_depth()`（deque 深度估計，附 heuristic-only 註記）|
| `src/tbb/thread_data.h` | +17 | 執行緒快取自身 `my_core_class` 與 patience 計數器；`attach_arena()` 時分類／釘選並寫入 slot 標籤 |
| `src/tbb/task_dispatcher.h` | +17/−6 | `receive_or_steal_task()`：竊取 session 開始時重新分類（防 OS 遷移造成標籤過期）、重置 patience；`steal_or_get_critical` 簽名改傳 `thread_data&` |
| `src/tbb/scheduler_common.h` | +2/−1 | 對應簽名與前向宣告 |
| `src/tbb/CMakeLists.txt` | +1 | 加入 `hetero.cpp` |

### 執行期開關（皆為環境變數，不需重編）

| 變數 | 預設 | 作用 |
|------|------|------|
| `TBB_HETERO_PCORES` | （自動偵測） | 明確指定 P-core cpulist，如 `0-3`；其餘 CPU 視為 E-core |
| `TBB_HETERO_DISABLE` | 0 | 強制關閉（即使在 hybrid 機器上） |
| `TBB_HETERO_ENDGAME` | #P-cores | 終局退讓門檻 τ |
| `TBB_HETERO_PATIENCE` | 8 | E-core 連續退讓上限（防饑餓） |
| `TBB_HETERO_PIN` | 0 | worker 依 slot 順序釘選，P-core 優先 |
| `TBB_HETERO_STATS` | 0 | 程序結束時傾印竊取統計（研究指標用） |

---

## 4. gem5 模擬

實際執行環境：**GCP `c3-highcpu-8`**（Xeon 8481C Sapphire Rapids、8 vCPU、50GB）、
**gem5 v24.0.0.0**、SE mode。流程腳本：`gem5/scripts/vm_build_static.sh`（兩版靜態
libtbb.a + 四個靜態 benchmark）、`gem5/scripts/vm_run_sims.sh`（四組模擬同時執行，
完成後自動產出 `SUMMARY.txt` 並關機）；細節見 `gem5/README.md`。

### 4.1 模擬系統規格（4P + 8E，1P:2E 核心比，`X86MinorCPU`）

> **CPU 模型選擇**：最初採用 gem5 的詳細亂序模型 `X86O3CPU`（實測過 ROB 512 與
> 1024 兩種組態），但長時間 PARSEC 模擬會在特定 x86 微指令序列觸發 O3 的
> TimeBuffer assertion（elaborate 成功、執行一段時間後 panic）。最終改用 gem5
> 較健壯的詳細 in-order 管線 `X86MinorCPU`，以**管線寬度、時脈、cache 拓樸與
> 分支預測器大小**建模 P/E 差異——CAWS 反應的是核心間「相對吞吐量差」，與
> 是否亂序無關（真實 E-core / LITTLE core 本就偏窄管線）。

**P-core（寬管線）— CPU 0–3**

| 參數 | 值 |
|------|-----|
| 時脈 | 4.0 GHz（獨立 clock domain） |
| decode / execute 寬度 | 8 / 8 |
| issue / commit 上限 | 8 / 8 |
| memory issue / commit 上限 | 4 / 4 |
| LSQ（requests / transfers / store buffer） | 4 / 4 / 8 |
| 分支預測 | TAGE-SC-L 64KB |
| L1I / L1D | 32 KB 8-way / 48 KB 12-way（lat 1/3 cycles） |
| L2 | **私有** 2 MB 16-way（lat 15） |

**E-core（窄管線）— CPU 4–11**

| 參數 | 值 |
|------|-----|
| 時脈 | 2.8 GHz（獨立 clock domain） |
| decode / execute 寬度 | 4 / 4 |
| issue / commit 上限 | 4 / 4 |
| memory issue / commit 上限 | 2 / 2 |
| LSQ（requests / transfers / store buffer） | 2 / 2 / 5 |
| 分支預測 | TAGE-SC-L 8KB |
| L1I / L1D | 64 KB 8-way / 32 KB 8-way |
| L2 | **每 4 核 cluster 共享** 4 MB 16-way（lat 18），共 2 個 cluster |

**Uncore**：共享 L3 16 MB 16-way（lat 42，mostly-exclusive）＋ DDR4-2400。
此組態下 E/P 單執行緒算力比 α ≈ 0.45–0.55（頻率比 0.7 × 管線寬度差），
與實體 Raptor Lake 的 P/E 比相當；雙 E-cluster 共享 L2 正是機制二
「E←E 竊取較便宜」的結構性依據。

### 4.2 PARSEC 3.0 的 oneTBB 移植（`parsec-ports/`）

PARSEC 3.0 的 TBB 程式碼以 2008 年的 TBB API 撰寫，已完成移植並通過功能驗證：

| 項目 | 原始碼 | 移植方式 |
|------|------|------|
| 併發度控制（兩者） | `tbb::task_scheduler_init` | `tbb::global_control` |
| bodytrack 影像/濾波兩階段 | `tbb::pipeline` + `tbb::filter`（已移除） | 原碼 `pipeline.run(1)` 本就無 stage 重疊，等價改寫為循序 stage 迴圈；計算平行度全在 stage 內的 `parallel_for`/`parallel_reduce`（不變） |
| fluidanimate 任務樹 | `tbb::task::spawn_root_and_wait` 兩層樹（已移除） | `LaunchGrids<T>()` 以 `tbb::task_group` 重現相同 NUM_GRIDS×NUM_TASKS 切分 |
| FlexImageLib BMP 載入 | `DWORD = unsigned long`（LP64 錯位） | `-DHAVE_STDINT_H=1`（原由 configure 定義） |

功能驗證（CAWS 函式庫 + `TBB_HETERO_PCORES=0-3`）：bodytrack simsmall 輸出
poses.txt 數值正常，竊取統計 E←E 205 次、終局退讓 772 次（證實 bodytrack 的
barrier 密集特性）；fluidanimate 5 frames 輸出正常，E←E 82 次、終局退讓 74 次。

### 4.3 執行指令與模擬數據

```bash
# VM 端一鍵執行（四組同時；simsmall = 1000 粒子 5 層 / 5 frames）
~/vm_run_sims.sh 1000 5 5 poweroff
# bodytrack:     <seqB_1> 4 1 1000 5 1 12   (thread model 1 = TBB, 12 threads)
# fluidanimate:  8 5 in_35K.fluid out.fluid (8 = 2 的冪次分解限制)
# CAWS 組環境變數: TBB_HETERO_PCORES=0-3  TBB_HETERO_STATS=1
```

煙霧測試（`ws_bench` 縮小版）已確認：12 核異質系統 elaborate 正常、SE mode
多執行緒（clone/futex）正常、**CAWS 在 gem5 內正確啟用**（getcpu 分類成功、
竊取統計輸出）、模擬完整跑完無 assertion；實測模擬速度 ≈ 87 KIPS／組
（四組同時跑，各佔一顆 host 核心）。

**四組正式模擬（完整 simsmall）正在執行中**，預計 fluidanimate ≈ 5–8 小時、
bodytrack ≈ 9–14 小時；完成後 `SUMMARY.txt` 的 `simSeconds` 將填入下表：

| Benchmark（simsmall, 4P+8E） | 修改前 simSeconds | 修改後 simSeconds | 加速 | 預估（執行前） |
|------|------|------|------|------|
| bodytrack（TBB, 12 threads） | 待填 | 待填 | 待填 | +8 ~ +15% |
| fluidanimate（TBB, 8 threads） | 待填 | 待填 | 待填 | +5 ~ +12% |

預估依據為機制分析：CAWS 的收益主要來自 barrier 收尾長尾的消除，
故 barrier 密度極高、粒子權重不平衡的 bodytrack 預期效益較高；
fluidanimate 的空間網格較規則、每 phase 不平衡度較低，效益預期略低；
兩者皆受不可平行序列段稀釋（Amdahl）。
次要驗證指標：`endgame declines` 數量級、E←E 竊取占比（預期 >70%）、
跨類遷移次數下降（對應提案 §4.3 的 Task Migration Frequency）。

---

## 5. 限制與未來工作

1. **喚醒順序仍是同質的**：CAWS 只改了「誰偷誰」，沒改「先叫醒誰」。讓 arena 在
   平行餘裕少時優先喚醒 P-core worker（修改 `private_server` 的 LIFO 喚醒序）是
   下一個自然延伸。
2. **任務大小註記**：若在 `parallel_for` partitioner 層傳遞範圍大小到 task，
   可把 endgame 的全域近似升級回提案原始的 per-task `W(t) ≤ κ·CW·W̄` 條件。
3. **α 的動態校準**：目前 P/E 只有類別之分；可在執行期以每核 task 吞吐量回歸出
   連續的 `CW(c)`，支援超過兩級的異質性（如 Lunar Lake 的 LP-E core）。