# boxrig移行に必要なAPIと担当範囲

## 判断の前提

elder-termsを最初からboxrigで実装した構造へ改めるための調査資料である。端末の解析、キー生成、画面上の認識、フォント、配置はboxrigへ集約する。elder-termsには接続管理、通信方式、操作設定、ログ、マクロの実行、外部コマンドの起動を残す。VTEのsignalや内部都合を模倣する互換層は作らない。

調査日は2026年10月14日。elder-termsの実装基準は`9e71944da6d9a5f4f6b5f0a2d02ce7ccda7e17f8`、boxrigは`deps/boxrig`に固定した`5ebe6420f6f81b4bf38d11fe233285f470a83ae6`である。公式文書と公開ヘッダーのAPIコメントを照合し、公開機能で判断できない箇所だけ実装を確認した。結合動作や性能を実測した結果ではない。

本書で「追加案」と呼ぶものは、固定版には存在しない。関数名を確定済みのAPIとして扱わず、必要な契約を示す。boxrigへの変更は上流で実装・公開されたコミットを取り込み、elder-terms内のsubmoduleへ直接パッチしない。実装順序と完了条件は[移行計画](plans/plan2-boxrig-migration.md)に記す。

## 直接対応がない項目と推奨判断

「必須」は現在の利用者向け機能を保つために解決が必要な項目である。VTEの具体的な実装やPCRE2の全構文を維持する意味ではない。

| ID | 現行の機能 | 固定版boxrigの不足・差分 | 推奨する担当と優先度 |
| --- | --- | --- | --- |
| R1 | マクロ、OSC 8 URI、設定画面での正規表現のコンパイル・照合 | 端末と独立した公開regex APIがない。画面上の認識器だけでは受信文字列やURIを直接照合できない | boxrig core。PCRE2撤去に必須 |
| R2 | `${1}`、`${name}`などのキャプチャ展開と参照名の検証 | 認識結果は一致全体の範囲だけ。名前付きグループは構文としても非対応 | boxrigの共通regexエンジン。既定VS Codeリンクとマクロに必須 |
| I1 | 切断中・転送中・モーダル表示中の入力禁止 | 選択・コピーを残して入力だけを止める公開設定がない | boxrig UI。保留IME・pasteまで扱う契約が必須 |
| K1 | 接続中のReturn・Backspace・TRS-80キー設定変更 | Returnと矢印mappingは生成時のみ。Backspace用policyがない | boxrig core。明示設定の維持とキー補正撤去に必須 |
| W1 | 端末内プログラムからの行列数変更要求 | `CSI 8 ; rows ; columns t`を通知するhost itemがない | boxrig coreで解析・通知、elder-termsで許可とウィンドウ操作。現行の要求受付を維持するために必要 |
| P1 | ローカルコマンドの非同期起動結果と失敗理由 | `spawn()`は同期API。子の`execvp()`失敗を親へ通知する専用経路がない | 起動診断はboxrig POSIXへの追加を推奨。子監視・接続表示はelder-terms |
| B1 | Linux共有ライブラリとしての組み込みとC++利用 | 標準Linux成果物は静的archive。UI・session・POSIXヘッダーのC linkageが一律でない | boxrigに共有ビルドとヘッダー整備を推奨。現行公開APIの範囲でelder-terms側のMake・C bridgeでも実現可能 |
| A1 | VTEが提供する可能性のある読み上げ・本文アクセシビリティ | boxrigはterminal roleのみでAtkText未対応 | boxrig GTK3で実装する候補。現行製品で得られる本文・選択・変更通知の範囲を比較して判断 |

R1・R2・I1・K1・W1をアプリ側の独自エンジンや制御列解析で埋めない。これらが未解決の間は独立したドライバーで移行を進め、本体の切り替えを完了扱いにしない。P1・A1は異なる種類の不足であり、後述の判断条件を設ける。

## 正規表現の移行

### 現在の使用範囲

