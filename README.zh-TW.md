<p align="center"><img src="assets/icon.png" width="112" alt=""></p>

<h1 align="center">Vigil</h1>

<p align="center">
  Claude Code 的狀態顯示：每個執行中的 session 是一顆多面體，用顏色和動作表示它正在做什麼。
</p>

<p align="center">
  <a href="README.md">English</a> · <b>繁體中文</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/macOS-panel-7AA2F7" alt="macOS 面板">
  <img src="https://img.shields.io/badge/Windows%20(WSL)-panel-3FC1D9" alt="Windows 面板">
  <img src="https://img.shields.io/badge/terminal-any%20truecolor-E9892B" alt="任何 truecolor 終端機">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-A3CF7E" alt="MIT 授權"></a>
</p>

<p align="center"><img src="assets/demo.gif" width="640" alt="終端機裡的三個 session 在切換狀態"></p>

同時開好幾個 Claude Code session，或讓一個在背景跑、自己去做別的事時，Vigil 會顯示哪些在思考、在執行工具、在等你核准，或已經做完。它可以開在終端機的一個窗格裡，也可以是 macOS 和 Windows 上一個永遠在最上層的小視窗。

在小視窗裡，指著某個 session 的核心就能看它最後一則回覆；雙擊核心可以直接輸入回覆。agent 做完時，不用每次都切回終端機。

Claude Code 的 hook 每個事件在檔案裡加一行；一個 C 程式讀這個檔案，用自己的渲染器畫出核心。全部在本機執行，不連網路，只需要 libc、libm 和 zlib。

<sub>舊名 dodeca-core：資料夾 `~/.dodeca-core` 和 `DODECA_*` 環境變數暫時沿用舊名。</sub>

<p align="center">
  <a href="#狀態">狀態</a> ·
  <a href="#快速開始">快速開始</a> ·
  <a href="#面板">面板</a> ·
  <a href="#隱私">隱私</a> ·
  <a href="#參考">參考</a>
</p>

## 狀態

<table>
  <tr>
    <td width="33%" align="center"><img src="assets/states/idle.gif" width="180" alt="IDLE"><br><b>IDLE</b><br><sub>剛開始或做完之後。慢慢轉，偶爾自己做個小動作</sub></td>
    <td width="33%" align="center"><img src="assets/states/think.gif" width="180" alt="THINK"><br><b>THINK</b><br><sub>送出 prompt 或工具跑完之後。琥珀色</sub></td>
    <td width="33%" align="center"><img src="assets/states/work.gif" width="180" alt="WORK"><br><b>WORK</b><br><sub>工具執行中。青色，邊線上有火花</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="assets/states/needs.gif" width="180" alt="NEEDS YOU"><br><b>NEEDS YOU</b><br><sub>等你核准。橘色，核心閃爍</sub></td>
    <td align="center"><img src="assets/states/done.gif" width="180" alt="DONE"><br><b>DONE</b><br><sub>Claude 回覆完畢。短暫爆開，然後回到 IDLE</sub></td>
    <td align="center"><img src="assets/states/sleep.gif" width="180" alt="SLEEP"><br><b>SLEEP</b><br><sub>閒置 90 秒。變暗</sub></td>
  </tr>
</table>

