# 端末描画の性能調査と改善方針

## 保存時点の結論と作業状況

2026年10月17日までに行った性能改善、測定、切り分け実験と考察を記録する。
対象は[boxrig移行計画](plan2-boxrig-migration.md)に続く、elder-termsの端末表示の遅さである。

今回のGTK3・X11環境では、ステータスバーと通信インジケーターの更新に伴うGTKの追加合成が、boxrig_termとの差の主因だった。
端末の文字描画にかかる時間は、同じ条件のboxrig_termとほぼ同じだった。
一時コードでステータスバーの描画ウィンドウを親と共有し、インジケーターの画像更新を止めると、出力からGTKの合成完了までの中央値は132.1 msから70.4 msへ短縮した。
同条件のboxrig_termは68.7 msだった。

この切り分けは完了したが、ステータスバーとインジケーターの製品向け修正は未実施である。
インジケーターを停止した実験結果を、点滅機能を保った修正の効果として扱うことはできない。
既に反映したelder-terms側のスケジューリング改善と、boxrigの読み上げ用本文公開の改善は維持している。

| 対象 | 測定対象のリビジョン・状態 |
| --- | --- |
| elder-terms | `b07927786ebc0dbbcfd6a2f008b0370c8d06a639`、`feature/boxrig` |
| elder-termsが参照するboxrig | `1d379548fd0de1ff7b15725cad90f4e1136d732b` |
| libxyzm | `b1cd04cfd2769bbd1a5a6bc6990d60338b5a3cf5`、今回の描画調査では変更なし |
| cardio | `31e8149ce52cdfdaaa48838ca9de39288074885c`、今回の描画調査では変更なし |
| boxrig側の作業 | `feature/accessibility-update-policy`を作成してpush済み。作業者によるdevelop／mainへのマージは行っていない |
| 保存時の別checkout | `~/Projects/boxrig`は利用者による取り込み後の`develop`、`6402257`。測定ではelder-termsの固定参照を使用 |
| 今後の依存更新 | 利用者がboxrigをリリースした後、elder-termsの固定参照を追従させる予定。今回の保存では更新しない |

## 調査のきっかけと範囲

利用者は`npm run dev`で起動し、ローカルのbash操作とbtopの両方で遅さを確認した。
boxrig_termは試した種類のいずれも高速で、elder-termsは体感で3倍以上遅かった。
初期の修正で改善した後も、同程度のウィンドウサイズ・行桁数のGNOME Terminalとの差が残った。
特にbtopのプロセスリストをスクロールすると差が明確だった。

まずelder-terms内で改善できる処理を調べ、その後にboxrigの読み上げ機能を改善した。
読み上げ機能の切り替えだけでは体感差を十分に説明できなかったため、受信からGTKの合成完了までを区間別に測定した。
比較には専用プロセスを起動し、利用者が動かしている古いelder-termsを測定対象に混ぜなかった。

ここでいう「読み上げ用本文公開」は、端末の本文・属性・選択・カーソル位置をアクセシビリティAPIへ渡す処理である。
音声を生成するスクリーンリーダーそのもののON／OFFとは区別する。
性能試験ではAT-SPIへの接続を無効にし、Orcaも接続しなかった。
そのため、本文を準備する負荷は測定しているが、スクリーンリーダーとの通信や発話の負荷は含まない。

## 第1段階 elder-terms内で実施した改善

### I/OとGTK描画の実行順

CardioのI/O処理をGLibのsourceとして登録し、読み取り可能な通信をメインループから進めるようにした。
これにより、受信が既に可能な状態で描画用idleだけが先行する問題を修正した。
続いて、連続受信や処理の続行要求がGTK描画を待たせ続けないように、受信通知と継続処理の優先度を調整した。
ネイティブ入力やwidget自身の要求は、必要な優先度で処理する。

約1 MBの連続文字出力では、初期状態の18.7086秒から、修正後3回平均の3.7534秒へ短縮した。
これは出力用プロセスを接続したアプリの終了までを測った値で、1回の入力に対する画面表示時間ではない。
大量出力の処理能力は改善したが、btopのスクロールの遅さは残った。

### 画面が変化しないときのframe要求

boxrigの公開`frame()`は、レイアウト処理や描画予約に加えて読み上げ用本文の公開も進める。
elder-termsでは、リンク認識の続行だけでもこの処理を繰り返していた。
通信・認識処理の続行と、widgetの新しいフレームが必要な状態を分けた。

同じサイズのallocation通知と送信だけの入力にも、不要なframe要求があった。
実際の端末領域の変更はwidgetからの要求に任せ、hostのサイズ変更要求が進行中の場合だけallocation通知を使うようにした。
通信結果も「続行可能」と「受信した本文がある」を分離し、キーの送信だけでは画像を更新しないようにした。
DEC 2026の同期出力が閉じた場合には、既にdirtyな画面でも必要なフレームを要求する。