| 使用箇所 | 照合する対象と結果の用途 |
| --- | --- |
| [main-window.cpp](../../elder-terms-vte/src/main-window.cpp) | VTEのmatch登録とポインター位置の候補検出。PCRE2のオプション定数も直接使用する |
| [terminal-hyperlink-resolver.cpp](../../elder-terms-vte/src/terminal-hyperlink-resolver.cpp) | OSC 8 URIまたは画面で選んだ候補を照合し、ルール順・引数・検証対象パスを決める |
| [terminal-macro-runner.cpp](../../elder-terms-vte/src/terminal-macro-runner.cpp) | 文字コード変換後の受信列を行単位で蓄積し、LF到着前にも照合する。最初に一致した一つのルールを行ごとに一度実行する |
| [regular-expression.cpp](../../shared/src/settings/regular-expression.cpp) | PCRE2のコンパイル、文字列検索、番号と名前のキャプチャ、メタデータを共通提供する |
| [hyperlink-settings.cpp](../../shared/src/settings/hyperlink-settings.cpp)、[macro-settings.cpp](../../shared/src/settings/macro-settings.cpp)、[設定UI](../../shared/src/settings-widget/settings-widget.cpp) | 構文、キャプチャ参照、画面用ルールの空一致を検証する。端末ウィンドウがないランチャーからも利用する |
| [regex-capture-template.cpp](../../shared/src/settings/regex-capture-template.cpp) | `${0}`、番号・名前付き参照、`$$`、リンク用の`uri-decode`を展開する。テンプレートの文法とコマンド用途はelder-termsの責任である |

端末側のregexだけを差し替えても、`shared`のPCRE2依存は消えない。新しいboxrig regex APIはGUIを生成せずに使える必要がある。ランチャーと端末が同じ文法・診断・キャプチャ情報を使う。

### 固定版で維持できること、できないこと

boxrigの認識器はUTF-8リテラル、文字クラス、グループ、選択、反復、行頭・行末、限定したescapeを受理する。照合は論理行ごとで、soft wrapを結合し、最左の開始位置から最長の一致を選ぶ。結果は非重複で、書記素の境界を守る。これは[公式の文字処理仕様][boxrig-text]と[公開APIの契約][boxrig-core-regex]に明記されている。

| 現在使えるもの・実例 | 固定版での状態 | 移行時の判断 |
| --- | --- | --- |
| 既定の画面HTTPリンクと`${0}` | パターンの基本構文と一致全体の範囲は対応。現在のUnicode文字クラスとの意味差はある | recognizerとsnapshotの範囲抽出で実装する |
| HTTPを含むOSC 8 URIの照合 | URIのhit取得は対応するが、その任意文字列をregexへ渡す公開入口がない | R1で実現する |
| 既定`vscode://file...`ルールの`(?<path>...)`、`(?<line>...)`、`(?<column>...)` | 名前付き構文とキャプチャ結果がない | R1・R2が必要。VS Code専用パーサーの追加では一般ルールの機能を維持できない |
| READMEの`^ERROR: (.+)$`と`${1}`、CHALLENGEの`${token}` | 括弧はグループ化できても、その内容を取り出せない | R2で番号・名前の両方を扱う |
| マクロ試験の`\x1b\[31mERROR ...` | `\x1b`は文書化された構文に含まれず、実装も拒否する | 同じ制御文字を`\u{1b}`で記述する。制御列込みのsubjectはR1へ渡す |
| 空文字にも一致する式 | boxrig認識器は拒否。現行は画面リンクだけ禁止し、マクロ・OSC 8では許容する | 新しいルールは全用途で空一致を禁止する案。無条件動作が必要ならregex外の明示操作として別途判断する |
| 先読み・後読み、後方参照、`(?i)`、`(?:...)`など | 固定版の限定文法では非対応 | PCRE2の全互換は求めない。必要な実利用例がある場合だけboxrigへの追加を検討する |
| `\d`・`\s`・`\w`のUnicode範囲 | 現行はPCRE2_UCP。boxrigはASCIIのクラス | Unicodeリテラルは使用できるが、分類の意味は変わる。Unicode分類が必要なルールは追加要件にする |
| 短い候補と長い候補が同じ位置で一致する選択式。例は`a`または`ab`を選ぶ式に対するsubject `ab` | 現行の通常PCRE2照合は先の選択肢を優先し、boxrigは同じ開始位置で最長を選ぶ | boxrigの最左最長を採用し、ルール順の優先とは分けて説明・試験する |

