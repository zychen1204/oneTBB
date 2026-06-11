# gem5 異質 P/E-core 模擬流程（CAWS oneTBB 評估）

評估 `oneTBB` **`caws`** branch（Capacity-Aware Work Stealing）對 PARSEC 3.0
bodytrack / fluidanimate 的效益。實際執行環境：GCP `c3-highcpu-8`
（Sapphire Rapids）、gem5 **v24.0.0.0**、SE mode。

## 模擬系統（`configs/hetero_pe_se.py`，預設值）

| | P-core ×4（CPU 0–3） | E-core ×8（CPU 4–11） |
|---|---|---|
| 模型 | X86O3CPU，8-wide | X86O3CPU，4-wide |
| 時脈 | 4.0 GHz | 2.8 GHz |
| ROB / IQ | 512 / 200 | 256 / 64 |
| L1I / L1D | 32K / 48K | 64K / 32K |
| L2 | 私有 2 MB | **每 4 核 cluster 共享 4 MB**（共 2 個 cluster） |
| 分支預測 | TAGE-SC-L 64KB | TAGE-SC-L 8KB |

Uncore：共享 L3 16 MB + DDR4-2400。CPU 編號 0–3 為 P-core，
與 workload 環境變數 `TBB_HETERO_PCORES=0-3` 一致。

## 目錄對應

| 檔案 | 用途 |
|---|---|
| `configs/hetero_pe_se.py` | 異質系統定義（`--num-p/--num-e/--p-clock/--e-clock` 可調） |
| `scripts/vm_build_static.sh` | VM 端：編兩版靜態 `libtbb.a`（master=stock / caws）+ 四個靜態 benchmark + `ws_bench` |
| `scripts/vm_run_sims.sh` | VM 端：同時啟動四組模擬（benchmark × 版本），跑完寫 `SUMMARY.txt` 並可自動關機 |
| `../parsec-ports/` | **已移植到 oneTBB** 的 bodytrack / fluidanimate TBB 版原始碼 + `build.sh` |

## 重現步驟

```bash
# 1. VM（一台即可）
gcloud compute instances create gem5-caws --machine-type=c3-highcpu-8 \
  --image-family=ubuntu-2404-lts-amd64 --image-project=ubuntu-os-cloud \
  --boot-disk-size=50GB --boot-disk-type=pd-balanced --zone=asia-east1-a

# 2. 上傳專案與輸入檔（輸入檔來源見下節）
gcloud compute scp --recurse ~/ControlStealing ~/parsec-ports /tmp/parsec-inputs \
  gem5-caws:~/ --zone=asia-east1-a

# 3. VM 內：依賴 + gem5（約 50 分鐘）
sudo apt install -y build-essential git scons python3-dev libprotobuf-dev \
    protobuf-compiler zlib1g-dev m4
git clone --branch v24.0.0.0 --depth 1 https://github.com/gem5/gem5 ~/gem5
cd ~/gem5 && scons build/X86/gem5.opt -j$(nproc)

# 4. 靜態建置 + 啟動四組模擬（simsmall：1000 粒子 5 層；5 frames）
~/vm_build_static.sh
~/vm_run_sims.sh 1000 5 5 poweroff

# 5. 結果
cat ~/results/SUMMARY.txt          # 每組的 simSeconds（越低越好）
grep -E "steals|declines" ~/results/*_caws/sim.log   # CAWS 竊取統計
```

## PARSEC 輸入檔

官方 parsec.cs.princeton.edu 已失聯；simsmall 輸入取自
`github.com/Multi2Sim/m2s-bench-parsec-3.0`（`bodytrack/data-small/sequenceB_1`、
`fluidanimate/data-small/in_35K.fluid`），與官方 simsmall 相同。

## oneTBB 移植摘要（`../parsec-ports/`）

PARSEC 3.0 的 TBB 程式碼用了 oneTBB 已移除的 API，移植內容：

| 舊 API | 移植方式 |
|---|---|
| `tbb::task_scheduler_init` | `tbb::global_control(max_allowed_parallelism, n)` |
| bodytrack `tbb::pipeline`/`tbb::filter` | 原碼為 `pipeline.run(1)`（無 stage 重疊），等價改寫為循序 stage 迴圈 |
| fluidanimate `tbb::task` 兩層 spawn 樹 | `LaunchGrids<T>()`：`tbb::task_group` 重現相同的 NUM_GRIDS×NUM_TASKS 切分 |
| （非 TBB）`FlexIO.h` 的 `DWORD=unsigned long` | 編譯加 `-DHAVE_STDINT_H=1`，否則 LP64 上 BMP 頭錯位讀不了輸入 |

## SE mode 注意事項

- gem5 SE 依 `clone()` 順序把執行緒放到閒置 CPU；TBB runtime 透過 `getcpu`
  syscall 分類執行緒（煙霧測試已驗證 v24 SE 可用）。**不要設** `TBB_HETERO_PIN`
  （SE 不支援 affinity 遷移）。
- 模擬速度實測約 **176 KIPS**（c3-highcpu-8，12 個 O3 模擬核心）；
  bodytrack simsmall 約 5–8 小時，fluidanimate simsmall 約 2–4 小時，
  四組可同時跑（gem5 單執行緒，8 vCPU 足夠）。