| コミット | 内容 | 回帰試験で確認したこと |
| --- | --- | --- |
| `64ed66d` | I/OをGLibメインループへ接続 | 実際のアプリ入口で、既に読み取り可能なpipeを処理する |
| `8ae5719` | 連続受信の間にGTK描画を実行 | 未処理の受信が残っていても描画処理へ進む |
| `0dbc6fd` | 本文認識だけの続行でframeを作らない | 認識の進行と必要な描画を両立し、同期出力の完了を表示する |
| `b4dcb33` | 同寸法のallocationによるframeを抑制 | 同じ端末領域の再割り当てで余分なframeを要求しない |
| `493b6ee` | 送信だけの入力によるframeを抑制 | 画面が静止した状態でもキーを送信し、受信時には表示を更新する |

40更新のbtop再生による探索測定では、公開`frame()`の呼び出しが314回から132回へ減った。
同寸法allocationの対策を比較した別の試行では131回から53回へ減り、そのうち表示の変更を必要としない呼び出しは84回から6回になった。
これらの回数はGTKの実描画回数とは異なる。

I/O改善後のruntimeと、不要なframe要求を抑えたruntimeを比較した別の40更新では、公開`frame()`が1,000回から56回になった。
呼び出し時間の累計は1,371.1 msから76.8 msへ減った。
この比較では、`d3234d6`のruntimeソースを当時の通信APIへ合わせて再リンクした診断用バイナリを基準にしている。
出力からwidgetのdraw終了までは69.1 msから61.7 ms、録画データの到着までは142.2 msから99.9 msだった。
当時はCPU周波数をそろえておらず、連続更新の反復結果にもばらつきがあった。
不要な呼び出しの削減は確認できたが、これらの単発の遅延値から改善率を確定しなかった。

実装と試験は[terminal-runtime.cpp](../../../elder-terms-terminal/src/terminal-runtime.cpp)、[terminal-connection.cpp](../../../elder-terms-terminal/src/terminal-connection.cpp)、[terminal-render-work-test.cpp](../../../elder-terms-terminal/tests/cpp/terminal-render-work-test.cpp)、[terminal-dispatch-test.cpp](../../../elder-terms-terminal/tests/cpp/terminal-dispatch-test.cpp)にある。

### 初期の比較で残った候補

フォントをMonospaceへ変えた試行や、トップレベルへRGBA visualを指定した試行では、明確な解消には至らなかった。
探索時のwidget draw終了／録画到着の中央値は、Monospaceで107.5／177.1 ms、RGBA visualで109.5／183.2 msだった。
同じ段階の通常構成は100.1／173.1 msだったが、反復や周波数の条件がそろっていないため優劣の確定には使わない。

Cairoへ切り替えた探索測定は44.8／79.8 msで、OpenGL構成より短かった。
これはOpenGLとGTKの接続部分を調べる理由になった。
描画方式全体を置き換える判断には使わず、OpenGLのままboxrig_termとの差を切り分けることにした。

GNOME Terminalとelder-termsで実際のbtopをスクロールした記録や、CPUプロファイルも取得した。
ただし、別々のbtopは出力内容も更新時刻も変わる。
以後の定量比較には、同じbtop出力を繰り返し再生する方法を使った。

## 第2段階 読み上げ用本文公開の改善

### 採用した案と実装内容

利用者が採用したのは、本文公開の切り替えと、変化していない本文スナップショットの再利用である。
スナップショットは、本文・属性・選択などを読み上げ側へ一貫して渡すために保持する状態を指す。
boxrigライブラリとelder-termsの両方で、既定値をOFFとすることが合意された。

boxrigではGTK3／GTK4の設定へ`accessibility_enabled`を追加した。
公開APIとして`boxrig_gtk3_terminal_widget_set_accessibility_enabled()`と対応するgetterを設け、GTK4にも同じ操作を用意した。
OFFでは公開済み本文を取り下げ、不要なスナップショットを作らない。
ONへ切り替えると次のフレームで公開を再開する。
同期出力中やcallback実行中の操作には、既存の同期・寿命・`BUSY`契約を適用した。

再利用の判定には、本文、buffer、色、model、行レイアウト、選択のrevisionを使う。
viewport、カーソル位置、行桁数も確認する。
不変の行レイアウトを再設定しただけではrevisionを進めないようにした。
一方、本文・属性・選択・カーソルが変わる場合は、更新を読み上げ側へ届ける。
これはスナップショット全体の再利用であり、変更行だけを再構築する実装ではない。