每個狀態有好幾種動作，每次進入時隨機挑一種（[完整列表](#參考)）。`SessionEnd` 會移除那一顆，30 分鐘沒有事件的也會移除。

## 快速開始

```sh
make
./vigil --demo                     # 假的 session；q 離開

python3 install_hooks.py           # dry run：列出 ~/.claude/settings.json 會改哪些地方
python3 install_hooks.py --apply   # 安裝 hook（會先備份你的設定）
./vigil                            # 你的 session
```

- 需要 C 編譯器、`make`、zlib 和 Python 3（只用標準函式庫）。macOS：`xcode-select --install`。WSL：`sudo apt install build-essential zlib1g-dev`。
- 能顯示影像的終端機（iTerm2、kitty、Ghostty、WezTerm）用完整解析度；其他 truecolor 終端機用半格字元。
- hook 只對安裝之後新開的 session 生效。它是用絕對路徑呼叫的，搬了這個資料夾要再跑一次 `--apply`。`--remove` 移除。
- `--rich` 會另外保留 Claude 最後一則回覆和最近幾輪對話（你的提示、回覆和簡短的步驟說明），面板的對話框和語音需要它。預設關閉，見[隱私](#隱私)。

## 面板

一個小視窗，停在其他視窗上面，不搶鍵盤焦點，除了核心以外的地方點擊都會穿過去。

```sh
sh mac/build.sh && ./vigil-panel --demo     # macOS
sh win/build.sh && ./vigil-win --demo       # Windows：在 WSL 裡執行（需要 mingw-w64）
```

<p align="center"><img src="assets/demos/reply.gif" width="800" alt="指著主核心顯示它在等的核准；指著小核心顯示它最後一則回覆"><br>
<sub>指著核心會顯示它最後一則回覆，或它在等的核准。停 2 秒後對焦框會鎖定。</sub></p>

<p align="center"><img src="assets/demos/input.gif" width="800" alt="捲動一則長回覆，再輸入回覆並附上一張圖片"><br>
<sub>滾輪捲動長回覆。雙擊打開回覆框；拖到核心上的圖片會變成 <code>[Image #1]</code>。按 Enter 送到那個 session。</sub></p>

<p align="center"><img src="assets/demos/carry.gif" width="806" alt="拿著核心移動：移動太快小核心會掉下來，靠近就會回到軌道"><br>
<sub>Alt 點一下就能拿起核心，小核心沿著軌道跟著走。移動太快它們會掉下來，撞到螢幕邊緣彈開；把主核心移到旁邊，它就會回到軌道上。重力：Floaty。</sub></p>

- **一顆或全部。** 單核心模式是一顆大核心顯示需要你的 session（沒有的話是最新回覆的那個），其他 session 是旁邊的小核心；全部核心模式則每個 session 各自一顆。Windows 面板預設是單核心模式（`--all` 切成全部核心，每顆放在你想放的位置）；macOS 面板預設是全部核心，排在螢幕右緣一列（`--single` 或 `⌃⌥C` 切成單核心模式）。
- **回覆。** 雙擊核心、輸入、按 Enter，可以把圖片拖到核心上。macOS 也可以按住核心說話（按住說話，在 Mac 上辨識），文字會打進那個 session 的 iTerm2 窗格。
- **語音回報**（macOS，`--voice`）：點核心會念出它回覆的簡短摘要。
- **設定**（Windows：右鍵 > Settings）：可以調整大小的視窗，附可放大的即時預覽和六頁（Look、Cluster、Replies、Motions、Shortcuts、Performance）：形狀、光暈、大小、重力、對焦框、對話框的字型和顏色、每個狀態用哪些動作，以及可以自己錄的全域快捷鍵。Mac 也有類似但較簡單的視窗（右鍵 > Settings...），另外多了 Voice 一列；在 Mac 上滾輪仍是調語音音量，⌥ + 滾輪才會轉動核心（見下方）。

<table>
  <tr>
    <td width="50%"><img src="assets/settings.png" alt="設定 > Look：各頁的側欄、即時預覽，和一列列的設定"></td>
    <td width="50%"><img src="assets/settings-motions.png" alt="設定 > Motions：每個狀態和它的常駐動作，以及選中狀態的常駐動作和小動作"></td>
  </tr>
</table>

<p align="center"><sub>Windows 面板的設定視窗：Look 與 Motions。</sub></p>

## 隱私

事件檔只有時間、session id、事件名、資料夾名稱（不含路徑）、工具名稱和通知類型，子代理（subagent）的事件另外有它的 agent id 和類型（例如 `Explore`），不會記錄 prompt 或工具的輸入輸出。沒有網路連線，沒有常駐程式，檔案權限是 600。

開啟 `DODECA_RICH=1`（`install_hooks.py --rich`）時，hook 另外為每個 session 保留一個檔案 `~/.dodeca-core/msg/<session id>.json`（權限 600，資料夾 700）：**Claude 最後一則回覆的文字**和最後一則通知（例如權限請求）的文字，各最多 4000 字，它所在終端機的 `ITERM_SESSION_ID`，以及**像終端機那樣的最近 5 輪對話**：你輸入的每個提示（最多 2000 字）、它的回覆和它的步驟，也就是工具名稱加一段簡短說明（指令的第一行、不含資料夾的檔名、搜尋樣式或網站網域；每輪保留最後 8 步，其餘只記數量），還有正在執行的那一步（開啟時已經在跑的 session，會從它的 Claude Code 對話紀錄補上之前的幾輪，只補一次；恢復（resume）的 session 一恢復就會補回之前的幾輪和最後一則回覆）。檔案超過約 24 KB 時先丟掉最舊的幾輪。子代理的最後回覆存在它自己的檔案 `msg/<session id>-<agent id>.json`，跟著 session 的檔案一起刪除。新的回覆或通知會蓋掉舊的，session 結束時檔案會刪掉，沒正常結束的 session 留下的檔案 24 小時後刪除。事件檔除了 `Stop` 那行多一個 `"m": 1` 之外不變。因為提示、回覆和指令都會寫到磁碟上，只在你自己的電腦上開。

語音（`vigil-panel --voice`）：語音合成在這台 Mac 上完成，按住說話的辨識強制離線，錄音不會離開這台 Mac，也不會保存。辨識出來的文字會打進它要送去的那個 Claude Code session，就像你自己打字一樣。唯一會用到網路的是 `--summarize claude`：它透過 `claude -p` 把回覆文字送給 Anthropic 來縮短。

## 參考

<details>
<summary><b>每個動作</b></summary>

| 狀態 | 觸發事件 | 畫面 |
|---|---|---|
| IDLE | `SessionStart`、完成後 | 平靜而精準的球體：白核藍暈、慢速旋轉；偶爾有一道光從底部爬到頂部，每隔幾秒會自己做點小動作（東張西望、探頭、轉圈、眨眼、把一個頂點轉向你、喀一聲轉一格、展開、掃描、冒刺、一道波紋掃過表面、一塊碎片繞著自己飛一圈、正對你的頂點周圍像花一樣綻開、一串相鄰節點依序亮起、慢慢翻轉半圈，偶爾整個發狂一下） |
| THINK | `UserPromptSubmit`、`PostToolUse` | 琥珀色。以下其中一種：**Puzzle**（外殼的一層碎片分開浮起，兩層像在解魔術方塊一樣反向轉動：快轉兩下、停一下，三次）、**Smooth puzzle**（同樣的動作但轉得平順）、**Long spin**（兩層輪流各轉三整圈）、**Blades**（單片三角形飄到核心後方，再像一排箭一樣成對掃過正面）。預設關閉、要在 Settings > Motions 打開的：**Skein**（五片三角形離開外殼變成刀鋒發光的細長刀刃，沿著一條曲線停在核心後方，再排成雁形掃過正面，到另一側停頓，然後掃回來、收回外殼） |
| WORK | `PreToolUse` | 電光青色，光點沿邊線遊走。以下其中一種：**Engine**（渦輪帶和依序推出的活塞）、**Gyro**（兩條互相垂直的帶）、**Helix**（浮起的碎片沿著轉動的外殼螺旋上升）、**Typewriter**（一排排按鍵和換行）、**Forge**（碎片發熱升起再重重敲下：火花和餘燼）、**Loom**（梭子在兩條交叉的帶上拉出光線）、**Conveyor**（兩圈輪流一格一格前進）、**Compressor**（分三下壓扁，再一口氣洩壓） |
| NEEDS YOU | `Notification`（閒置提醒除外） | 橘色，核心故障式閃爍。以下其中一種：**Swimmers**（兩條由小碎片組成的「魚」在表面下游動）、**Beacon**（像燈塔的光束，每次轉向你就閃一下）、**Knock**（朝你敲兩下，每下一圈光） |
| DONE | `Stop` | 以下其中一種：**Unlock**（蓄力、帶著尖刺和一圈光爆開、轉一整圈，碎片一片片鎖回去）、**Bloom**（像花一樣一圈圈綻開、轉一圈、收回封好）、**Zip**（拉鍊頭沿螺旋往下把碎片一片片扣上，然後火花噴出）；2.5 秒後回 IDLE（Bloom 和 Zip 稍久一點） |
| SLEEP | IDLE 超過 90 秒 | 整體變暗。**Black hole**：外殼爆開，碎片四散，核心變暗成為一個小黑洞，把碎片沿螺旋拉回、落入盤面，越靠近越被拉長，每吞下一塊就變大一些。光線按真實黑洞周圍的彎曲方式追蹤（Schwarzschild 黑洞，如 Luminet 1979 年的圖和 Interstellar 的 Gargantua）：從稍高處看的薄吸積盤橫過黑影並轉動，盤的另一側被彎到黑影上方和下方，黑影周圍有一整圈明亮的白色光子環，朝你而來的一側較粗，黑影裡面則映出翻轉、向光環擠壓的吸積盤倒影，轉動它時跟著改變（開啟 Lensing 時），盤上的氣體柔和地旋轉，朝你轉來的一側較亮（都卜勒效應），背後的星星固定在螢幕上，不論黑洞怎麼轉都不會跟著轉或變大，只在靠近它、光線被重力彎曲的地方被拉成繞著它的短弧（**Lensing**，在 Settings > Motions 的 Asleep 頁裡可以開關）；在淺色桌面上也看得清楚，在 Windows 面板裡它的視窗會變大，讓吸積盤和各種動作有空間。偶爾會演出真實黑洞上觀測到或預測的現象（在 GPU 上和其餘部分一起追蹤成發光的氣體，所以會繞著黑洞彎曲、從它後方經過）：**Tidal disruption**（一顆恆星，一團靠自身重力聚在一起的氣體，圓圓地飛來；從黑洞下方近距離掠過時，潮汐力勝過它自身的重力，把它撕成一道物質流：外側一半被甩走，內側一半繞回來與自己相撞，形成發光的激波，再繞圈落入並加熱吸積盤）；**Flare**（磁環，就像太陽上的：一道熾熱氣體的拱環從內盤升起，越長越扭，然後斷開：一道閃光，內盤亮起、光向外擴散，光子環稍後跟著一亮，拱環頂端被甩出成一團氣體，在黑洞的重力下爬升、減速並彎轉，一路從白色冷卻成橘色）；**Precession**（參考系拖曳：吸積盤像陀螺一樣晃動，自轉軸繞著越來越大的圓錐轉，從各個方向露出正面，周圍的核心被甩得越轉越快、越來越近，像要被吸進去，然後再放開）；以及順著氣體方向朝內緣盤旋落入、越落越快的 **Hot spot**（熱點），身後拖成一道弧，轉到黑洞後方時它的影像被彎到黑影上方。操作它時它仍是黑洞：指著它時吸積盤轉得更快、你那一側變亮；點一下會從那一側丟進一顆恆星；按住不放會從游標處拉出一道氣體流，螺旋落入並加熱吸積盤；滾輪會推動吸積盤的氣體轉得更快（反向滾動時反轉），盤也跟著變亮，幾秒後慢慢恢復；不停地捲動會推得越來越用力，就像核心會越轉越快；拖曳或橫向滑動會像一般核心一樣把它翻轉，可以從上方、側面或下方看它，甩出去會繼續轉；放著不動會慢慢回到側面視角；其他 session 的核心在它的吸積盤平面上繞行，跟著傾斜、從它後方經過，被甩出去的衛星會被它拉回、捕獲進軌道；自己的 session 也在睡眠的衛星會變成迷你黑洞；你離開時它會安靜下來（吸積盤變暗變慢，衛星漂到外圈），你回來時會亮起一陣閃焰；衝向它會讓它爆出閃焰。只有不碰它、停在上面 2 秒才會把它叫醒。它睡著時，有未讀回覆的衛星每隔幾秒會被它丟向你的游標（Windows 面板）：先蓄力發亮，再朝游標要去的地方甩出；打中了就黏在游標上繞圈，直到你甩動滑鼠把它甩掉；躲開了，黑洞的引力會把它拉回來、捕獲回軌道；把它引進黑洞裡，它會被吞掉，片刻後再吐回軌道上。 |

**READY 閃避遊戲**（Windows 面板）：完整躲過一次攻擊就開始一局，十顆愛心會落在核心所在螢幕的左邊；每被打中一次就少一顆（它會彈一下然後變暗）；最後一顆沒了，或你讀了回覆，這局就結束。

每次進入一個狀態時，會從它的動作中隨機挑一種（由面板的設定決定有哪些；見下面的 Motions）。外殼不會彎曲或晃動：它用喀一聲的轉動、會滑出再鎖回的三角形碎片（每次隨機切，不會重複）、相對轉動的層、整片被帶走的碎片、以及尖刺來動（一個節點被拉得很遠變成尖錐，同時另一個被拉向中心），還有火花、光軌和光圈。面板自己的狀態也會動：SPEAKING（碎片隨語音起伏）、LISTENING（正面隨麥克風張開）、TRANSCRIBING（一次轉 36° 同時掃描面在讀）、STANDBY（靜止、正對你）、ERROR（一陣一陣地發狂）。碎片需要測地線外殼（預設的 `geo2` 或 `geo3`）；`dodeca` 和 `bucky` 保持剛性的網，只有轉動、尖刺和光。

`SessionEnd` 會移除那一顆；30 分鐘沒有任何事件的 session 也會移除。

</details>

<details>
<summary><b>浮動面板（macOS）</b></summary>

`vigil-panel` 是同一套渲染核心的原生視窗版：一個沒有標題列的面板貼在螢幕右緣，不搶鍵盤、滑鼠點擊會穿過去（核心上除外，見下面）、暗的地方是透明的，所以可以浮在全螢幕的 iTerm2 上面。畫面由系統直接合成，不經過終端機。

```sh
sh mac/build.sh
./vigil-panel --demo              # 先單獨看效果，Ctrl-C 結束
```

`mac/build.sh` 會產生 `Vigil.app` 和一個小的啟動腳本 `./vigil-panel`。啟動腳本透過 macOS 啟動 App（這樣語音功能用到的麥克風和語音辨識權限算在這個 App 身上，而不是你的終端機），一直留在終端機裡直到 App 結束，選項也都一樣。Ctrl-C 或關掉那個終端機窗格兩個會一起結束。App 用 ad-hoc 簽章，每次建置都會變，所以重新建置後 macOS 會再問一次權限；不想這樣的話，在「鑰匙圈存取」建一個「程式碼簽署」憑證，之後用 `DODECA_SIGN_ID="憑證名稱" sh mac/build.sh` 建置。

建好之後，`open_pane.sh` 會自動改用它：右邊切出來的 iTerm2 窗格保持空白，只負責把那塊位置空出來，面板在裡面啟動、量出窗格的大小、蓋在正上方。關掉那個窗格面板就結束；最後一個 session 結束 15 秒後面板和窗格一起關。

- 面板只留在啟動時所在的那個 Space，也就是啟動它的那個全螢幕 iTerm2 視窗；切到別的桌面或 App 不會跟過去。
- 加 `--all-spaces` 會改成每個 Space 都出現，這時只在 iTerm2 是最前面的 App 時顯示（再加 `--always` 取消這個限制）。
- 切到沒有那個空窗格的 iTerm2 分頁時，面板仍會浮在右緣，可能蓋到文字；介意的話用 `--bg 1e1e1e` 給它實心底色。
- 手動執行時面板寬度是螢幕的 17%，用 `--width`（單位是點）和 `--top` 調整位置，`--inset` 調整第一個 session 上方的留白（預設 30）；由 `open_pane.sh` 啟動時會加 `--fit-pane`，改成跟著那個空窗格的大小。
- 字體預設 Monaco 12，用 `--font`、`--font-size` 對齊你的 iTerm2 設定。
- 想退回終端機窗格版：`export DODECA_PANE_MODE=terminal`。
- 核心會對滑鼠和快捷鍵起反應：見[互動與語音](#參考)。

</details>

<details>
<summary><b>互動與語音（macOS 面板）</b></summary>

`vigil-panel` 裡的核心會對滑鼠和一個全域快捷鍵起反應。面板仍然不會搶走 iTerm2 的鍵盤焦點，點在核心圓形外殼以外的地方也照樣傳到終端機。說話和按住說話要加 `--voice` 才會開；沒加的話，單擊只會把回覆標成已讀，長按核心沒有作用。`./vigil-panel --demo --voice` 可以用假的 session 試下面每一項（加 `--away 10` 可以很快看到待命狀態）。

| 輸入 | 動作 | 核心反應 |
|---|---|---|
| 滑鼠指著 | 0.3 秒後在面板左邊出現提示框：資料夾、狀態，以及最後一則回覆，排版和終端機裡一樣，最多 16 行（NEEDS YOU 時改顯示通知文字） | 變亮、轉慢、朝游標轉過來，並依狀態有自己的回應：IDLE 騷動（你那一側冒出緩和的尖刺）、THINK 點亮游標下的節點、WORK 的活塞推得更用力、NEEDS YOU 的魚游向你、TRANSCRIBING 的掃描面跟著你、SLEEP 和 STANDBY 醒來、ERROR 閃開 |
| 停在上面 2 秒 | — | 鎖定：停下來把一個頂點對著你，對焦框收攏（Settings > Focus frame：segments、lens、polar、diamond、lock、orbits 或 none） |
| 衝向它 | — | 只有從很遠（超過 2.2 個外殼半徑）在 0.18 秒內衝到核心上才算：碎片夾緊、朝你來的方向射出尖刺、外殼一震。WORK 時不會 |
| 有未讀回覆 | — | 規律的柔和閃光，直到你點它或送出新的 prompt |
| 單擊 | 念出回報，或停止；回覆標成已讀 | 被點的碎片凹進去，這一下會往周圍的碎片傳出去；然後 SPEAKING（青綠色） |
| 長按（> 0.35 秒）後放開 | 按住說話：說的話會在核心下方顯示 1.5 秒，然後打進那個 session 並按 Enter；這段時間內點核心可以取消 | 按住時 LISTENING（綠色、外殼停轉、光暈隨麥克風音量變大），放開後 TRANSCRIBING（紫色光點沿邊線快速繞行） |
| 雙擊 | 在核心的提示框下方打開回覆框：輸入後按 Enter 送到那個 session（Shift+Enter 換行，窗格收到時會合成一行），Esc 或右上角的 ✕ 會關閉並保留草稿；點其他地方時回覆框不會關，方便你到 Finder 或截圖縮圖上拿圖片。拖到核心或回覆框上、或用 ⌘V 貼上的圖片，會在游標處各放一個 `[Image #N]` 標記，和 Claude Code 一樣；Backspace 一次刪掉整個標記，送出時每個標記會換成圖片的路徑。Claude 還在工作時會先問你，和語音回覆一樣 | 一圈衝擊波；圖片拖到核心上方時，核心會讀取（TRANSCRIBING） |
| 右鍵 | 選單：重播、複製回覆、靜音這個 session、叫到最前面，以及 **Settings...** 設定視窗：即時預覽（可拖動核心、在上面捲動放大、按鈕切換各狀態、用文字框外觀顯示的回覆）、**Look**（形狀、光暈、螢幕上的大小、內核大小、3D 公轉和傾斜程度）、**Motion**（重力、顯示或隱藏其他 session、對焦框、⌥ + 滾輪做什麼）、**Replies**（文字框的字型、字級、框的大小、底色和文字顏色、出現位置、核心上的名稱）、**Voice**（唸出回覆、自動唸新回覆、用 Claude 摘要、送出前等點擊）和待機時間；**Motions** 分頁選擇每個狀態可以用哪些 routine 和 action（▶ 在預覽裡播放）。選擇立刻生效，存在 `~/.dodeca-core/panel-settings.json`，優先於啟動參數；Reset to defaults 會清掉它們。沒有 session 時右鍵只顯示 Settings | — |
| 拖曳 | 用手轉動多面體；邊移動邊放開會繼續轉，慢慢回到自己的轉速 | — |
| 滾輪 | 這顆核心的語音音量（在 Mac 上滾輪不會轉動核心） | 外殼短暫變粗或變細 |
| ⌥ + 滾輪 | 轉動核心：Roll（往下捲時頂部往後翻）、Roll back、Turn（繞直立軸轉）、Turn back 或 Off，依 Settings > Motion > ⌥ + scroll on a core 的設定 | 會繼續轉，再慢慢回到自己的轉速，和拖曳後一樣 |
| 錄音中按 Esc | 取消，不送出任何東西 | 光暈縮小淡出 |
| 按住 `⌃⌥V`（需 `--voice`） | 對最需要你的 session 按住說話：NEEDS YOU 優先，其次最近完成的，再其次最近活動的 | 同長按核心 |
| 2 分鐘沒有鍵盤或滑鼠輸入 | — | 除了 NEEDS YOU 以外的核心都進入 STANDBY（暗靛色）；你回來時，有未讀回覆的核心會亮一下提醒你 |

- **單核心模式**（`--single`，或隨時按 `⌃⌥C`；`--mode-hotkey` 可以換按鍵）：一顆大核心代表一個 session，其他 session 變成小核心在旁邊漂浮。每個 session 的核心會帶一點自己的顏色，一看就知道誰是誰；外殼仍保留狀態的顏色。面板會縮成只有這一團：按住 ⌥ 拖曳可以放到螢幕任何地方（或 ⌥ 點一下，它就跟著滑鼠走，再點一下放下；拿著時游標會隱藏，對焦框框住它，核心會朝移動方向傾斜、停下或轉向時稍微落後再穩住（不會彈），後側的碎片微微張開，用力甩會讓它發狂；小核心靠重力留在軌道上（Settings > Motion > Gravity，Floaty 到 Firm），拿著移動時會沿軌道滑到後方，移動太快會把它們甩飛出去：它們會繼續飛、在螢幕邊緣反彈，直到你把大核心移到它們附近；開啟 Settings > Look > 3D orbit（預設開啟，只有 Mac 有）時，軌道會傾斜並自己緩緩搖擺，轉到遠側的小核心會變小、變暗，並從大核心後面經過，滑鼠靠近整群時會放平，變回俯視；另外偶爾在大核心跑 Skein 時，刀刃會撞到小核心，讓它碎成幾塊再組回來，或直接被撞飛、一邊翻滾一邊脫離軌道，直到你把大核心移到它附近），下次會回到同一個位置（依螢幕大小存在 `~/.dodeca-core/panel-position.json`；右鍵 > Reset position 放回右緣）。點一顆小核心，它會換到中間並固定在那裡（提示框會顯示「pinned」）；右鍵 > Follow automatically 讓面板重新自動選。自動時中間那顆跟著新消息走：需要你核准的 session 優先，其次是最新有回覆的；不會只因為 session 在忙就跳來跳去。用快捷鍵按住說話時，會送給中間那顆。右鍵選單也可以切換模式。
- 在全部核心模式下，按住 `⌥` 拖曳核心可以上下調整順序：核心會跟著滑鼠，其他核心會讓出位置。⌥ 點一下可以不按住就拿起核心，再點一下放下。
- `--labels hover` 會拿掉每顆核心上方的兩行文字，面板只顯示核心（空出來的位置讓核心變大），由提示框告訴你哪顆是哪個。
- 提示框和辨識結果的預覽會出現在面板左邊、蓋在終端機上，文字會自動換行而不是被截掉；預覽會完整顯示要送出的內容。標題用那個 session 的顏色（狀態的顏色；單核心模式下是它自己的顏色），左邊有一條同色的線；內文用系統字體，大小跟著 `--font-size`，排版和 Claude Code 的終端機一樣：段落之間有空隙、`-` 項目變成圓點而且換行會對齊縮排、**粗體**、`code` 用等寬字體、程式碼區塊縮排。
- 單擊會等系統的雙擊時間過了才動作，所以雙擊不會先觸發一次單擊。
- 快捷鍵用 `RegisterEventHotKey`，不需要「輔助使用」權限。Esc 只在錄音時才攔，其他時候終端機照常收得到。`--hotkey ctrl+opt+v` 可以改按住說話的按鍵（`cmd`、`opt`、`ctrl`、`shift` 加上一個字母、數字、`space`、`return`、`tab` 或 `f1`–`f12`；`none` 關閉）。macOS 已經用掉 ⌃Space、⌃⌥Space、⌘Space 和 ⌥⌘Space，所以預設是 `⌃⌥V`。
- 是否離開是看系統提供的「距離上次輸入的秒數」（不需要「輸入監控」權限，也不記錄按鍵）。`--away 秒數` 改掉預設的 120 秒；`--away 0` 關閉。
- 送出回覆和「叫到最前面」需要 `DODECA_RICH`（iTerm2 session id 存在 msg 檔裡），第一次使用時還要允許面板控制 iTerm2（系統設定 > 隱私權與安全性 > 自動化）。

**語音回報。** 回報開頭會說來源：回覆是「notes 完成了：…」，需要你核准時是「notes 需要你：…」加上權限請求的內容。同一時間只有一個 session 在說話，其他的排隊，權限請求會插到隊伍最前面。

- 預設只有在你點核心或選「重播」時才會說話。加 `--auto-speak` 的話，如果你最近 60 秒內有用鍵盤或滑鼠，新的回覆會自動念出來；新的權限請求則會立刻念。靜音的 session 不會自己說話。
- 念的是摘要。預設是回覆的第一段，最多 3 句或 150 字（不花錢、沒有延遲）。加 `--summarize claude` 的話，面板會在背景呼叫 `claude -p`（haiku 模型，關掉 hook 和工具），請它濃縮成兩三句口語化的繁體中文；超過 15 秒就改用本地摘要。摘要會存在 msg 檔裡，重播不會再問一次。這會把回覆文字再送給 Anthropic 一次，也會用到你的 Claude Code 額度。
- 念之前，程式碼區塊會換成「（略過一段程式碼）」，Markdown 符號和網址會拿掉（連結保留文字）。
- 用的是系統設定 > 輔助使用 > 閱讀與朗讀 裡的 **系統聲音**。請選 **Siri** 的聲音：只有它念中文夾英文會自然，而且 App 只能用這個方式用到它（面板用 `say` 一句一句產生語音再播放，所以按下後大約一秒才會出聲）。`--voice-id zh-TW`（或 `en-US`、聲音識別碼）改用 App 可以直接指定的聲音：會立刻開始，面板會挑裝好的最好的那個（zh-TW 請在管理聲音下載 **美佳（高品質）**），但中文裡夾的英文會念得很差。`--rate` 設定語速（0 到 1，預設 0.5）。macOS 語言是中文時，面板自己的用語是中文。
- 說話不需要任何權限。

**語音回覆。** 按住核心（或 `⌃⌥V`）、說話、放開。

- 辨識只在這台 Mac 上進行（`SFSpeechRecognizer`，強制離線辨識），錄音不會上傳。如果那個語言沒有離線模型，按住說話會關閉，核心會說明原因。`--listen-locale` 設定語言（預設 `zh-TW`）。
- 辨識出來的文字會顯示在核心下方，1.5 秒後打進那個 session 的 iTerm2 窗格並按 Enter；在那之前點核心可以取消。加 `--confirm-send` 的話會一直等，再點一次才送出。
- 如果 Claude 還在工作，面板會先問（「還在工作中，要打斷嗎？」）並跳出選單：等這一步做完再送（Claude Code 會在目前這一步結束後讀到）、打斷再送（先按 Esc），或取消。
- 找不到窗格時，核心會變成紅色（ERROR）並說明原因。
- 錄音用系統的輸入裝置，但如果那是虛擬裝置（BlackHole、Loopback、Soundflower、Teams 或 Zoom 的音訊裝置），它傳的是別的 App 的聲音而不是你的聲音，這時會改用內建麥克風。`--mic 名稱` 可以用名稱的一部分指定裝置。錄到完全沒有聲音時，核心會說它用的是哪個裝置。
- `--debug` 會把語音的每個步驟寫到 stderr（裝置、音量、辨識錯誤、送出結果；不會寫你說的內容）。
- 送出需要 `DODECA_RICH`（session 的 `ITERM_SESSION_ID` 存在 msg 檔裡）。用的是 AppleScript，第一次會問能不能控制 iTerm2；不允許的話會改用 iTerm2 的 Python API（`pane_api.py send`）。
- `--demo` 不會打字到任何地方，核心只會說它本來會送出什麼。

**語音功能的一次性設定：**

1. `sh mac/build.sh`，然後用 `--voice` 啟動面板（`open_pane.sh` 開的窗格用 `DODECA_PANE_ARGS="--voice"`）。
2. `python3 install_hooks.py --rich --apply`，保留回覆和 iTerm2 session id（之後新開的 Claude Code session 才會生效）。
3. 第一次按住核心時，macOS 會問麥克風和語音辨識權限：兩個都允許，然後再按住一次。
4. 第一次送出回覆或選「Bring to front」時，macOS 會問 Vigil 能不能控制 iTerm2：允許（系統設定 > 隱私權與安全性 > 自動化）。
5. 選用：更好的 zh-TW 聲音（系統設定 > 輔助使用 > 朗讀內容 > 系統聲音 > 管理聲音）；iTerm2 > Settings > General > Magic > Enable Python API，當作送出的備援。
- 每種反應的樣子在 `src/session.c` 設定：`LOOKS`（SPEAKING、LISTENING、TRANSCRIBING、STANDBY 那幾列）、`HOVER` 和 `TOUCH`。

</details>

<details>
<summary><b>Windows 面板</b></summary>

在 Windows 上，請在 WSL 裡跑 Claude Code；hook 和終端機版本在那裡跟 Linux 一樣能用（`sudo apt install build-essential zlib1g-dev`，然後在 WSL 裡 `make` 和 `python3 install_hooks.py --rich --apply`）。浮動面板是原生的 Windows 程式，透過 WSL 的 `\\wsl.localhost` 共用讀同一個事件檔：

```sh
sudo apt install mingw-w64      # 只要一次，在 WSL 裡
sh win/build.sh                 # 產生 vigil-panel.exe 和啟動腳本 ./vigil-win
./vigil-win                     # 在 WSL 裡：用 ~/.dodeca-core/events.jsonl 啟動面板；Ctrl-C 結束
./vigil-win --demo              # 假的 session
```

預設是單核心模式（Mac 上要加 `--single`）：焦點 session 是一顆大核心，其他 session 以小衛星繞著它，放在一個小小的半透明視窗裡，永遠在最上層，不會搶焦點，點在核心以外的地方會傳到後面的視窗。核心會像 macOS 的單核心模式一樣跟著消息走（先是需要你的 session，否則是最新回覆的那個）；點一下核心，或用右鍵選單的 **Session**，可以把它固定住。關掉 **Show other sessions**（或 `--no-others`）就只剩大核心。`--all` 或 **Ctrl+Alt+C** 切換成所有核心：每顆核心是自己的小視窗，一開始排在右緣一列，每顆都可以各自放到任何地方。

Alt 等於 Mac 上的 ⌥：**Alt 拖曳**可以把整團（單核心）或那一顆核心（所有核心）移到螢幕任何地方；**Alt 點一下**把它拿起來，下一次在任何地方點擊就放下。位置依螢幕大小記在 `~/.dodeca-core/panel-position-win.json`（每顆核心依資料夾名稱），選單裡的 **Reset position(s)** 會把它們忘掉。

指著核心一下會出現對話框：名稱和狀態，接著是像 Claude Code 那樣的最近幾輪對話（要用 `--rich` 的 hook）：每個提示在自己的底色帶裡，然後是步驟（`● Bash(make -j)`，正在執行的那一步是橘色；做完的一輪把步驟收成一行），回覆，需要你的時候最後是權限通知。它會打開在最新的提示，Claude 工作時則跟著最底下；往上捲可以看前面幾輪，正在看的那一輪的提示會釘在頂端成一行（指著它看全文，點它回到那裡）。回覆的排版和 Claude Code 一樣（表格有完整格線，格子裡也能有粗體、斜體和 `code`）。它會放在叢集旁邊第一個不會蓋到任何核心（包括小核心）的位置：左、右、下或上；叢集在螢幕中間時先試下方和上方。指著核心停 2 秒，或拿著它走，會畫出對焦框（六種樣式，跟 Mac 一樣）。一段時間沒有輸入（預設 2 分鐘），每顆核心都顯示 STANDBY，需要你的那顆除外；回來時，有未讀回覆的會閃一下。滑鼠滾輪在核心上可以捲動對話框裡較長的回覆，核心也會跟著轉，之後就照你送出的方向繼續轉，連續捲得越久轉得越快；橫向捲動（觸控板的兩指滑動）會讓它繞另一個軸轉，所以用滑動可以把它往任何方向轉。

Claude 啟動的子代理會以自己的衛星出現，名稱是它的類型（`Explore`、`Plan`……），它工作和思考時跟著變化，它的 session 則在等它。做完時顯示 DONE，回覆標成未讀；指著它可以在對話框讀回覆，指標離開後衛星就消失。這需要在這次更新之後重新安裝 hook（再跑一次 `python3 install_hooks.py --rich --apply`）。

**雙擊**核心可以在對話框下方輸入回覆：Enter 送出，Shift+Enter 換行，Esc 關閉並保留草稿。把圖片檔（png、jpg、gif、webp）拖到核心或那個輸入框上就會一起送出：圖片在核心上方時核心會開始讀取（TRANSCRIBING），每張圖片會在游標處放入一個 `[Image #N]` 標記，和 Claude Code 一樣，可以像一般文字移動或刪除（Backspace 會一次刪掉整個標記）。Ctrl+V 也一樣：複製的圖片（截圖、在瀏覽器裡複製的圖片，會存成 PNG 放在 Windows 暫存資料夾，超過一天的會清掉）或在檔案總管裡複製的圖片檔都會變成標記；文字則照常貼上。送出時每個標記會在原處換成圖片的 WSL 路徑（`/mnt/c/...`），Claude Code 會當成圖片讀取。如果 Claude Code 在 Windows Terminal 的分頁裡執行，回覆會直接送進去，不會把任何視窗叫到前面，也不用另外設定：`--rich` 的 hook 會記下分頁，面板把回覆打進那個分頁（多行時當成一次貼上，再按 Enter）。在 WSL 的 tmux 裡執行，或用 `./vigil-claude` 啟動的 session 也一樣（給其他終端機用：它照原樣執行 `claude`，並把面板送來的文字貼進去）。否則終端機會被叫到最前面，文字以一行打進去。如果送出時那個 session 正在問你事情（NEEDS YOU，例如權限請求），回覆會先等著，所以絕不會替你回答那個提示：等你在終端機回答之後才送進去，對話框的標題會顯示它在等；再打開回覆框可以把它拿回來修改。

右鍵點核心有 **Reply**、**Copy reply** 和 **Bring to front**：這裡沒有 iTerm2，所以它會叫出標題裡有資料夾名稱的終端機或 VS Code 視窗，否則是最近用過的 Windows Terminal 視窗，但無法選到分頁。

右鍵選單用和設定視窗一樣的顏色。右鍵 > **Settings...** 打開設定視窗。左邊的側欄列出各頁（最上面的按鈕可以把它收成一排圖示）；頂端列有該頁的名稱、它的 Reset 和關閉，拖動它可以移動視窗（側欄的空白處也可以）；下面是即時預覽：叢集實際的樣子（拖動核心可以感受重力；在 Motions 頁它會停在你放下的地方，方便從那裡看動作或 READY 的攻擊；− / + 或滾輪可放大到 3 倍，雙擊兩者都恢復）、要用哪個狀態顯示，在 Replies 頁還有一段用對話框字型和大小顯示的回覆。下方是該頁的內容：一張張卡片，每一列左邊是設定名稱、右邊是它的控制項；選中的按鈕用橘色框標出。視窗可以從右邊、下邊和右下角拉大縮小（最小 620 × 480），並記住大小和側欄是否收起。它是一般的視窗（有自己的工作列按鈕）：核心和它們的衛星永遠在它上面，切換到其他程式時它會像一般視窗一樣退到後面。

- **Look**：形狀、光暈、內核大小；螢幕上的大小、對焦框；背景暗化（Off、On，或在核心下方是淺色時自動開啟的 Auto：沿著每條線、每個節點和每個丟出的碎片畫一圈細細的柔和暗邊，不管它們變成什麼形狀都跟著，讓它們在淺色程式上也看得清楚）、它的顏色、暗邊的寬度和濃度，以及要不要在明亮的中心後面加一個小小的柔和暗圓。
- **Cluster**：單核心或所有核心、顯示或隱藏其他 session、所有核心模式下核心上方的名稱；重力（Floaty 到 Firm：主核心把小核心拉在軌道上的力道；搬動時它們沿著軌道滑到後方跟著走，用力甩出去會在螢幕邊緣反彈）和在核心上捲動滾輪時怎麼轉（Roll、Roll back、Turn、Turn back 或 Off）；多久沒操作後核心休息（STANDBY：1 分鐘到 1 小時，或永不）。
- **Replies**：對話框的字型（終端機的等寬字型，讓回覆排列和 Claude Code 裡一樣，中文用微軟正黑體、每字佔兩格，和 Windows Terminal 相同；或 Segoe UI；回覆輸入框一律用終端機字型）和文字大小；對話框大小、底色（五種深色）和文字顏色（Auto、White、Cream、Mint、Sky），兩列各有一格調色盤，可以打開 Windows 的色彩視窗挑任何顏色（Auto 時，淺色框上的文字會變深色），以及出現的位置（核心旁邊，或螢幕中間）。
- **Motions**：左邊是每個狀態和它所用常駐動作的圖示；右邊是選中狀態的常駐動作（每次進入狀態時挑一種；任何動作都可以給任何狀態，只有黑洞限睡眠時使用）和小動作（沒被碰時偶爾做），用中的排在前面。點一下就在預覽裡試播；把它拖到上面用中的那一區就會用它，拖到下面的 MORE 就不再用。**Try it** 播放這個狀態的一種常駐動作；**Reset** 把這個狀態恢復預設。Asleep 在黑洞模式時有自己的一頁：黑洞本身（Lensing 開或關、它自己傾斜翻轉的速度）、它的動作列表（點一下或按播放鍵就能看；開關決定它會不會自己演出）、你可以用滑鼠對它做的事，最下面是 Asleep 可以挑的常駐動作。
- **Shortcuts**：在任何程式裡都能用的快捷鍵：單核心 / 所有核心（Ctrl+Alt+C）、顯示或隱藏 Vigil（Ctrl+Alt+V）、回覆主核心（Ctrl+Alt+R）、打開設定（未設定）；點一下再按新的組合鍵（Esc 取消，Delete 清除）；被其他程式占用的會標出來。移動（拖曳）或搬動（點一下）核心時要按住的鍵：Alt、Ctrl 或 Shift。回覆列用 Enter 還是 Ctrl+Enter 送出。回覆列也接受命令列的按鍵：沒有選取文字時 Ctrl+C 清空整行（有選取時是複製），Ctrl+W 或 Ctrl+Backspace 刪掉游標前的一個字詞，Ctrl+U 刪掉游標前的全部。
- **Performance**：流暢度，多久畫一次（Saver、Balanced、Smooth、Max）。

每頁都有自己的 **Reset**；側欄最下面的 **Reset everything** 會回到啟動選項。選了立刻生效，記在 `~/.dodeca-core/panel-settings-win.json`，在重設之前優先於啟動選項；按 Esc 關閉視窗。

右鍵 > **Smoothness** 設定每顆核心多久重畫一次：**Saver**（最省 CPU）、**Balanced**（預設）、**Smooth**（忙碌時每秒 60 張）或 **Max**（螢幕的更新率）。不論選哪個，移動中的核心都跟著螢幕的節奏走：面板依螢幕的更新率計時（有東西在動時每秒 60 次），漂浮的衛星不會再一頓一頓。Settings > Performance 有同樣的預設，下面還能分別設定每種時刻每秒畫幾張（閒置、思考、忙碌、睡著或離開、拿在手上；2 到 120，或 Max，也就是螢幕的更新率）：動了任何一個就變成你自己的 Custom。**Drawing threads** 選擇幾個執行緒一起畫大核心（Auto：大約每個實體核心一個）；**Black hole drawn on** 選擇由 GPU、CPU 或 **CPU light**（背後沒有星星、演出時每秒最多 30 張：用 CPU 也接近 GPU 的流暢度）繪製黑洞（沒有可用的 GPU 時會說明原因，改由 CPU 繪製）。啟動時可用 `--smooth NAME` 指定；`--stats` 每 5 秒把 tick、畫面數和它們的花費寫到事件檔旁的 `panel-stats.txt`。黑洞在有 GPU 時由 GPU 繪製（OpenGL，Windows 內建）：CPU 花費約只有原本的六分之一；沒有 GPU（遠端桌面、部分虛擬機）或加上 `--no-gpu` 時改由 CPU 繪製。每個螢幕都用它自己的縮放（125%、100%……）來畫，所以核心在螢幕之間搬動時一樣清晰、一樣大小。

其他也都有：所有動作、鎖定、衝撞防禦、按壓、拖曳轉動、雙擊光圈。還沒有：語音；iTerm2 相關功能在 Windows 上沒有對應。Windows Terminal 無法顯示 kitty 或 iTerm2 的影像，所以終端機版本在那裡用半格字元繪製。

</details>

<details>
<summary><b>iTerm2 側邊窗格</b></summary>

`--pane` 是給窄窗格用的直排版面：session 由上到下，每個先兩行文字（名稱、狀態），下面才是多面體。文字用的是終端機本身的字體，所以跟同一個 profile 的其他窗格一樣大。

```sh
./vigil --pane --demo                # 在一個窄窗格裡先看效果
python3 install_hooks.py --pane --apply    # 之後每次開 Claude Code 自動開窗格
```

加了 `--pane` 安裝之後，`SessionStart` 會多執行 `open_pane.sh`：把 Claude Code 所在的窗格往右切一塊跑 `vigil --pane --exit-when-empty`，縮到 34 個字元寬，再把鍵盤焦點還給 Claude Code。已經有窗格開著就不會再開；最後一個 session 結束 15 秒後窗格自己關掉。

- 窗格寬度：環境變數 `DODECA_PANE_COLS`（預設 34）。
- 縮窄窗格要靠 iTerm2 的 Python API（`pane_api.py`）：AppleScript 能切窗格，但在全螢幕視窗裡不能調寬度。需要兩個一次性步驟：在 iTerm2 的 Settings > General > Magic 勾選 Enable Python API；再把腳本複製到 iTerm2 找腳本的資料夾：`mkdir -p ~/Library/Application\ Support/iTerm2/Scripts && cp pane_api.py ~/Library/Application\ Support/iTerm2/Scripts/vigil_pane.py`（之後 `open_pane.sh` 會自動保持這份複本是最新的）。沒設定的話窗格會是一半寬，可以自己拖分隔線，面板會跟著變。
- 除錯：`DODECA_PANE_DEBUG=1 sh open_pane.sh` 會在前景執行並顯示 iTerm2 的回應。
- 額外參數：環境變數 `DODECA_PANE_ARGS`，例如 `"--shape geo2 --res 1200"`。
- 第一次執行時 macOS 會問是否允許控制 iTerm2，要按允許。
- 不用自動開的話，自己切一個窗格跑 `./vigil --pane` 也一樣。

</details>

<details>
<summary><b>輸出方式</b></summary>

程式先在記憶體裡畫出一張 RGB 影像，再用三種方式之一送到終端機，啟動時會自動偵測：

| 模式 | 說明 |
|---|---|
| `--kitty` | kitty graphics protocol，真像素。終端機回應支援時優先使用 |
| `--iterm` | iTerm2 inline image（PNG），真像素。在 iTerm2 裡且不支援上一種時使用 |
| `--cells` | 半格字元 `▀`，每個字元格兩顆像素。任何 truecolor 終端機都能用，但很粗；tmux 裡固定用這個 |

自動偵測的結果不理想時可以用參數強制指定。

</details>

<details>
<summary><b>選項</b></summary>

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

</details>

<details>
<summary><b>架構</b></summary>

```
Claude Code ──hook.py──▶ ~/.dodeca-core/events.jsonl ──tail──▶ 狀態機 ──▶ 軟體渲染 ──▶ 終端機
```

| 檔案 | 內容 |
|---|---|
| `mac/panel.m` | 浮動面板：Cocoa 視窗、文字標籤、把畫布轉成透明影像 |
| `pane_api.py` | 用 iTerm2 Python API 切窗格並設定寬度 |
| `open_pane.sh` | `SessionStart` hook：在 iTerm2 裡自動切出側邊窗格 |
| `hook.py` | 每個事件 append 一行 JSON：時間、session id、事件名、資料夾名、工具名、通知類型（還有子代理的 id 和類型） |
| `vigil-claude` | 啟動 Claude Code，讓 Windows 面板不用把終端機叫到前面就能把回覆貼進去 |
| `src/events.c` | 跟著事件檔讀新增的行 |
| `src/session.c` | 事件對應到狀態，以及每個狀態的外觀參數表 `LOOKS` |
| `src/geom.c` | 產生正十二面體、截角二十面體、測地球的頂點與邊 |
| `src/render.c` | 把線條、光暈、核心、標籤畫進 float RGB 畫布 |
| `src/motion.c` | 每個狀態怎麼動：轉動、碎片、尖刺、每種動作一個 routine、閒置時的小動作 |
| `win/panel.c` | Windows 面板：分層視窗、軌道物理、設定視窗 |
| `src/main.c` | 主迴圈、終端機設定、三種輸出方式、demo |
| `src/wasm.c` | 把共用的 C 編成 WebAssembly，給 `web/` 裡的網頁用 |
| `web/bestiary.html` | Core Bestiary：每種動作的提案與現況；**Today** 跑的是面板自己的 C |
| `web/settings-mock.html` | 重新設計的設定視窗模型（拉動瀏覽器視窗大小試試） |

顏色在 `src/session.c` 的 `LOOKS` 表；每個狀態怎麼動是 `src/motion.c` 裡的 routine。

`web/` 裡的網頁直接從磁碟打開就能用。Bestiary 的 **Today** 要先把 C 編成瀏覽器用的版本：`sudo apt install clang lld wasi-libc libclang-rt-dev-wasm32`，然後 `make wasm`（產生 `web/vigil.wasm` 和 `web/vigil-wasm.js`，這兩個不進版控）。打開 `web/bestiary.html#today` 會直接從 Today 開始。

</details>

<details>
<summary><b>已知限制</b></summary>

- 權限請求核准之後，Claude Code 不會再送一次事件，所以那個工具執行期間仍顯示 NEEDS YOU，要等工具跑完才會切走。
- 標籤用的是內建的 3×5 點陣字，只有英數和少數符號；資料夾名稱裡的中文會顯示成 `?`。

</details>

## 致謝與授權

Vigil 由 **Zaviel**（[zelvet-velvet](https://github.com/zelvet-velvet)）開發：活的核心與 Core Bestiary 動作、Windows 面板、單核心模式與拿著走、各種互動、語音，以及設定視窗。

它源自 **Shang-En Hsu**（[Ehsu745](https://github.com/Ehsu745/dodeca-core-cc-status)）的 **dodeca-core**：最早的多面體渲染、終端機輸出方式、第一版 macOS 浮動面板和 iTerm2 側邊窗格都來自 dodeca-core。dodeca-core 本身又以 Zaviel 的 [clawd-room](https://github.com/zelvet-velvet/clawd-room) 為起點：`hook.py`、`install_hooks.py` 和 `src/events.c` 的事件讀取改寫自它。黑洞的外觀參照 Mayank 的 [blackhole-simulation](https://github.com/steeltroops-ai/blackhole-simulation)（MIT）：它的光線追蹤、吸積盤和光環移植成 C（`src/render.c` 的 `bh2_*`），`web/blackhole-mock.html` 則把它的 shader 和我們的並排執行以便調整。

MIT 授權；[LICENSE](LICENSE) 保留兩行版權聲明。