ルールの優先順はelder-termsが維持し、一つのルール内での一致選択はboxrigへ従う。

調査した組み込みルール・README・テストには、番号・名前付きキャプチャとESCを含む照合の実例がある。一方、先読み・後読み・後方参照の維持を必須にする具体例は確認していない。任意の利用者設定すべてを調べたわけではないため、その利用が存在しないとは断定しない。

### R1 任意UTF-8文字列に対する公開regex API

boxrig内の同じエンジンを、端末・UIなしで使う公開モジュールとして提供する案を推奨する。内部の[src/core/regex.h][boxrig-private-regex]に同種の実装はあるが、アプリから直接include・export・コピーしない。

必要な操作は、長さ付きパターンのコンパイルと破棄、長さ付きUTF-8 subjectの照合、開始offset、コンパイル診断と位置、キャプチャのメタデータ取得である。一致なし、構文エラー、容量超過、継続中を区別する。入力にはNULやESCも含み得るため、終端文字だけに依存するAPIにしない。

処理量・パターン長・NFA状態数・subject長・結果保持量へ上限を設ける。継続処理は仕事量を整数で指定し、GTKのイベントループへ制御を戻せる契約にする。subjectをコピーするか借用するか、継続照合中の寿命を公開コメントで定める。内部実装の型をそのまま公開する判断とは分ける。

受け入れ試験は、GUIなしで既定OSC 8ルールの検証と照合、設定エラーのバイト位置、不正UTF-8、NUL、資源上限、分割再開、開始offset、結果の寿命を検証する。elder-terms側のregex実装、`GRegex`、`std::regex`への振り替えは行わない。

### R2 キャプチャと画面認識器の一貫性

[公開結果型][boxrig-regex-result]には一致全体の`range`しかない。番号付き括弧と`(?<name>...)`のキャプチャ、番号・名前の対応表、未一致の任意グループを区別する結果が必要である。全体一致を番号0とする。

任意文字列にはUTF-8バイト範囲、画面認識には元のtext revisionと対応する範囲を返す。呼び出し側が必要な本文をコピーでき、端末更新後の古い結果は`STALE`として扱える契約にする。キャプチャ内部をどの境界で表すか、反復・選択内のキャプチャをどの規則で確定するかも文書化する。

画面上の候補検出とキャプチャ抽出は同じコンパイル済みパターン・同じsubject文脈で行う。候補文字列だけを切り出して別の規則で再照合すると、anchorや一致選択が変わるためである。公開結果にキャプチャを含めるか、match IDとrevisionによる取得APIを設けるかは上流で選べる。

受け入れ試験は、既定VS Code URI、READMEのローカルパスルール、番号・名前参照、未一致グループ、繰り返し、複数一致、折り返し、結合文字、古いrevisionを含める。資源上限と最左最長を維持できるかは実装前に検証し、単に既存NFAへ結果フィールドを足せば済むとは見積もらない。

### マクロを画面認識へ置き換えない理由

[現行マクロの試験](../../elder-terms-vte/tests/cpp/terminal-macro-runner-test.cpp)は、LF前の実行、分割受信、制御列の保持、CRLFのCR除外、1 MiBの行上限を確認している。画面ではANSI列が消え、CRによる上書きも起こる。認識器の結果更新は画面の現在値であり、途中に出て消えた入力をすべて通知する契約ではない。literal monitorもregex monitorの代わりにはならない。[監視と認識の契約][boxrig-text]に基づく差分である。

推奨は、elder-termsが接続からのUTF-8受信行と実行済み状態を管理し、照合だけをR1・R2へ渡す構成である。上限、CRLF、LF前のprompt判定、ルール順、接続世代をアプリの仕様として持つ。画面用の擬似端末へ受信列を投入したり、PCRE2をマクロ専用に残したりしない。

boxrigに将来regex付きstream monitorを追加する選択肢はある。その場合も「表示内容の出現」と「受信列の到着」を区別し、通知欠落、再走査、実行済み判定の契約が必要になる。今回の必須追加は汎用照合とキャプチャまでとする。

### PCRE2廃止の範囲

elder-termsが所有するコードの`pcre2_*`呼び出し、ヘッダー、compile flag、Meson依存、pkg-config検査、`libpcre2-dev`の明示導入、利用者向けPCRE2文法案内を取り除く。リンク・マクロ・設定検証はboxrigへ統一する。