| リポジトリ | コミット | 内容 |
| --- | --- | --- |
| boxrig | `6c20311` | GTK3／GTK4の本文公開を既定OFFとし、実行中の切り替えを追加 |
| boxrig | `3eb21e1` | 変化していないスナップショットと行レイアウトを再利用 |
| boxrig | `1d37954` | 新しい公開APIの契約試験を登録 |
| elder-terms | `a3fd9c9` | 「端末本文の読み上げ」設定を追加し、起動時・実行中へ反映 |
| elder-terms | `ae76bb6` | 本文公開OFFでも選択範囲のコピーを利用可能にした |
| elder-terms | `b079277` | 契約試験を含むboxrigの固定参照へ更新 |

elder-termsの保存設定は`[terminal] accessibility_enabled=false`である。
設定変更はその場で反映し、再接続後も維持する。
コピーの有効判定はアクセシビリティ本文への依存をやめ、`boxrig_ui_view_get_selection()`を使う。
本文をアクセシビリティAPI経由で読む既存試験では、必要なケースで明示的にONにした。

上流の実装は[GTK3 widget][boxrig-widget]、[本文公開処理][boxrig-accessible]、[スナップショットの実装][boxrig-snapshot]を参照。
elder-terms側の反映処理は[terminal-presentation.cpp](../../../elder-terms-terminal/src/terminal-presentation.cpp)にある。

### 3条件の測定結果

旧版、更新版ON、更新版OFFで同じbtop出力を再生した。
190桁×66行、16 DIP、セル10×22 pxとし、40更新を1回ずつ表示確認してから次へ進めた。
3条件の順序を入れ替えた6通りを実行し、各条件240更新を比較した。
旧版はelder-terms `9671be2`／boxrig `3ce40c4`、更新版は本書冒頭の固定参照である。

時間はms。遅延は各条件240更新の中央値、CPU時間とスナップショットは6回の累計値の中央値を示す。

| 指標 | 旧版 | 更新版ON | 更新版OFF |
| --- | ---: | ---: | ---: |
| 出力からwidget draw終了 | 92.83 | 92.87 | 85.46 |
| 出力から録画データ到着 | 152.27 | 154.52 | 150.00 |
| プロセスCPU時間／40更新 | 4,147.18 | 4,252.17 | 3,766.31 |
| スナップショット作成回数／40更新 | 101 | 88.5 | 0 |
| スナップショット作成時間／40更新 | 555.83 | 505.66 | 0 |

OFFではスナップショット作成が0になり、widget draw終了までが約7.9%、CPU時間が約9.2%減った。
録画データ到着までの差は約1.5%だった。
ONの再利用では作成回数が減ったが、表示時間全体の明確な改善は確認できなかった。
CPU時間には起動処理と全スレッドの処理を含む。
録画到着には撮影とpipeへの配送時間が含まれるため、モニターに表示された瞬間の差を表す値ではない。

### 効果が限られた理由

同じ記録をdrawの開始・終了で分けると、OFFでもdraw開始前に約30.7 ms、draw中に約54.4 msが残った。
公開`frame()`の累計時間は大幅に減っていた。

| 区間・指標 | 旧版 | 更新版ON | 更新版OFF |
| --- | ---: | ---: | ---: |
| 出力からGTK draw開始までの中央値 | 30.54 | 28.31 | 30.70 |
| GTK draw開始から終了までの中央値 | 61.51 | 61.38 | 54.40 |
| draw終了から録画到着までの中央値 | 62.97 | 66.50 | 62.00 |
| 公開`frame()`の累計時間／40更新の中央値 | 314.74 | 264.22 | 1.95 |

公開`frame()`は描画を予約し、実際のOpenGL描画は後のGtkGLArea callbackで行う。
そのため、公開`frame()`だけを短縮しても、端末の描画やGTKの合成時間は残る。
btopでは本文・属性・カーソルなどが更新され続けるため、変化していない状態を再利用できる範囲も限られる。

draw開始前の時間には受信、構文解析、イベント処理、待ち時間が含まれる。
draw中の時間にもCPU処理と描画ドライバー内の待ちが混在する。
この時点では、それぞれを純粋な待ち時間やGPU処理時間とは断定できなかった。
また、draw終了後のGTK合成を測っていなかったことが、次の調査で分かった。

この結果から、読み上げ用本文公開は負荷の一部だったと判断した。
GTK版boxrig_termも同じ公開経路を使うため、これだけではelder-terms固有の大きな差を説明できない。
端末描画、GTK合成、通信・認識処理を分けて測る方針へ進んだ。

## 第3段階 描画経路の切り分け

### 比較条件と計測点

最終比較では、GTK3版boxrig_termをelder-termsと同じ固定参照の共有ライブラリへ接続した。
フォント、セル寸法、行桁数、OpenGL描画、読み上げ用本文公開OFFをそろえた。
トップレベルの外寸にはメニューなどの差があるが、端末が描く面積は同じである。

