# dodeca-core

在終端機裡，每個執行中的 Claude Code session 是一顆發光的多面體：白色細線外殼、藍色光暈、中心一顆像太陽的亮核。狀態改變時，顏色、轉速和核心的明暗跟著變。

做法與部分程式碼（`hook.py`、`install_hooks.py`、`src/events.c` 的事件讀取）改寫自 [zelvet-velvet/clawd-room](https://github.com/zelvet-velvet/clawd-room)（MIT License，Copyright (c) 2026 Zaviel）；多面體渲染、macOS 浮動面板與 pane 相關部分為本專案新寫。授權見 [LICENSE](LICENSE)。

## 狀態

| 狀態 | 觸發事件 | 畫面 |
|---|---|---|
| IDLE | `SessionStart`、完成後 | 白核藍暈，慢速旋轉 |
| THINK | `UserPromptSubmit`、`PostToolUse` | 黃色線條與光暈，核心呼吸式脈動 |
| WORK | `PreToolUse` | 白核藍暈，轉速加快，光點沿邊線遊走 |
| NEEDS YOU | `Notification`（閒置提醒除外） | 紅色線條，白核紅暈，核心故障式閃爍 |
| DONE | `Stop` | 核心爆亮加一圈衝擊波，2.5 秒後回 IDLE |
| SLEEP | IDLE 超過 90 秒 | 整體變暗，實心核仍看得見 |

`SessionEnd` 會移除那一顆；30 分鐘沒有任何事件的 session 也會移除。

## 需求

- C 編譯器與 `make`（macOS：`xcode-select --install`）
- zlib（macOS 內建）
- Python 3（只有 hook 和安裝腳本用到，僅標準函式庫）
- 支援 truecolor 的終端機；要看到細線和光暈，需要能顯示影像的終端機（iTerm2、kitty、Ghostty、WezTerm）

## 使用

```sh
make
./dodeca-core --demo          # 先用假的 session 看效果，q 離開

python3 install_hooks.py          # 只印出會寫進 ~/.claude/settings.json 的內容
python3 install_hooks.py --apply  # 實際寫入（會先備份）
./dodeca-core                     # 顯示真正的 session
```

- 安裝 hook 之後不要搬動這個資料夾，hook 是用絕對路徑呼叫的；搬了就再跑一次 `--apply`。
- 移除 hook：`python3 install_hooks.py --remove`。
- hook 只對安裝之後新開的 Claude Code session 生效。

## 側邊窗格（iTerm2）

`--pane` 是給窄窗格用的直排版面：session 由上到下，每個先兩行文字（名稱、狀態），下面才是多面體。文字用的是終端機本身的字體，所以跟同一個 profile 的其他窗格一樣大。

```sh
./dodeca-core --pane --demo                # 在一個窄窗格裡先看效果
python3 install_hooks.py --pane --apply    # 之後每次開 Claude Code 自動開窗格
```

加了 `--pane` 安裝之後，`SessionStart` 會多執行 `open_pane.sh`：把 Claude Code 所在的窗格往右切一塊跑 `dodeca-core --pane --exit-when-empty`，縮到 34 個字元寬，再把鍵盤焦點還給 Claude Code。已經有窗格開著就不會再開；最後一個 session 結束 15 秒後窗格自己關掉。

- 窗格寬度：環境變數 `DODECA_PANE_COLS`（預設 34）。
- 縮窄窗格要靠 iTerm2 的 Python API（`pane_api.py`）：AppleScript 能切窗格，但在全螢幕視窗裡不能調寬度。需要兩個一次性步驟：在 iTerm2 的 Settings > General > Magic 勾選 Enable Python API；再把腳本複製到 iTerm2 找腳本的資料夾：`mkdir -p ~/Library/Application\ Support/iTerm2/Scripts && cp pane_api.py ~/Library/Application\ Support/iTerm2/Scripts/dodeca_pane.py`（之後 `open_pane.sh` 會自動保持這份複本是最新的）。沒設定的話窗格會是一半寬，可以自己拖分隔線，面板會跟著變。
- 除錯：`DODECA_PANE_DEBUG=1 sh open_pane.sh` 會在前景執行並顯示 iTerm2 的回應。
- 額外參數：環境變數 `DODECA_PANE_ARGS`，例如 `"--shape geo2 --res 1200"`。
- 第一次執行時 macOS 會問是否允許控制 iTerm2，要按允許。
- 不用自動開的話，自己切一個窗格跑 `./dodeca-core --pane` 也一樣。

## 浮動面板（macOS）

`dodeca-panel` 是同一套渲染核心的原生視窗版：一個沒有標題列的面板貼在螢幕右緣，不搶鍵盤、滑鼠點擊會穿過去、暗的地方是透明的，所以可以浮在全螢幕的 iTerm2 上面。畫面由系統直接合成，不經過終端機。

```sh
sh mac/build.sh
./dodeca-panel --demo              # 先單獨看效果，Ctrl-C 結束
```

建好之後，`open_pane.sh` 會自動改用它：右邊切出來的 iTerm2 窗格保持空白，只負責把那塊位置空出來，面板在裡面啟動、量出窗格的大小、蓋在正上方。關掉那個窗格面板就結束；最後一個 session 結束 15 秒後面板和窗格一起關。

- 面板只留在啟動時所在的那個 Space，也就是啟動它的那個全螢幕 iTerm2 視窗；切到別的桌面或 App 不會跟過去。
- 加 `--all-spaces` 會改成每個 Space 都出現，這時只在 iTerm2 是最前面的 App 時顯示（再加 `--always` 取消這個限制）。
- 切到沒有那個空窗格的 iTerm2 分頁時，面板仍會浮在右緣，可能蓋到文字；介意的話用 `--bg 1e1e1e` 給它實心底色。
- 手動執行時面板寬度是螢幕的 17%，用 `--width`（單位是點）和 `--top` 調整位置，`--inset` 調整第一個 session 上方的留白（預設 30）；由 `open_pane.sh` 啟動時會加 `--fit-pane`，改成跟著那個空窗格的大小。
- 字體預設 Monaco 12，用 `--font`、`--font-size` 對齊你的 iTerm2 設定。
- 想退回終端機窗格版：`export DODECA_PANE_MODE=terminal`。

## 輸出方式

程式先在記憶體裡畫出一張 RGB 影像，再用三種方式之一送到終端機，啟動時會自動偵測：

| 模式 | 說明 |
|---|---|
| `--kitty` | kitty graphics protocol，真像素。終端機回應支援時優先使用 |
| `--iterm` | iTerm2 inline image（PNG），真像素。在 iTerm2 裡且不支援上一種時使用 |
| `--cells` | 半格字元 `▀`，每個字元格兩顆像素。任何 truecolor 終端機都能用，但很粗；tmux 裡固定用這個 |

自動偵測的結果不理想時可以用參數強制指定。

## 選項

```
--pane            側邊窗格的直排版面
--exit-when-empty 沒有 session 超過 15 秒就結束
--demo [N]        用 N 個假 session（1-6，預設 3）
--shape NAME      dodeca | bucky | geo2（預設）| geo3
--res N           影像最長邊的像素數（預設 960，`--pane` 時 420；越大越細但終端機負擔越重）
--fps N           每秒最多幾格（預設 20；平靜的 session 會自動降低重畫頻率）
--glow X          光暈強度（預設 1.0）
--core X          核心半徑，占外殼內切球半徑的比例（預設 0.35）
--events PATH     事件檔（預設 ~/.dodeca-core/events.jsonl，或環境變數 DODECA_EVENTS）
--snapshot FILE   輸出一張 demo 畫面成 PPM 檔後結束（搭配 --at 秒數、--size 寬x高）
```

## 架構

```
Claude Code ──hook.py──▶ ~/.dodeca-core/events.jsonl ──tail──▶ 狀態機 ──▶ 軟體渲染 ──▶ 終端機
```

| 檔案 | 內容 |
|---|---|
| `mac/panel.m` | 浮動面板：Cocoa 視窗、文字標籤、把畫布轉成透明影像 |
| `pane_api.py` | 用 iTerm2 Python API 切窗格並設定寬度 |
| `open_pane.sh` | `SessionStart` hook：在 iTerm2 裡自動切出側邊窗格 |
| `hook.py` | 每個事件 append 一行 JSON：時間、session id、事件名、資料夾名、工具名、通知類型 |
| `src/events.c` | 跟著事件檔讀新增的行 |
| `src/session.c` | 事件對應到狀態，以及每個狀態的外觀參數表 `LOOKS` |
| `src/geom.c` | 產生正十二面體、截角二十面體、測地球的頂點與邊 |
| `src/render.c` | 把線條、光暈、核心、標籤畫進 float RGB 畫布 |
| `src/main.c` | 主迴圈、終端機設定、三種輸出方式、demo |

要改顏色或動態，改 `src/session.c` 的 `LOOKS` 表就好。

## 隱私

事件檔只有時間、session id、事件名、資料夾名稱（不含路徑）、工具名稱和通知類型，不會記錄 prompt 或工具的輸入輸出。沒有網路連線，沒有常駐程式，檔案權限是 600。

## 已知限制

- 權限請求核准之後，Claude Code 不會再送一次事件，所以那個工具執行期間仍顯示 NEEDS YOU，要等工具跑完才會切走。
- 標籤用的是內建的 3×5 點陣字，只有英數和少數符號；資料夾名稱裡的中文會顯示成 `?`。