GTKの基盤であるGLibにも[正規表現機能](https://docs.gtk.org/glib/struct.Regex.html)がある。調査環境の`/usr/lib/x86_64-linux-gnu/libglib-2.0.so.0`を`readelf -d`で調べると、`libpcre2-8.so.0`が`DT_NEEDED`に含まれていた。GTK3を維持したまま、全推移依存や実行プロセスからPCRE2を必ず消すことは別の課題になる。

完了判定では、elder-termsと同梱boxrigの直接依存・regex実装からの撤去を確認し、システムGTK/GLibから来る間接依存を区別する。`ldd`全体にPCRE2が一度でも現れたら移行失敗とする条件は設けない。GLibのregexをアプリが利用する代替案も採用しない。

## 入力と端末制御

### I1 UI入力を禁止し、履歴とコピーを残す

現行は[main-window.cpp](../../elder-terms-vte/src/main-window.cpp)で`vte_terminal_set_input_enabled()`を切り替える。boxrigの[GTK3 API][boxrig-gtk]にはkey・mouse router、paste、composition、quiesceがあるが、同等の可逆な入力policyはない。quiesceは不可逆であり、入力禁止の代わりにはならない。

追加案は、terminalへ利用者入力を渡すかをwidgetが一括管理するpolicyと、その状態・世代の取得である。キー、IME確定、mouse report、focus report、非同期clipboard応答、予約済みpasteを対象にし、切り替え時にどこまで取り消すかを定める。選択、履歴移動、コピー、描画、受信解析、端末応答は継続する。

押下途中のキーを禁止状態へ移した際のrelease処理と、開始済みbracketed pasteの終端処理もboxrigが管理する。送信済みのデータは取り戻せないため、禁止完了の境界を明示する。elder-termsは接続世代ごとの送信待ちデータと転送開始の順序を管理する。

受け入れ試験では、遅延IME・clipboard・key repeatを保留したまま禁止と再開を行い、再接続先へ古い入力が送られないことを確認する。同時に選択コピーが動き、必要な端末応答は失われないことを検証する。routerでキーだけを握りつぶす案やwidget全体をinsensitiveにする案では、この条件を満たせない。

### K1 端末の入力policyを実行中に変更する

現行[TerminalTextSettings](../../shared/include/elder-terms/settings.h)には文字コード、Backspace、Return、TRS-80カーソルキー設定があり、接続中にも変更できる。独立した受信改行設定は存在しない。前版計画に記した「既存の受信改行変換を維持」は訂正し、その機能を新規追加する根拠にはしない。

boxrigは生成時の`enter_newline`と矢印の`key_mappings`を提供する。Kittyとapplication keypadの扱いもcoreにある。一方、これらのruntime setterとBackspace policyはない。[送受信ガイド][boxrig-io]と[公開config][boxrig-core]を根拠とする。

追加案は、既存端末に対して入力policyを検証・コピーして一括交換する公開操作である。少なくともEnter、legacy Backspace、矢印mappingを対象とする。失敗時は前の設定を残し、画面・履歴・選択・端末モードを再生成しない。保留キー・pasteの途中で交換できない場合は`BUSY`などを返し、交換の境界を定める。

Autoはboxrigの規則へ従う。固定版の[Backspace実装][boxrig-input]では通常BackspaceがDEL、Ctrl+BackspaceがBSであり、現行VTEテストのAuto=BSとは異なる。Autoの過去の出力を保つための補正は作らず、BSが必要な接続には明示設定を使う。BS／DELの明示指定は維持し、Kitty報告と修飾キーへの優先順位をboxrig側で決める。

TRS-80は4方向のkey mappingとして設定する。既存の出力文字列を再走査する`TerminalCursorKeyMapper`と、Kitty状態を受信列から再解析する`TerminalKeyboardProtocolState`を廃止する。Returnのcommit補正やidleで消す保留キー状態も不要になる。

受け入れ試験は、通常・修飾キー・keypad・Kitty push/pop・代替画面、設定変更途中の押下／release、明示BS／DEL、CR／LF／CRLF、TRS-80を送信先のバイトで確認する。型や関数の名称が置き換わったことだけでは判定しない。

### W1 端末からのサイズ変更要求

現行[resize-windowハンドラー](../../elder-terms-vte/src/terminal-layout.cpp)は通常ウィンドウで受けた行列数を希望値へ反映する。boxrigは通常のレイアウト変更APIを備えるが、[host item一覧][boxrig-host-items]に端末由来のサイズ要求がなく、[CSI実装][boxrig-interpreter]でも`CSI 8 ; rows ; columns t`を通知しない。`14t`・`16t`のpixel問い合わせとは別の機能である。

追加案は、coreがこの制御列を解析し、型付きの行列数要求をhost queueへ出す機能である。0、省略、範囲外、分割入力、キュー満杯の扱いを定め、OSC・DCS内の文字列では発火させない。host queueの既存の所有権と順序に従い、coreが勝手にウィンドウやPTYをリサイズしない。

elder-termsは通常・最大化・タイルなどの状態から要求を採用するか決め、採用時だけ`set_preferred_grid()`へ渡す。その後は通常のlayout actionで処理する。完了通知を必要とする設計なら、拒否を含む結果を一度返す。既存signalのためだけに、受信列をアプリが再解析するコードは追加しない。

受け入れ試験は、実PTYからの要求、分割列、0値、拒否、最大化中、フォント変更との競合、遅いSSH resize中の次要求を含める。gridが一度確定した後に旧タイマーが元へ戻す挙動がないことも確認する。

## elder-terms側で実装する機能

### 任意の文字コード、ログ、送信処理

boxrig内蔵encodingはUTF-8とWHATWG Shift_JISである。elder-termsは[設定検証](../../shared/src/settings/terminal-settings.cpp)でiconvによる任意の双方向変換を受け付け、CP932、EUC-JP、GB18030なども候補に持つ。CP932を内蔵Shift_JISへ単純に同一視すると変換結果が変わり得る。[上流のencoding仕様][boxrig-io]にこの制約がある。

この文字コード対応は、端末表示から独立した接続製品の機能として残す。推奨は、coreのencodingをUTF-8として、通信側に独立したiconv変換を置くこと。rawログは変換前、cookedログと受信マクロは変換後の一つの列から通知する。再試行時に変換・ログ・マクロを二重実行しない。

文字変換にはキー判定・カーソル列書換え・Return補正を含めない。受信は端末の通常データだけを変換し、XYZMODEMを分岐した後に処理する。送信は端末出力とアプリのテキスト送信に対して順序を管理し、バイナリ転送を別経路にする。途中の不完全文字と設定交換の境界をテストする。追加codecをboxrigへ寄せるなら汎用codec拡張が候補だが、全iconv encodingの内蔵化は必須としない。

### 貼り付けと速度を指定したテキスト送信

boxrigの`queue_paste()`は入力を正規化し、coreのpaste transactionを進める。行間待機、速度、進捗、送信キャンセルはelder-termsの[送信runner](../../elder-terms-vte/src/terminal-text-send-runner.cpp)の機能であり、直接対応する一つのAPIはない。

現在の「貼り付けも設定済みのテキスト送信として扱う」機能はアプリに残す。key・mouse routerで選んだ操作とメニューを一つの送信サービスへ渡し、CLIPBOARD／PRIMARY取得の世代を管理する。boxrigのネイティブ選択とCOPY actionはそのまま使う。[公開router契約][boxrig-ui-input]と[GTKサンプルの操作接続][boxrig-native-host]が根拠であり、サンプルのpaste shortcutをwidgetに固定された機能と取り違えない。

通常の標準pasteを採用する場合は、boxrigのbracketed pasteを含む規則に従う。一方、現在のテキスト送信はそれを付加しない。今回の推奨は後者の製品機能を明示的に残すことで、両経路を同時に起動しない。全pasteを標準transactionへ集約する変更を望む場合は、速度・キャンセルと開始／終端の契約を別途boxrigへ追加する。

### 非同期通信とレイアウト完了

boxrigの[session API][boxrig-session]は非ブロッキングread／writeと有限バッファを提供する。Cardioの受信・送信キューをcallbackへ接続すれば、ログやXYZMODEMを迂回せず使える。前版計画の「汎用sessionは使わない」という一律の判断は撤回する。

ただしsessionのresize callbackは同期であり、現行[SSH resize](../../elder-terms-vte/src/terminal-sessions/ssh-session/ssh-session.cpp)は非同期である。アプリのlayout coordinatorが通信先のresize完了を待ち、その後coreをresizeし、同じrevisionをwidgetへ完了通知する構成にする。非同期系のsessionにはresize callbackを登録せず、この順序で一度だけ反映する。ローカルPTYには同期callbackを使える。これは公開APIで実装でき、新しい端末パーサーや寸法補正を必要としない。

SSHは要求の送信処理完了、TelnetはNAWS送信の完了または未交渉時の保存、シリアルは外部resize不要を、それぞれの成功境界とする。相手が確認応答を持たないプロトコルで、応答待ちを作らない。現行のvoid通知や警告だけで済ませる失敗処理は改める。

タイトル合成、BELの音声ファイルと既定beep、リンクコマンド・パス検証、認証、再接続、BREAK、XYZMODEM、FTP・SFTP・WebDAVもアプリの責任である。boxrigにこれらの製品機能を一括追加する要求は出さない。

VTEのリンク許可setterとmatch cursor名setterにも同名のAPIはないが、製品の有効・無効はリンクactionとhover表示の許可として実装できる。画面候補とOSC 8のhit、annotation、GTKのポインター表示を組み合わせる。DeleteのAuto、カーソル点滅、選択の有無とテキストコピーはboxrigの標準core／UIに従い、旧VTEの出力や見た目の再現を目的に補正しない。TITLE host itemの対象・push／pop flagsはアプリのタイトル管理へ接続する。[GTK3公開API][boxrig-gtk]と[ホスト連携ガイド](https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/docs/ja/host-integration.md)を参照する。

## その他の上流候補

### P1 POSIX PTYの起動結果

[POSIX API][boxrig-posix]はPTY生成、FD・PID取得、resize、wait、destroyを提供する。しかし[実装][boxrig-pty-impl]では`execvp()`失敗を子が127で終了して伝えるため、親のspawn成功と対象コマンドの起動成功を区別しなければならない。正常なプログラムの終了値127と起動失敗も区別できない。

推奨する追加は、exec成功またはerrnoを取得できる起動完了経路である。待機可能FDなどを公開してCardio／GLibへ接続でき、GTKスレッドをブロックせず、子の回収を二重に行わない契約が望ましい。VTEのasync callback形式をコピーする必要はない。

子PIDの監視、既定シェルの選択、終了要求、接続表示はelder-termsが担当する。`wait()`をGTKスレッドで呼ばない。起動失敗の詳細を含む現行機能を維持するなら追加が必要であり、単に「生成後すぐ終了した」と表示する方式を同等として扱わない。不存在・実行不可・通常終了127・起動中の中止・FDリークを受け入れ試験とする。

### B1 共有ビルドとC++公開ヘッダー

[ビルドガイド][boxrig-building]と[Makefile][boxrig-make]はLinuxで静的archiveを生成する構成である。上流への追加案は、core、UI、GTK3、session、POSIXをPICで共有化し、SONAME、公開symbol、private codecのvisibility、外部build directory、pkg-configを整えること。C++側から公開ヘッダーを通常includeしてC ABIへリンクできることも確認する。

上流対応を待たずに進める場合は、elder-terms所有のMakefileが固定した外部ソースをPICでコンパイルし、私有共有ライブラリとして同梱できる。これは外部ソースの変更ではない。公開APIのC linkageをまとめる薄いC99 bridgeも許容するが、VTE型やsignalを再現する層にはしない。

共有ビルドは通常のelder-termsビルドから必ず起動する。別途システムへboxrigを手動インストールする前提にはしない。codecのsymbol衝突、重複object、未解決symbol、C++リンク、増分更新、インストール先での実ロードを検証する。

### A1 アクセシビリティ

[上流の対応範囲][boxrig-a11y]はGTK3のterminal roleのみで、AtkTextによる本文・変更・選択を未対応としている。elder-termsは明示的にAtkTextを呼んでいないが、VTEのwidgetから得られる機能が失われる可能性がある。

現行製品をAT-SPIで観測し、本文、caret、選択、変更通知と読み上げを比較する。必要な対応はboxrig GTK3の公開widgetへ実装するのが適切であり、elder-termsだけに端末用accessible実装を重複させる案は推奨しない。上流未対応のまま同等性を断定せず、実装するか機能差として受け入れるかを本体切り替え前に明示する。

## 追加APIが不要なサイズ・フォント機能

通常のウィンドウサイズ変更、文字サイズ変更、希望行列数、実寸取得、標準スクロールバーには専用の公開APIがある。`set_preferred_grid()`、`take_layout_action()`、`complete_size_command()`、`complete_grid_resize()`、`set_window_state()`、`set_geometry_hints()`を一つの配置経路にする。[GTK3の契約][boxrig-gtk-layout]と[GTKのhost実装例][boxrig-native-host]を参照する。

フォントは`set_font_specification()`、ズーム操作は`visual_zoom()`を使う。visual zoomはraw font specificationを変更せず、進行中のpresentationと次の希望値をboxrigが管理する。保存する文字サイズは、確定した`get_effective_metrics()`の`requested_size_dip`から取得して次回のfont specificationへ渡す。VTEの倍率0.5〜3.0・0.1刻みは維持要件としない。[フォントAPI][boxrig-font]と[配置ガイド][boxrig-font-guide]に従う。

必要なhost処理は、boxrigから要求された内容寸法をGTKの論理座標へ換算し、兄弟widgetの実寸を加えてトップレベルへ伝え、子の割当後にrevisionを完了する部分である。CSDを二重加算したり、希望値に一致するまで`gdk_window_resize()`を繰り返したりしない。削除する既存関数・状態・タイマーは[移行計画](plans/plan2-boxrig-migration.md)で特定する。

描画方針は`BOXRIG_GTK_RENDER_OPENGL_PREFERRED`とする。OpenGLを組み込んで優先使用し、利用できない場合はboxrigのCairo fallbackを許容する。[描画policyの定義][boxrig-rendering]がこの動作を明記している。通常のサイズ・フォント操作は両backendで同じAPIを使う。

## この調査の完了条件と結果

現行の利用箇所と上流の公開契約を対応づけ、直接APIがない機能、担当、追加案、追加しない場合の差分、受け入れ試験を示した。PCRE2の全使用経路とサイズ・フォントの撤去対象を移行計画へ反映した。これにより、上流で先に実装する項目とelder-terms内で進められる項目を判断できる。

R1〜A1の追加実装や採用承認、移行後の結合試験が完了したという意味ではない。現行版で不足を発見した段階として保存し、上流更新時はこの根拠と完了条件を再評価する。

[boxrig-text]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/docs/ja/text.md
[boxrig-core]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/include/boxrig/terminal.h
[boxrig-core-regex]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/include/boxrig/terminal.h#L1743
[boxrig-regex-result]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/include/boxrig/terminal.h#L402
[boxrig-private-regex]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/src/core/regex.h
[boxrig-gtk]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/include/boxrig/gtk3/terminal_widget.h
[boxrig-gtk-layout]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/include/boxrig/gtk3/terminal_widget.h#L191
[boxrig-io]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/docs/ja/terminal-io.md
[boxrig-input]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/src/core/input.c#L677
[boxrig-ui-input]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/include/boxrig/ui/input.h
[boxrig-host-items]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/include/boxrig/terminal.h#L872
[boxrig-interpreter]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/src/core/interpreter.c#L318
[boxrig-session]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/include/boxrig/session.h
[boxrig-native-host]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/examples/boxrig_term/gtk3/native_host.c
[boxrig-posix]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/include/boxrig/posix.h
[boxrig-pty-impl]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/src/posix/pty.c
[boxrig-building]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/docs/ja/building.md
[boxrig-make]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/Makefile
[boxrig-a11y]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/docs/ja/reference/platform-support.md#L145
[boxrig-font]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/include/boxrig/gtk3/terminal_widget.h#L466
[boxrig-font-guide]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/docs/ja/fonts-and-layout.md
[boxrig-rendering]: https://github.com/kekyo/boxrig/blob/5ebe6420f6f81b4bf38d11fe233285f470a83ae6/include/boxrig/gtk/rendering.h