| 項目 | 条件 |
| --- | --- |
| GTK・表示環境 | GTK 3.24.41、X11、ホストの実ディスプレイ |
| GPU | NVIDIA GeForce RTX 3090、OpenGL 3.3、ドライバー615.71.09 |
| CPU | Intel Core i9-12900KS、24論理CPU |
| CPU設定 | `intel_pstate`、governor `powersave`、EPP `power` |
| 実行CPU | 測定用の子プロセスをCPU 2へ固定。システムの電源設定は変更していない |
| 測定中の周波数 | すべての条件で中央値800,000 kHz、約0.8 GHz |
| 端末 | 190桁×66行、Noto Sans Mono／Monospace、16 DIP、セル10×22 px |
| 端末描画面 | 1,900×1,452 px |
| トップレベル外寸 | elder-terms 1,965×1,587 px、boxrig_term 1,925×1,505 px |
| 録画 | マーカー領域を60 fpsで取得し、rawデータがpipeへ届いた時刻を記録 |
| その他 | `NO_AT_BRIDGE=1`、`GTK_IM_MODULE=gtk-im-context-simple` |

初期測定では、同じ描画処理のCPU時間が約4〜5 msと約25 msに分かれた。
CPU周波数の変動が比較を乱していたため、以後は子プロセスのCPUを固定して周波数も記録した。
最終値の絶対時間は、この0.8 GHzの環境に依存する。
GPUクロックは固定しておらず、一度の観測値だけからGPUの一定動作を仮定していない。

btopの記録には、DEC 2026による同期更新とプロセスリストのスクロールが含まれる。
各更新へ一意な色のマーカーを加え、書き込み、構文解析、描画、録画の記録を対応付けた。
元の出力ファイルのSHA-256は以下である。

```text
5a5a8bceee8f361658f3a91bb1aaf70db2c25f877bf72ce18102ce1e483daaf3
```

| 計測点 | 意味 |
| --- | --- |
| 出力 | 再生peerが端末向けデータを書き込む直前。単調増加時計を使用 |
| 解析完了 | マーカーを端末が受理し、同期出力が閉じた状態でfeedが終了 |
| draw開始 | マーカーを含む端末widgetのGTK draw開始 |
| widget draw終了 | 端末widgetのdraw終了。ログの`Native marker` |
| GTK合成完了 | そのdrawを含む`gdk_window_end_draw_frame()`の終了 |
| 録画到着 | マーカーを含む撮影データがffmpegのpipeへ到着 |

第2段階の85.46 msはwidget draw終了までで、今回の132.14 msはGTK合成完了までである。
CPUの制御条件も異なるため、この2値を比較して性能が悪化したとは判断しない。
GTK合成完了も物理モニターの走査完了時刻ではない。

### 測定用コードと比較した5条件

既存のboxrig静的objectを一時的な共有ライブラリへ再リンクし、描画と文字整形の処理時間を記録した。
GTK／GDKや端末feed、本文認識の公開関数にも計測を入れ、経過時間とthread CPU時間を比較した。
対象には`boxrig_gtk_gl_surface_draw()`、`boxrig_gtk_gl_surface_present()`、`boxrig_ogl_text_provider_shape_row()`、`gdk_cairo_draw_from_gl()`、`gdk_window_end_draw_frame()`が含まれる。
private関数を使う計測は`/tmp`の診断用コードだけに置き、製品へ依存を追加していない。

| 条件名 | 内容 |
| --- | --- |
| `off` | 現行elder-terms。本文公開OFF |
| `quiet` | `set_indicator_active()`内の画像更新を一時コードで止める |
| `sharedbar` | 一時UIで`status_bar`の`visible-window`だけを`False`へ変更する |
| `quietbar` | `quiet`と`sharedbar`を組み合わせる |
| `sample` | 同じ共有ライブラリ、フォント、端末寸法のGTK3版boxrig_term |

`quiet`は受信・送信の点滅だけでなく、同じヘルパーを使う接続状態の画像更新も止める。
機能を保った修正ではなく、インジケーターの描画負荷を切り分ける実験である。
一時バイナリ、UI、計測ライブラリは`/tmp/elder-render-bottleneck/`へ配置した。
第3段階では製品コード、インストール済みバイナリ、submodule参照を変更していない。

### 1更新ずつ表示を待つ比較

各条件で40更新を再生し、前のマーカーが録画へ現れてから次の更新を書き込んだ。
実行順を次の4通りに変え、5条件×4回、合計800更新を検証した。

```text
off       quiet     sharedbar quietbar sample
sample    quietbar  sharedbar quiet    off
quiet     off       sample    quietbar sharedbar
sharedbar quietbar  sample    off      quiet
```

以下は各条件160更新をまとめた中央値で、単位はmsである。
区間には包含関係があり、中央値も加算できないため、各行の値を足して全体時間を求めない。

