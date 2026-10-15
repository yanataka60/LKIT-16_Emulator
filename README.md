# PANAFACOM LKIT-16 EMULATOR
　PANAFACOM LKIT-16のEmulatorを作ってみました。

　動作には実機のMonitor-ROMのバイナリファイルが必要です。

　また、BASIC-ROM、CG-ROMのバイナリファイルがあればBASICも動作可能です。

## MONITOR-ROMの吸い出し
　大きく分けて３つの方法があると思います。

### 1)自力で吸い出せる方
　一つだけ注意してほしいのは基板上では左側のROMが上位バイト、右側のROMが下位バイトのデータとなっており、Emulatorでは２つの内容を合成したLKIT-16 ROM.binというファイルにする必要があります。

### 2)ユーザーズマニュアルに掲載されているアセンブルリストからバイナリデータを打ち込む。
　手間と時間が掛かりますが、確実な方法と言えるとは思います。

### 3)モニタコマンドからCMTにSAVEする操作を行い、パソコンにWAVファイルとして保存し、バイナリファイルに変換する。
　MONITOR-ROMをそのままCMTにSAVEする操作は拒否されてしまうのでちょっと手間が掛かりますが、確実ではあります。

#### 手順
　ROMを256語ずつRAMへコピーし、4回に分けてSAVEします。

　作業には0040～0052と1000～10FFを使用しますので1000～10FFにRAMが実装されていないと実行できません。

##### 1. コピープログラムを入力する
　以下のプログラムをパネルから入力します。

|アドレス|入力値|処理|
| ------------ | ------------ | ------------ |
|0040|C350|コピー元を取得|
|0041|C451|コピー先を取得|
|0042|C252|ワード数を取得|
|0043|E000|ROMから1ワード読む|
|0044|A800|RAMへ1ワード書く|
|0045|4B01|コピー元を進める|
|0046|4C01|コピー先を進める|
|0047|4241|残りワード数を減らす|
|0048|C743|コピーを繰り返す|
|0049|C749|完了後、ここで待機|

##### 2. コピー条件を設定する
|アドレス|入力値|意味|
| ------------ | ------------ | ------------ |
|0050|0100|コピー元ROMの開始番地|
|0051|1000|コピー先RAMの開始番地|
|0052|0100|コピーする数(256ワード)|

##### 3. コピーを実行する
　開始番地0040を設定してRUNを押し実行します。

　コピーはすぐに終了し、0049で待機します。少し待ってSTOPを押し、モニタへ戻してください。

|アドレス|入力値|
| ------------ | ------------ |
|0017|0040|

##### 4. コピーしたRAMをSAVEする
　保存開始・終了番地を設定します。

|アドレス|入力値|
| ------------ | ------------ |
|000C|1000|
|000D|10FF|

　PCで録音を開始してから、ST／R1 → FUNCを押し、SAVE処理を実行します。

　SAVE処理が完了したら録音を停止し、ROM0100.wavとして保存します。

##### 5. 残りの3区間も保存する
　0050番地を次表の値へ変更し、手順3から繰り返します。0051・0052とSAVE範囲は同じです。

|回|0050の設定値|元のROM範囲|wavファイル名|
| ------------ | ------------ | ------------ | ------------ |
|1|0100|0100～01FF|ROM0100.wav|
|2|0200|0200～02FF|ROM0200.wav|
|3|0300|0300～03FF|ROM0300.wav|
|4|0400|0400～04FF|ROM0400.wav|

##### 6. WAVをBINに変換します
　Wav2Binフォルダのlkit16_wav2bin.exeを使ってバイナリファイルに変換します。コマンドラインで実行してください。

|実行コマンド|
| ------------ |
|lkit16_wav2bin ROM0100.wav ROM0100.bin|
|lkit16_wav2bin ROM0200.wav ROM0200.bin|
|lkit16_wav2bin ROM0300.wav ROM0300.bin|
|lkit16_wav2bin ROM0400.wav ROM0400.bin|
|copy /b ROM0100.bin +ROM0200.bin +ROM0300.bin +ROM0400.bin "LKIT-16 ROM.bin"|

　LKIT-16 ROM.binが出来上がるはずです。

## 使い方
　まず、MONITOR-ROMを吸い出す必要があります。

　吸い出す術がわからない方はユーザーズマニュアルに掲載されているアセンブルリストを入力するという手もあるかもしれません。

　基板上で左側のROM(上位バイト)の内容をLKIT-16 ROM_A.bin、右側(下位バイト)の内容をLKIT-16 ROM_B.binとしてください。

　適当なフォルダにLkit16Emulator.exe、LKIT-16 ROM_A.bin、LKIT-16 ROM_B.binを置き、ダブルクリック等で起動させます。

　起動した状態でメインパネルが操作可能になります。