| 指標 | off | quiet | sharedbar | quietbar | sample |
| --- | ---: | ---: | ---: | ---: | ---: |
| 出力→解析完了 | 23.75 | 11.54 | 26.18 | 11.58 | 9.24 |
| 解析完了→draw開始 | 7.29 | 2.89 | 8.57 | 2.91 | 2.73 |
| widget drawの処理時間 | 59.58 | 54.10 | 56.12 | 56.12 | 55.88 |
| boxrigのsurface draw | 27.36 | 27.35 | 27.28 | 27.28 | 27.21 |
| 上記に含む文字整形の累計 | 9.87 | 9.88 | 9.83 | 9.87 | 9.72 |
| surface present | 0.40 | 0.39 | 0.39 | 0.39 | 0.38 |
| `gdk_cairo_draw_from_gl()` | 22.63 | 19.86 | 22.25 | 21.75 | 21.22 |
| `gdk_window_end_draw_frame()` | 38.83 | 18.52 | 15.58 | 0.58 | 0.57 |
| 出力→widget draw終了 | 93.78 | 68.63 | 94.29 | 69.66 | 67.74 |
| 出力→GTK合成完了 | 132.14 | 87.63 | 111.62 | 70.39 | 68.71 |
| 出力→録画到着 | 163.95 | 116.30 | 133.46 | 99.44 | 99.15 |

`quietbar`では出力からGTK合成完了までが46.7%短縮し、sampleに近づいた。
一方、文字描画は全条件で約27.3 ms、文字整形の対象は中央値23行のままだった。
差が大きく減ったのは、端末の描画後にGTKがウィンドウを合成する区間である。

| 指標 | off | quiet | sharedbar | quietbar | sample |
| --- | ---: | ---: | ---: | ---: | ---: |
| 各試行の「出力→GTK合成完了」中央値の範囲、ms | 125.8〜137.9 | 80.1〜91.9 | 99.8〜125.4 | 68.4〜74.3 | 66.8〜76.8 |
| 40更新中のGDK描画完了回数の範囲 | 80〜85 | 41〜43 | 80〜83 | 40〜41 | 42 |

インジケーターの画像更新を止めると、GDKの描画回数もほぼ半分になった。
ステータスバーのウィンドウ共有だけでは、この余分な回数は残った。
全20試行で40個のdraw終了マーカーと録画マーカーを確認し、行桁数の一致と本文スナップショット作成0回も検証した。

### 連続更新での比較と計測の影響確認

次に、前の表示を待たず、25 ms間隔を目標に60更新を送った。
送信は約1.5秒で終わり、最終マーカーまでの到達を待った。
詳細計測付きで4条件を3回ずつ実行し、画面に現れた異なるマーカーの数を数えた。

| 条件 | 表示できた状態数、3試行 | 最初の出力から最終マーカーの録画到着まで、ms | 最終更新の出力から録画到着まで、ms |
| --- | --- | --- | --- |
| off | 4、3、4 | 1,725、1,709、2,181 | 107、220、572 |
| sharedbar | 5、3、6 | 1,672、1,720、1,635 | 196、156、146 |
| quietbar | 15、14、15 | 1,634、1,552、1,578 | 161、78、104 |
| sample | 16、15、16 | 1,605、1,593、1,602 | 95、115、118 |

詳細な関数フックと再リンクした計測ライブラリを外した対照試験も、順序を変えて2回ずつ行った。
マーカーの計測と本文公開OFFの確認は残した。

| 条件 | 詳細計測なしで表示できた状態数、2試行 |
| --- | --- |
| off | 3、3 |
| quietbar | 11、14 |
| sample | 15、12 |

計測の軽い対照試験でも、`quietbar`とsampleはoffより多くの中間状態を表示した。
全18試行、合計1,080更新について最終マーカーへの到達を確認し、観測した番号の逆行もなかった。
途中の更新は描画時にまとめられるため、表示状態数が60未満でも、受信文字が失われたことを意味しない。

この有限の再生では、offの3〜4状態に対してquietbarは11〜15状態、sampleは12〜16状態だった。
スクロールの滑らかさに大きな差が出るという利用者の観察と整合する。
ただし、一般的なFPSや実キー入力の応答時間を測った値ではない。
この段階ではGNOME Terminalへ同じ再生試験を行っていない。

## 問題箇所と動作の考察

### ステータスバーの描画ウィンドウ

[main-window.ui](../../../elder-terms-terminal/src/main-window.ui)の`status_bar`は`GtkEventBox`で、`visible-window=True`となっている。
この1項目だけをFalseにした実験で、`gdk_window_end_draw_frame()`が38.83 msから15.58 msへ短縮した。
インジケーター更新も止めると0.58 msになった。

GTKのwidget描画経路を追うと、端末のOpenGL描画後にステータスバーのdrawも通っていた。
その際、ステータスバーのローカル座標で端末側へ広がるclipが観測された。
GTKには、OpenGLへ反映済みの領域へCairoの描画が続く場合に、再合成が必要な領域を記録する処理がある。
描画終了時には、その領域をウィンドウへ合成する。
[clipの記録処理][gtk-mark-paint]と[ウィンドウの描画完了処理][gtk-end-paint]を参照。

実験で追加合成の関与は確認できたが、GTK内部のどの領域判定が最小であるべきかまでは確定していない。
GTK全般の不具合や、すべてのGPUで同じ遅延が起きるとする結論には広げない。

### 通信インジケーターの同じ画像への更新

[activity-indicator.cpp](../../../elder-terms-terminal/src/activity-indicator.cpp)の`set_indicator_active()`は、現在と同じ状態でも`gtk_image_set_from_pixbuf()`を呼ぶ。
受信・送信の通知から繰り返し呼ばれ、150 msの点滅タイマーからも更新される。
その結果、同じON画像を指定するだけの通知でもGTKの描画が追加される。

既存の`--test-latch-activity-indicators`で点灯を固定しても、通知のたびに画像を再設定する経路は残った。
CPUを固定した探索試行でも、draw終了／録画到着は91.2／149.6 msで、差は解消しなかった。
点滅タイマーだけを止めても十分ではない。

今後は、実際に表示画像が変わったときに更新する方式が候補になる。
画像の差し替えやテーマ変更時には、ON／OFFが同じでも新しい画像を反映する必要がある。
また、正しく点滅した場合の描画負荷は残る。
単に状態比較を加えるだけで`quietbar`と同じ効果になるかは未測定である。

### GTKとOpenGLの同期で待っていた箇所

GTK経由のGL呼び出しも追跡し、`glXWaitX()`と`glXBindTexImageEXT()`で大きな経過時間を観測した。
その区間のthread CPU時間は小さく、CPUが文字処理を続けているだけでは説明できなかった。
GTKのX11実装には、Cairoで描いたsurfaceをGLテクスチャとして使うため、これらを呼ぶ経路がある。
[GTKのX11テクスチャ合成処理][gtk-x11-texture]を参照。

`glXSwapBuffers()`は通常約0.1 msだった。
調べたGTK経路では`glReadPixels()`は観測されず、CPUへ全画像を読み戻すfallbackが主因という証拠も得られなかった。
GTKには効率的なGL経路と読み戻しを伴うfallbackの両方があるため、実際に通った呼び出しを区別した。
[GTK公式のGL描画API説明][gtk-from-gl]を参照。

`__GL_SYNC_TO_VBLANK=0`を指定した探索試行でも差は残った。
その1回のdraw終了／録画到着はelder-termsが77.7／135.1 ms、sampleが60.7／82.9 msだった。
反復数が少ないため垂直同期の影響全般を否定する結果ではないが、設定変更だけで解決する根拠はなかった。

### 全画面blitと文字描画

boxrigは保持している描画結果をフレームバッファへコピーする。
この全画面blitを候補として調べたが、`surface_present()`のAPI呼び出し時間は約0.4 msだった。
さらに、同じ描画とblitを残したままelder-termsとsampleとの差をほぼ解消できた。
したがって、今回のアプリ間の差を説明する主因としては支持されなかった。
[boxrigのGL surface実装][boxrig-surface]を参照。

約0.4 msはCPU側で呼び出しにかかった時間であり、GPU timestampで測ったblit単体の実行時間ではない。
描画処理全体にはまだ負荷があるが、今回の差を直すために先にレンダラーを作り替える根拠はない。
文字整形も全条件で約9.8 msと近く、フォント差や文字整形だけではelder-termsの追加遅延を説明できなかった。

### 次に残る本文リンク認識の負荷

40更新の間に本文リンク認識へ使った累計時間は、elder-termsで約439〜499 ms、sampleで約168〜174 msだった。
端末feedの累計は双方とも約190〜205 msで、構文解析だけに大きな差はなかった。
認識の一部はidleで進むため、累計時間の差をそのまま各画面の遅延へ足すことはできない。

elder-termsの既定ルールは4個だが、本文を走査するルールは1個で、残り3個はOSC 8用である。
sampleも本文用のURL認識器を1個登録する。
elder-termsの既定式にはUnicodeの空白区切りを扱う条件があり、sampleの式はASCIIを基準にしている。
ただし、式の差だけを独立して比較していないため、認識時間の差をその条件だけに帰することはできない。
[elder-termsの既定式](../../../shared/src/settings/hyperlink-settings.cpp)と[boxrig_termの認識処理][boxrig-sample-links]を参照。

CPUプロファイルにも認識やレイアウトの負荷が現れたが、GTK／ドライバーの未解決symbolが残る記録もあった。
包含関係のある割合を加算せず、今回の結論は区間計測と変更条件の比較を中心に判断した。
リンク認識は次の改善候補として残す。

## 検証結果と残る制約