![Ramda-1](https://github.com/yanataka60/Ramda-1-Emulator/blob/main/PNG/MAIN%20PANEL.png)

　メインパネルの操作はマウスでのクリック操作となりますが、メインパネルがアクティブなときには次のキーがWindowsキーボードに割り当てられています。

|メインパネルの操作|Windowsキー|備考|
| ------------ | ------------ | ------------ |
|STOP|F5||
|RUN|F6||
|FUNC|F7||
|CANCL|F8||
|SET ADRS|F9||
|ASMBL|F10||
|STORE|F11又はENTER||
|DISP|F12||
|L/R0|F1||
|ST/R1|F2||
|B/R2|F3||
|BAL/X0|F4||
|0～9|0～9|テンキーも可|
|A|A又はテンキーの/||
|B|B又はテンキーの*||
|C|C又はテンキーの-||
|D|D又はテンキーの+||
|E|E又はテンキーのENTER||
|F|F又はテンキーの.||

### ファイル入出力
　バイナリファイルの読込、保存の方法は二通りの方法があります。

![Memory INOUT](https://github.com/yanataka60/Ramda-1-Emulator/blob/main/PNG/Memory%20INOUT.png)
#### BIN/BTKメモリ入出力
　バイナリファイルを直接RAMにロード、RAMからセーブします。
##### BIN読込
　BIN読込開始アドレスを16進数4桁で指定して拡張子binのバイナリファイルをLOADします。
##### BTK読込
　拙作LKIT-16_SDで使用している保存形式BTKファイルを読み込みます。BTKファイルには読込開始アドレス、読込バイト数の情報があるのでアドレス等の情報を指定する必要はありません。
##### BIN保存
　設定した保存範囲を単純なバイナリファイルとして指定したファイル名で保存します。拡張子はBINとなります。
##### BTK保存
　設定した保存範囲を拙作LKIT-16_SDで使用している保存形式BTKファイルとして指定したファイル名で保存します。拡張子はBTKとなります。

#### カセットWAV読込
　読み込むWAVファイルを選択した後にL/R0,FUNCとCMT読込操作を行ってください。

#### カセットWAV保存
　保存するWAVファイル名を入力又は選択した後にST/R1,FUNCとCMT保存操作を行ってください。

　ファイルはWAVファイルとなります。

![CMT](https://github.com/yanataka60/Ramda-1-Emulator/blob/main/PNG/CMT.png)

### BASIC起動
　BASIC-ROMのバイナリファイル、CG-ROMのバイナリファイルが必要です。CG-ROMはWindowsフォントから疑似的に作ることが可能です。

　MONITOR-ROMと同様にBASIC-ROMの内容を抽出し、BASIC-ROM1.Bin

　
　RAMの内容が確認できます。表示のみで書き換えることはできません。

![Memory Disp](https://github.com/yanataka60/Ramda-1-Emulator/blob/main/PNG/Memory%20Disp.png)

### プリンタ
　拡張PIA#2へのプリンタ出力がそのまま表示されます。

　出力ファイルを指定して「受信データをテキストファイルに書き出す」をチェックすればテキストファイルとして保存も出来ます。

![Printer](https://github.com/yanataka60/Ramda-1-Emulator/blob/main/PNG/Printer.png)

### 電子オルガン
　このパネルを表示しなくても電子オルガンは接続済みです。手動でオルガンから音を出すためのものです。

![Organ](https://github.com/yanataka60/Ramda-1-Emulator/blob/main/PNG/Organ.png)

## ソフトウェア
### CHICK-BUG
　Ramda1Emulator.exeと同じフォルダにCHICK-BUG.binがある状態で起動するとアドレスがFE78にセットされ、カラーTVディスプレイのウィンドウが表示されます。

![CHICK-BUG1](https://github.com/yanataka60/Ramda-1-Emulator/blob/main/PNG/CHICK-BUG1.png)

　この状態でGO-SWを操作するとCHICK-BUGが起動し、ATNキーを押すことでコマンド待ちになります。

![CHICK-BUG2](https://github.com/yanataka60/Ramda-1-Emulator/blob/main/PNG/CHICK-BUG2.png)

![CHICK-BUG3](https://github.com/yanataka60/Ramda-1-Emulator/blob/main/PNG/CHICK-BUG3.png)

　操作はパソコンのキーボードか、又は表示メニューから仮想フルキーボードを表示させて操作することも出来ます。

![Keyboard](https://github.com/yanataka60/Ramda-1-Emulator/blob/main/PNG/KeyBoard.png)

　メインパネル、カラーTVディスプレイ、仮想フルキーボード、RAMモニタのいずれかがアクティブなときにはWindowsキーボードから操作が出来ます。

　Windowsキーボードにはないキーの割り当ては次の通りです。
|仮想フルキーボート|Windowsキー|
| ------------ | ------------ |
|ATN|Home|
|LF(0Ah)|下向きカーソルキー|
|HTAB(09h)|Tabキー又は上向きカーソルキー|

　なお、WindowsキーボードのENTERはCR(0Dh)の入力となります。LF(0Ah)と混同しないにしてください。

### Assembler
　アセンブラの起動には巻末付録のユーティリティプログラムが必要です。

　Ramda1Emulator.exeと同じフォルダにAssembler.binとUTILITY.binを置いてRamda1Emulatorを起動させると自動的に読み込みます。

　CHICK-BUGが起動したらGコマンドでA000hにジャンプするとメニューが表示されるので5を選択するとAssemblerが起動します。

![Assembler1](https://github.com/yanataka60/Ramda-1-Emulator/blob/main/PNG/Assembler1.png)

![Assembler2](https://github.com/yanataka60/Ramda-1-Emulator/blob/main/PNG/Assembler2.png)

![Assembler3](https://github.com/yanataka60/Ramda-1-Emulator/blob/main/PNG/Assembler3.png)

### 電大版Tiny-Basic
　電大版Tiny-Basicの起動には巻末付録のユーティリティプログラムが必要です。

　Ramda1Emulator.exeと同じフォルダにTinyBasic.binを置いてRamda1Emulatorを起動させると自動的に読み込みます。

　CHICK-BUGが起動したらGコマンドでA000hにジャンプするとメニューが表示されるので6を選択すると電大版Tiny-Basicが起動します。

![Tint-Basic](https://github.com/yanataka60/Ramda-1-Emulator/blob/main/PNG/Tiny-Basic.png)