第1段階の修正では、再現試験のREDを確認してから修正した。
GLibの実行順、継続受信中の描画、認識だけの続行、同じallocation、送信だけの入力を試験している。
測定準備で見つかった起動直後の受信ログの取りこぼしは`de474ea`で修正した。
転送進捗の撮影方法と受信peerの準備待ちは`0495e59`、`9671be2`で修正し、描画性能の改善とは別に検証した。

本文公開機能の追加後、elder-termsでは全体試験を実行し、修正が必要だったsuiteを再実行した。
最終集計は重複を除いて928件成功、3件skipだった。
内訳はshared 124件、launcher 109件、native 153件、terminal 542件である。
設定画面の共有マスター画像6件も更新して目視確認した。

boxrigではGTK3／GTK4の既定OFF、実行中の切り替え、同期出力、callbackの寿命、スナップショットの再利用を検証した。
選択・カーソル・属性・本文の変更、IME、Cairo／OpenGL、実AT-SPI、Orcaによる発話も確認した。
公開API一覧への追加漏れを修正し、72ヘッダーの1,058宣言を分類した。

boxrigの全体試験は実行したが、全件成功とはしていない。
調査環境では次の失敗が残り、変更前の版でも再現した。

| 条件 | 残った失敗 | 記録 |
| --- | --- | --- |
| GTK3・Wayland終了処理 | `test-ime-clause`、`test-gtk3-opengl`でCairo hash tableのassertion | `baseline-wayland-test.log`、`baseline-wayland-opengl-test.log` |
| Wineのnative blink | `blink_renderer_fixture.c:557`のピクセル判定 | `baseline-win32-blink.log` |
| Wineのsample起動前 | テストドライバーの`SetThreadDpiAwarenessContext()`がNULLを返し、boxrig起動へ到達しない | `baseline-win32-dpi-failure.log`、`current-win32-dpi-failure.log` |

これらのログは`/tmp/boxrig-a11y-work/`にある。
対象の上流ソースは[boxrigの検証対象コミット][boxrig-revision]で追跡できる。
ここで記録した試験失敗を、今回のGTK3・X11の性能問題の原因とは扱わない。

第3段階の完了条件は、同じ出力・端末寸法・ライブラリで比較すること、処理区間を分けること、候補を単独と組み合わせで試すこと、最終状態の到達を確認することだった。
800更新の逐次比較、18試行の連続更新、詳細計測を外した対照試験でこれらを満たした。
GTK3・X11・NVIDIAの測定環境で問題箇所を特定する調査として完了している。

製品向け修正の完了条件は、まだ満たしていない。
実際の点滅を保ったときの効果、別GPU／Waylandでの挙動、物理モニターの入力応答は未確認である。
今後の測定でも、録画到着、widget draw終了、GTK合成完了を混同しない。

## 今後の改善候補と完了条件

以下は調査結果に基づく実装候補であり、本書の保存作業では着手しない。
各段階で単独実行できる成果物を作り、機能を保った修正と測定をそろえて判断する。

### 候補1 ステータスバーの描画ウィンドウ共有

`status_bar`の`visible-window=False`を製品構成で検証する。
変更前に追加合成の負荷を再現し、変更後は背景、枠、文字、インジケーター、ポインター操作、サイズ変更を確認する。
試験では実際の表示と操作を観測し、従来の機能が保たれることを検証する。

完了条件は、OpenGLとCairoで表示・操作の回帰がなく、同じ再生条件でGTK合成時間の減少を確認できることとする。
単独変更の効果を記録してコミットする。

### 候補2 インジケーターの不要な画像更新を抑制

同じ画像を再設定する通知を抑え、実際に表示が変わる場合に更新する。
ON／OFFの状態だけでなく、画像の差し替え、初期化、解放、接続表示も扱う。
150 msの点滅と活動通知の蓄積を維持し、単にインジケーターを停止する実験コードは採用しない。

変更前に同じ表示への更新で余分な描画が起きることを再現する。
変更後は点滅の時間的な挙動を動画で確認し、同じ状態への通知、点滅の停止、テーマ変更を検証する。
完了条件は、必要な表示が保たれ、余分なGDK描画が減り、候補1との組み合わせでも効果が再現することとする。
残った点滅自体の負荷が大きい場合は、測定結果に戻って描画方法を検討する。

### 候補3 修正後の全体比較と残余負荷の判断

機能を保った版で、40更新の逐次比較と60更新の連続比較を再実行する。
順序、CPU周波数、行桁数、フォント、ライブラリを記録し、軽い計測でも傾向が一致するか確認する。
実際のbash操作とbtopスクロールでも確認する。

完了条件は、最終状態の到達、描画時間と表示状態数の改善、全体テストの成功、既知の環境依存失敗との区別がそろうことである。
`quietbar`の70.4 msは診断上の参考値とし、機能を止めずに同じ値へ必ず到達するという約束にはしない。
本文公開ONでの機能回帰も確認する。

その結果でまだ負荷が残る場合に、本文リンク認識の式・走査量・続行回数を独立に比較する。
Unicode空白の扱いやリンクの意味を変えずに改善できるかを調べる。
boxrigの追加変更やレンダラー全体の改修は、その測定で必要性が判明してから検討する。

## 記録ファイルと再測定への引き継ぎ

測定スクリプトと詳細データは以下に保存してある。
`/tmp`は永続保管場所ではないため、主要な条件と結果は本書へ転記した。
動画、全フレームの生データ、一時バイナリはリポジトリへ追加していない。

| 場所 | 内容 |
| --- | --- |
| `/tmp/elder-render-profile/` | 初期の大量出力、btop／GNOME Terminal比較、frame削減、フォント・RGBA・Cairoの探索記録 |
| 同上の`btop-host-baseline.output` | 各段階で共通使用したbtop出力。SHA-256は本文に記載 |
| 同上の`final-validation.json`、`final-performance.log` | 初期改善の試験集計と大量出力の測定 |
| `/tmp/boxrig-a11y-work/measurement-summary.json` | 読み上げ用本文公開の旧版／ON／OFFの6反復比較 |
| 同上の`validation-summary.json` | elder-termsとboxrigの試験集計、既知の失敗のログ参照 |
| `/tmp/elder-render-bottleneck/summary.json` | 5条件×4回、800更新の区間計測と各試行の集計 |
| 同上の`stream-summary.json` | 連続更新12試行と詳細計測なしの対照6試行 |
| 同上の`findings.json` | 検証件数、改善率、問題箇所、上流ソースの参照 |
| 同上の`raw-host-*.json`、`.writes`、`.mkv` | 各試行のログ、書き込み時刻、録画 |
| 同上の`matrix.mjs`、`replay.mjs`、`summarize.mjs` | 逐次比較の実行と集計 |
| 同上の`stream-matrix.mjs`、`stream-replay.mjs`、`btop-replay-peer.mjs` | 連続更新の実行と再生peer |
| 同上の`build.mjs`、`build-elder.mjs`、`stages.c`、`gdk-probe.cpp`、`gl-hooks.cpp`、`counter.cpp` | 診断用バイナリと関数計測の構築 |

再測定では、既存スクリプトが固定のcheckout・ビルドパスと一時objectを参照する点を確認する。
リビジョンや通常ビルドが変わった場合は、対応するライブラリと診断用バイナリをそろえる。
実行した基本形は以下である。

```sh
node /tmp/elder-render-bottleneck/build.mjs
node /tmp/elder-render-bottleneck/build-elder.mjs
node /tmp/elder-render-bottleneck/matrix.mjs
node /tmp/elder-render-bottleneck/summarize.mjs
node /tmp/elder-render-bottleneck/stream-matrix.mjs
```

`matrix.mjs`と`stream-matrix.mjs`は、子プロセスに`PROBE_CPU=2`、`PROBE_STAGES=1`を指定する。
詳細計測なしの対照試験は`stream-replay.mjs`を`PROBE_STAGES=0`で別途実行した。
`stream-matrix.mjs`を再実行すると既存の`stream-summary.json`を上書きするため、対照試験を含む過去の集計は先に別名で保管する。
画面を使う測定では、専用ウィンドウとプロセスを管理し、終了後にフォーカスを戻す。

[boxrig-revision]: https://github.com/kekyo/boxrig/tree/1d379548fd0de1ff7b15725cad90f4e1136d732b
[boxrig-widget]: https://github.com/kekyo/boxrig/blob/1d379548fd0de1ff7b15725cad90f4e1136d732b/src/gtk3/terminal_widget.c
[boxrig-accessible]: https://github.com/kekyo/boxrig/blob/1d379548fd0de1ff7b15725cad90f4e1136d732b/src/gtk/accessible_text.c
[boxrig-snapshot]: https://github.com/kekyo/boxrig/blob/1d379548fd0de1ff7b15725cad90f4e1136d732b/src/ui/accessible.c
[boxrig-surface]: https://github.com/kekyo/boxrig/blob/1d379548fd0de1ff7b15725cad90f4e1136d732b/src/gtk/gl_surface.c
[boxrig-sample-links]: https://github.com/kekyo/boxrig/blob/1d379548fd0de1ff7b15725cad90f4e1136d732b/examples/boxrig_term/common/text_policy.c
[gtk-mark-paint]: https://github.com/GNOME/gtk/blob/3.24.41/gdk/gdkwindow.c#L3385
[gtk-end-paint]: https://github.com/GNOME/gtk/blob/3.24.41/gdk/gdkwindow.c#L3050
[gtk-x11-texture]: https://github.com/GNOME/gtk/blob/3.24.41/gdk/x11/gdkglcontext-x11.c#L426
[gtk-from-gl]: https://docs.gtk.org/gdk3/func.cairo_draw_from_gl.html
