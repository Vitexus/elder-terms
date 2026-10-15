# boxrig移行に必要なAPIと担当範囲

## 判断の前提

elder-termsを最初からboxrigで実装した構造へ改めるための調査資料である。端末の解析、キー生成、画面上の認識、フォント、配置はboxrigへ集約する。elder-termsには接続管理、通信方式、操作設定、ログ、マクロの実行、外部コマンドの起動を残す。VTEのsignalや内部都合を模倣する互換層は作らない。

再調査日は2026年10月15日。elder-termsの実装基準は`9e71944da6d9a5f4f6b5f0a2d02ce7ccda7e17f8`のまま、`deps/boxrig`を取得時の`origin/develop` HEADである`4b664b87cf405dc97477a1403d8a987881a3134f`へ更新した。直前の固定版は`19be168c464d0509c3388bdc7bb3bef4c2717d3d`、その前は`8ab9bdfe2c44c76b81d8d74899484350734fc86a`、初回は`5ebe6420f6f81b4bf38d11fe233285f470a83ae6`である。今回の差分はUnicode一般カテゴリを指定する正規表現、公式文書、公開APIコメント、試験の追加である。関数や公開型の追加はなく、従来のregex APIから使える。全差分と以前の8項目への影響を確認した。

本書のcoreは端末解析と画面状態、UI modelは表示用モデル、GTK3 widgetはGTKへ配置する端末部品を指す。sessionは端末と通信路を結ぶ有限バッファ付きの処理単位、host itemは端末からアプリへの通知である。

regexは正規表現を指す。コンパイル済みの式をprogram、一つの入力を検索する状態をmatcherと呼ぶ。captureは括弧で抽出する部分文字列、recognizerは画面の論理行を認識する仕組みである。revisionは結果や配置の世代を区別する番号、snapshotは特定時点の状態を保持するコピーを指す。layout actionはアプリが反映する配置要求、DIPはDPIに依存しない論理的な寸法である。

「対応済み」は固定版の公開契約で実装できる意味であり、elder-termsへの結合完了を意味しない。boxrigへの変更は上流で公開されたコミットを取り込み、submoduleへ直接パッチしない。実装順序と完了条件は[移行計画](plan2-boxrig-migration.md)に記す。

## 前回の不足項目の再評価

「必須」は現在の利用者向け機能を保つために解決が必要な項目である。VTEの具体的な実装やPCRE2の全構文を維持する意味ではない。

| ID | 現行の機能 | develop固定版での再評価 | 残る担当・判断 |
| --- | --- | --- | --- |
| R1 | マクロ、OSC 8 URI、設定画面でのregex | 対応済み。独立した`boxrig/regex.h`でコンパイルと継続照合が可能 | elder-termsが同じprogramを設定・URI・受信行へ使う |
| R2 | 番号・名前付きcaptureと参照名の検証 | 対応済み。`boxrig_regex_program_find_named_group()`で照合前に名前の存在と番号を取得できる | elder-termsが保存時検証を接続し、結果はboxrigのcaptureを直接利用する |
| I1 | 切断中・転送中・モーダル中の入力禁止 | 対応済み。`set_input_enabled()`と`get_input_state()`で停止段階と世代を管理 | アプリの送信runner、通信キュー、接続世代はelder-termsが停止・排出する |
| K1 | 接続中のReturn・Backspace・TRS-80設定変更 | 対応済み。`boxrig_input_config`、GTK3の保留入力を考慮したsetter、TTY消去文字の取得・更新 | elder-termsは設定変換とローカルTTY情報の更新を行う |
| W1 | `CSI 8 ; rows ; columns t`によるサイズ要求 | 対応済み。`BOXRIG_HOST_ITEM_GRID_RESIZE`で各軸を型付き通知 | elder-termsが許可・上限を判断し、標準layoutへ渡す |
| P1 | ローカルコマンドの起動結果と失敗理由 | 診断不足は解消。起動診断FDとstage・errnoを取得できる。親側の準備は同期 | elder-termsがCardioへ接続し、取消・子回収・終了結果を管理する |
| B1 | Linux共有ビルドとC++利用 | 対応済み。`shared-gtk3`／`install-shared-gtk3`と公開ヘッダーのC linkage | elder-termsのビルドから上流Makeを呼び、共有5本を同梱する |
| A1 | 本文アクセシビリティと変更通知 | GTK3のAtkTextに対応。上流でVTE比較とAT-SPI・Orcaの観測あり | elder-termsへの結合確認を行う。表示範囲・読取専用・保持上限の契約に従う |

前回解消したR2を含め、本計画で必須としたAPIの不足はない。今回、共通regex試験が使う`\p{L}`にも対応したため、その式を変更せず移行できる。共有ビルドから設定検証、本体切り替えまで、追加APIの公開を待たずに実装を進められる。以下に記すregexの文法差とアプリ側の担当範囲は残る。PCRE2の全構文互換を要件には加えない。

## 正規表現の移行

### 現在の使用範囲

| 使用箇所 | 照合する対象と結果の用途 |
| --- | --- |
| [main-window.cpp](../../../elder-terms-vte/src/main-window.cpp) | VTEのmatch登録とポインター位置の候補検出。PCRE2のオプション定数も直接使用する |
| [terminal-hyperlink-resolver.cpp](../../../elder-terms-vte/src/terminal-hyperlink-resolver.cpp) | OSC 8 URIまたは画面で選んだ候補を照合し、ルール順・引数・検証対象パスを決める |
| [terminal-macro-runner.cpp](../../../elder-terms-vte/src/terminal-macro-runner.cpp) | 文字コード変換後の受信列を行単位で蓄積し、LF到着前にも照合する。最初に一致した一つのルールを行ごとに一度実行する |
| [regular-expression.cpp](../../../shared/src/settings/regular-expression.cpp) | PCRE2のコンパイル、文字列検索、番号と名前のキャプチャ、メタデータを共通提供する |
| [hyperlink-settings.cpp](../../../shared/src/settings/hyperlink-settings.cpp)、[macro-settings.cpp](../../../shared/src/settings/macro-settings.cpp)、[設定UI](../../../shared/src/settings-widget/settings-widget.cpp) | 構文、キャプチャ参照、画面用ルールの空一致を検証する。端末ウィンドウがないランチャーからも利用する |
| [regex-capture-template.cpp](../../../shared/src/settings/regex-capture-template.cpp) | `${0}`、番号・名前付き参照、`$$`、リンク用の`uri-decode`を展開する。テンプレートの文法とコマンド用途はelder-termsの責任である |

端末側のregexだけを差し替えても、`shared`のPCRE2依存は消えない。新しい公開regex APIを`shared`から使い、GUIを生成せずにランチャーと端末が同じ文法・診断・キャプチャ情報を扱う。

### 固定版で維持できること、できないこと

boxrigは独立した照合と画面認識に同じregexエンジンを使う。独立照合は渡したUTF-8文字列全体、画面認識は自動折り返しを結合した論理行を対象とする。最左の開始位置から最長の一致を選び、一致全体の両端は書記素境界になる。画面の複数一致は非重複である。[regexガイド][boxrig-regex-guide]、[公開regex API][boxrig-regex-api]、[画面認識の仕様][boxrig-text]を根拠とする。

| 現在使えるもの・実例 | 固定版での状態 | 移行時の判断 |
| --- | --- | --- |
| 既定の画面HTTPリンクと`${0}` | 対応。Unicodeの空白も明示したクラスで区切れる | recognizerが保持したregex resultを使い、既定HTTPルールの区切りを維持する |
| HTTPを含むOSC 8 URIの照合 | 対応。URIを独立matcherへ渡せる | R1の公開APIを使う |
| 既定`vscode://file...`の`(?<path>...)`、`(?<line>...)`、`(?<column>...)` | 対応。名前付き構文と結果取得がある | 同じprogramを使い、アプリでテンプレートを展開する |
| READMEの`^ERROR: (.+)$`と`${1}`、CHALLENGEの`${token}` | 対応。番号・名前の両方を取得できる | 受信行へ照合し、結果をマクロへ渡す |
| マクロ試験の`\x1b\[31mERROR ...` | `\x1b`は文書化された構文に含まれず、実装も拒否する | 同じ制御文字を`\u{1b}`で記述する。制御列込みのsubjectはR1へ渡す |
| 空文字にも一致する式 | 独立regexも認識器も拒否。現行はマクロ・OSC 8では許容する | 全用途で空一致を禁止する。無条件動作を必要とする機能は別途判断する |
| `(?:...)` | 非キャプチャグループとして対応済み | 現在の公開文法を使う |
| 先読み・後読み、後方参照、`(?i)`、遅延量指定 | 限定文法では非対応 | PCRE2の全互換は求めない。必要な実利用例がある場合だけ追加を検討する |
| `\d`・`\s`・`\w`のUnicode範囲 | 現行はPCRE2_UCP。boxrigの略記は引き続きASCII限定 | 一般カテゴリを明示したクラスで必要な分類を指定する。既定HTTPルールの変更案は後述する。任意設定の自動変換はしない |
| `\p{L}`によるUnicode文字種の指定 | 対応済み。[共通regex試験](../../../shared/tests/settings-test.cpp)の日本語hostの式をそのまま使える | 名前付きcaptureとUTF-8バイト位置の検証を維持し、否定クラスによる代替は行わない |
| `\p{Greek}`等の文字体系、`\p{White_Space}`等の一般カテゴリ以外の属性 | 非対応。今回の追加は一般カテゴリの略称に限る | 必要な文字種は対応済みのカテゴリとリテラルで記述する。新たな実利用要件がある場合だけ上流追加を検討する |
| `.`と`^`／`$` | 独立regexの`.`はLFにも一致し、anchorは入力全体を指す。検索開始offsetを変えてもanchorの基準は変わらない | 現行PCRE2はDOTALLを付けていない。改行を除外する式では`[^\n]`を使い、全入力と論理行の違いを案内する |
| 短い候補と長い候補が同じ位置で一致する選択式。例は`a`または`ab`を選ぶ式に対するsubject `ab` | 現行の通常PCRE2照合は先の選択肢を優先し、boxrigは同じ開始位置で最長を選ぶ | boxrigの最左最長を採用し、ルール順の優先とは分けて説明・試験する |

ルールの優先順はelder-termsが維持し、一つのルール内での一致選択はboxrigへ従う。

調査した組み込みルール・README・テストには、番号・名前付きキャプチャとESCを含む照合の実例がある。一方、先読み・後読み・後方参照の維持を必須にする具体例は確認していない。任意の利用者設定すべてを調べたわけではないため、その利用が存在しないとは断定しない。

### Unicode一般カテゴリと既定リンク

一般カテゴリは、Unicodeの文字を文字・結合符号・数・記号などに分類する仕組みである。`\p{カテゴリ}`はその分類に属するUnicodeスカラー1個、`\P{カテゴリ}`は属さない1個に一致する。`L`・`M`・`N`・`P`・`S`・`Z`・`C`、`LC`、`Lu`や`Nd`などの2文字の細分類を指定できる。波括弧は必須で、大文字・小文字を区別する。`Letter`のような長い名前や`gc=L`形式は受理しない。[公式ガイド][boxrig-regex-guide]と[APIコメント][boxrig-regex-api]に従う。

単独でも文字クラス`[]`内でも使え、クラス内はリテラル・範囲・カテゴリの和集合になる。`[\P{L}A]`は文字以外と`A`、`[^\P{L}]`は文字を表す。カテゴリを範囲の端点にはできない。不正な名前や波括弧は`INVALID_ESCAPE`となり、診断位置はバックスラッシュのバイト位置を指す。範囲の端点に使った場合は`INVALID_RANGE`になる。

分類は同梱のutf8procに従う。アプリがUnicode表を持ったり、PCRE2へ照合を委譲したりする必要はない。追加の外部依存と公開symbolも不要で、既存の共有5本の構成を維持できる。分類データの版や一致規則までPCRE2と同一になる意味ではない。

現行の共通regex試験は次の式を使い、入力`Open https://example.みんな now`のバイト位置`[5, 30)`を照合している。

```regex
(?<scheme>https?)://(?<host>[\p{L}0-9.-]+)
```

更新版の共有ライブラリでこの式をそのままコンパイルし、照合前の`host`の番号2、`scheme`の`https`、`host`の`example.みんな`を確認した。captureの位置は一致本文からの相対位置で、`scheme`が`[0, 5)`、`host`が`[8, 25)`になる。前回検討した「区切り文字以外を取り込む」代替表現へ変更する必要はなく、絵文字や全角スペースをhostへ取り込む意味の拡大も避けられる。この式自体はホスト名の候補抽出であり、ドメイン名全体の妥当性検証ではない。

一般カテゴリ対応後も、一致全体は書記素境界に限られる。`e`と結合アクセントからなる文字列へ`\p{L}+`だけを照合しても、文字の途中で終われないため一致しない。結合符号も必要な用途では`[\p{L}\p{M}]+`を使う。内部captureの端点はUnicodeスカラー境界なので、`(?<letter>\p{L})(?<mark>\p{M})`なら両者を分けて取得できる。アプリが暗黙に`\p{M}`を補うことはせず、[上流のcapture試験][boxrig-captures-test]と同じ規則を案内する。

[既定HTTPルール](../../../shared/src/settings/hyperlink-settings.cpp)の`\s`は、現行の`PCRE2_UCP`ではUnicodeの空白も区切る。boxrigの`\s`だけへ移すと全角スペースなどをURLに含めるため、移行時は次のように区切りを明示する。上が画面用、下がOSC 8用である。

```regex
https?://[^\s\p{Z}\u{85}\u{180e}<>"']+
^https?://[^\s\p{Z}\u{85}\u{180e}<>"']+$
```

`Z`はUnicodeの空白・行・段落の区切り、`\u{85}`はNEL、`\u{180e}`はMongolian vowel separatorを指す。ASCIIの空白・タブ・改行などは`\s`が扱う。[PCRE2の文字種仕様][pcre2-pattern]にあるUCPの区切りを明示した案であり、elder-termsの現行エンジンと更新版boxrigで26種類の区切りを比較した。画面用は区切り直前までを取得し、OSC 8用は途中に区切りがあるURIを拒否する結果が一致した。これは製品が用意する既定2ルールの変更であり、利用者設定を変換する互換層は作らない。書記素境界と最左最長などの意味差は引き続きboxrigへ従う。

### R1 任意UTF-8文字列に対する公開regex API

`boxrig_regex_compile()`で不変のprogramを作り、`boxrig_regex_matcher_create()`へ長さ付きUTF-8入力と検索開始位置を渡す。NULやESCを含む入力も扱える。コンパイル診断は種類とパターン内のバイト位置を持つ。`boxrig_regex_matcher_resume()`は正の仕事量を受け、継続中・一致・不一致を返す。[公開ヘッダー][boxrig-regex-api]に所有権と上限が定義されている。

matcherはprogramを参照保持し、入力をコピーする。生成時のUTF-8検証とコピーは入力長に比例する同期処理であり、resumeの仕事量制限に含まれない。アプリは入力長を制限し、resumeの間でGTKへ制御を返す。共有programの参照操作と利用は同じイベントスレッドへ集約するか、明示的に直列化する。

既定上限はパターン64 KiB、状態数32768、有限反復4096、入力1 MiB、グループ32、キャプチャ用記憶16 MiBである。受信マクロの現行1 MiB上限に合わせられる。全体一致のoffsetは元入力の絶対バイト位置だが、captureは後述の結果本文に対する相対位置になる。

受け入れ試験は、GUIなしの設定検証・URI照合、不正UTF-8、NUL、容量超過、仕事量1での分割再開、開始位置、結果の寿命を含める。`GRegex`や`std::regex`へ振り替えず、公開APIだけで実装できる。

### R2 キャプチャと照合前の名前検証

`boxrig_regex_matcher_result()`は照合をやり直さず、不変の結果を取得する。`result_data()`の一致本文と、`result_get_capture()`／`result_get_named_capture()`の範囲を組み合わせる。番号0は全体一致、範囲は結果本文のUTF-8バイト位置である。未参加と空文字は`participated`で区別できる。全体一致と異なり、内部captureの端点は書記素内のUnicodeスカラー境界にもなる。[regex API][boxrig-regex-api]に従う。

同じprogramを`boxrig_terminal_register_compiled_text_recognizer()`へ登録できる。画面では認識器ID・match ID・結果集合のrevisionを`boxrig_terminal_text_recognizer_result()`へ渡す。古い結果は`STALE`になり、取得済みの結果は再走査・解除・端末破棄後も有効である。候補文字列の再照合は不要になった。[core API][boxrig-core-regex]が根拠である。

名前は一意なASCII識別子`[A-Za-z_][A-Za-z_0-9]*`に限られる。同じ全体範囲のcaptureは左の選択肢と貪欲な反復を優先し、反復内のgroupは最後に参加した値を保持する。結果集合の本文保持上限は既定16 MiBで、切り替え時は現在と次の集合を持つ。アプリが取得して保持した結果は別枠なので、操作後に`boxrig_regex_result_destroy()`で解放する。

照合前の名前照会には、`19be168`で取り込んだ`boxrig_regex_program_find_named_group()`を使う。不変program、NUL終端の名前、存在有無と番号の出力先を渡す。[設定保存前の公式手順][boxrig-regex-guide]と[公開APIコメント][boxrig-regex-api]で、次の契約を確認できる。

| 入力と状態 | 戻り値と出力 |
| --- | --- |
| 定義済みの名前 | `OK`、`found=true`、1以上のgroup番号 |
| 未定義、大小文字が異なる名前、空文字、名前の文法に合わない文字列 | `OK`、`found=false`、番号0 |
| program・name・出力先のいずれかがNULL | `INVALID_ARGUMENT`。NULLでない出力先はfalse／0へ戻る |
| optional groupまたは`{0}`で実行されないgroupの名前 | 定義済みとして取得できる。一致への参加とは別の情報 |

照会はメモリ確保、照合、追加コンパイル、端末生成を必要とせず、programと参照数を変更しない。同じprogramの利用は引き続き直列化する。共有ライブラリの公開symbolにも追加されている。全名前の列挙APIはないが、現在の製品機能には不要である。

現行の[参照検証](../../../shared/src/settings/regex-capture-template.cpp)は、正常にコンパイルできたprogramへ接続する。`${0}`と番号参照を桁あふれ検査付きで解析し、0から`boxrig_regex_program_group_count()`までを受理する構成に改める。名前参照は新APIの`found`で判定する。`OK`だけで存在すると判断せず、`${missing}`を保存時に拒否する。不正引数は呼び出し側のエラーとして区別する。NULを含む長さ付きの参照名は、C文字列へ渡す前に拒否し、途中で切れた別の名前として照会しない。

[現在の共通regex型](../../../shared/include/elder-terms/settings/regular-expression.h)が持つ名前一覧mapとcapture文字列配列は、boxrigの不変resultの所有へ改める。テンプレートの展開時には番号・名前のgetterを直接使い、名前表を再構築しない。未参加の定義済みgroupは現在と同じく空文字へ展開し、未知の参照はエラーにする。これにより、画面で取得したcaptureも同じ経路で利用できる。

受け入れ試験では、`(?<path>a)?b`をコンパイルした直後、照合せずに`path`の存在と番号、`missing`の不在を取得する。大小文字、無効な名前、NULL、番号0・最大番号・範囲外、重複名、未参加group、元パターンの解放後の照会、C99/C++20の共有リンクを含める。設定画面で`${path}`を受理し`${missing}`を拒否する結合試験は移行実装時に行う。API不足の判定は「対応済み」へ変更する。

### マクロを画面認識へ置き換えない理由

[現行マクロの試験](../../../elder-terms-vte/tests/cpp/terminal-macro-runner-test.cpp)は、LF前の実行、分割受信、制御列の保持、CRLFのCR除外、1 MiBの行上限を確認している。画面ではANSI列が消え、CRによる上書きも起こる。認識器の結果更新は画面の現在値であり、途中に出て消えた入力をすべて通知する契約ではない。literal monitorもregex monitorの代わりにはならない。[監視と認識の契約][boxrig-text]に基づく差分である。

推奨は、elder-termsが接続からのUTF-8受信行と実行済み状態を管理し、照合だけをR1・R2へ渡す構成である。上限、CRLF、LF前のprompt判定、ルール順、接続世代をアプリの仕様として持つ。画面用の擬似端末へ受信列を投入したり、PCRE2をマクロ専用に残したりしない。

boxrigに将来regex付きstream monitorを追加する選択肢はある。その場合も「表示内容の出現」と「受信列の到着」を区別し、通知欠落、再走査、実行済み判定の契約が必要になる。今回必要な汎用照合とキャプチャは公開済みなので、stream monitorを追加待ちの条件にしない。

### PCRE2廃止の範囲

elder-termsが所有するコードの`pcre2_*`呼び出し、ヘッダー、compile flag、Meson依存、pkg-config検査、`libpcre2-dev`の明示導入、利用者向けPCRE2文法案内を取り除く。リンク・マクロ・設定検証はboxrigへ統一する。

GTKの基盤であるGLibにも[正規表現機能](https://docs.gtk.org/glib/struct.Regex.html)がある。調査環境の`/usr/lib/x86_64-linux-gnu/libglib-2.0.so.0`を`readelf -d`で調べると、`libpcre2-8.so.0`が`DT_NEEDED`に含まれていた。GTK3を維持したまま、全推移依存や実行プロセスからPCRE2を必ず消すことは別の課題になる。

完了判定では、elder-termsと同梱boxrigの直接依存・regex実装からの撤去を確認し、システムGTK/GLibから来る間接依存を区別する。`ldd`全体にPCRE2が一度でも現れたら移行失敗とする条件は設けない。GLibのregexをアプリが利用する代替案も採用しない。

## 入力と端末制御

### I1 UI入力を禁止し、履歴とコピーを残す

現行は[main-window.cpp](../../../elder-terms-vte/src/main-window.cpp)で`vte_terminal_set_input_enabled()`を切り替える。boxrigの[GTK3公開API][boxrig-gtk]へ`boxrig_gtk3_terminal_widget_set_input_enabled()`と`get_input_state()`が追加された。可逆な入力禁止にはこれらを使う。不可逆なquiesceは終了処理に限る。

禁止はキー、IME確定、貼り付け、端末向けマウス入力などを止める。選択、コピー、履歴移動、描画、受信解析、端末応答とアプリのrouterは継続する。古い世代のIME・clipboard応答は再開後も破棄され、押下中だったキーはreleaseまで抑止される。[GTKガイド][boxrig-gtk-guide]と[入力状態の型][boxrig-ui-input]を参照する。

戻り値が`BUSY`のときは状態も確認する。`STOPPING`なら停止要求は受理済みで、開始済みbracketed pasteの終端を通常のframe処理でキューへ入れる。入力callback内で呼んだ場合の`BUSY`は状態が変わらず、callbackを抜けて再試行する。`STOPPING`中は再開できない。

`DISABLED`は必要なpaste終端をcoreへ投入した境界であり、通信先への排出完了ではない。すでにあるcore出力・端末応答や、直接coreへ送るアプリ処理までは止めない。elder-termsはテキスト送信runnerとclipboard取得を自分の接続世代で取り消し、core・通信キューを排出してからXYZMODEMへ切り替える。切断時には旧接続の未送信データを破棄し、再接続へ持ち越さない。

転送中は通常sessionのwrite callbackを`WOULD_BLOCK`で保留し、遅れて生成された端末応答も転送データに混ぜない。XYZMODEMはアプリの通信管理から専用経路で送信する。転送完了の通知で通常送信を再開し、途中で接続世代が変わった場合は旧出力を破棄する。frameを繰り返して送信可能になるのを待つ実装にはしない。

受け入れ試験では、遅延IME・clipboard・key repeatを保留したまま禁止と再開を行い、再接続先へ古い入力が送られないことを確認する。同時に選択コピーが動き、遅い端末応答が転送データへ混入せず、同じ接続で転送が完了すれば通常送信を再開できることを検証する。routerでキーだけを握りつぶす案やwidget全体をinsensitiveにする案では、この条件を満たせない。

### K1 端末の入力policyを実行中に変更する

現行[TerminalTextSettings](../../../shared/include/elder-terms/settings.h)には文字コード、Backspace、Return、TRS-80カーソルキー設定があり、接続中にも変更できる。独立した受信改行設定は存在しない。前版計画に記した「既存の受信改行変換を維持」は訂正し、その機能を新規追加する根拠にはしない。

生成設定は`config.input`の`boxrig_input_config`へ整理された。`enter_newline`、BackspaceとDeleteのbinding、矢印の`key_mappings`をまとめて交換できる。GTK3からは`boxrig_gtk3_terminal_widget_set_input_config()`を使う。押下中のキーやIME・clipboard・pasteが残ると`BUSY`になり、旧設定を保つ。直接core setterでこの保護を迂回しない。[送受信ガイド][boxrig-io]と[公開config][boxrig-core]が根拠である。

交換は検証とコピーを伴い、失敗時も画面・履歴・選択・端末モードを維持する。設定はRIS／DECSTRによるリセット後も残る。既にキューへ入ったバイトは書き換えないため、アプリは設定変更の完了と受理待ちを区別する。

Autoは通常BackspaceがDEL、Deleteが`ESC[3~`になる。現行の[ローカル端末試験](../../../elder-terms-vte/tests/local-session.test.ts)はAutoでBSを期待しており、この差を操作説明と試験へ反映する。BS／DELの明示指定、delete sequence、TTYから得た消去文字を選ぶbindingもある。Kittyが優先し、1バイトのBS／DELはCtrlで交換、AltでESCを前置する。CSIのDeleteには修飾値が符号化される。旧VTEのAuto出力を再現する補正は作らない。

TTY bindingを使う場合は、ローカルPTYのキー処理直前に`boxrig_posix_get_tty_erase()`を呼び、その成功・失敗のどちらも`boxrig_terminal_set_tty_erase()`へ反映する。これで子の`stty erase`変更に追従できる。NULと無効化値も取得済みの1バイトとして扱い、未取得時だけAutoへ戻る。接続変更時に古い値を消す。現在のAuto設定はboxrig Autoへ対応させ、TTY追従を設定として提供するかは別の製品判断にする。[POSIX API][boxrig-posix]がこの境界を定める。

TRS-80は4方向のkey mappingとして設定する。既存の出力文字列を再走査する`TerminalCursorKeyMapper`と、Kitty状態を受信列から再解析する`TerminalKeyboardProtocolState`を廃止する。Returnのcommit補正やidleで消す保留キー状態も不要になる。

受け入れ試験は、通常・修飾キー・keypad・Kitty push/pop・代替画面、設定変更途中の押下／release、明示BS／DEL、CR／LF／CRLF、TRS-80を送信先のバイトで確認する。型や関数の名称が置き換わったことだけでは判定しない。

### W1 端末からのサイズ変更要求

現行[resize-windowハンドラー](../../../elder-terms-vte/src/terminal-layout.cpp)が扱う要求に対し、coreへ`BOXRIG_HOST_ITEM_GRID_RESIZE`が追加された。`item.grid_resize`は行と列を別々に保持し、正値は`EXPLICIT`、省略は`KEEP`、0は`DISPLAY`を表す。`14t`・`16t`のpixel問い合わせとは別である。[hostガイド][boxrig-host-guide]と[公開型][boxrig-host-items]を根拠とする。

明示値は`1..UINT32_MAX`なので、アプリが許可と資源上限を検証する。`KEEP`は処理時の現在値、`DISPLAY`は使用可能な表示領域と確定セル寸法から解決する。通常ウィンドウでは採用した希望gridを`set_preferred_grid()`へ渡し、最大化・タイル中は現行と同様に要求を適用せず消費する。coreは受信だけでリサイズしない。

通知のrequest IDは0で、採用・拒否のどちらもhost itemのconsumeだけが必要である。端末への応答やhost requestの完了APIは呼ばない。採用した後の配置は通常のlayout revisionで別途完了する。キュー満杯の`WOULD_BLOCK`には未消費部分だけを再投入し、アプリ側でCSIを解析しない。

受け入れ試験は、実PTYからの要求、分割列、0値、拒否、最大化中、フォント変更との競合、遅いSSH resize中の次要求を含める。gridが一度確定した後に旧タイマーが元へ戻す挙動がないことも確認する。

## elder-terms側で実装する機能

### 任意の文字コード、ログ、送信処理

boxrig内蔵encodingはUTF-8とWHATWG Shift_JISである。elder-termsは[設定検証](../../../shared/src/settings/terminal-settings.cpp)でiconvによる任意の双方向変換を受け付け、CP932、EUC-JP、GB18030なども候補に持つ。CP932を内蔵Shift_JISへ単純に同一視すると変換結果が変わり得る。[上流のencoding仕様][boxrig-io]にこの制約がある。

この文字コード対応は、端末表示から独立した接続製品の機能として残す。推奨は、coreのencodingをUTF-8として、通信側に独立したiconv変換を置くこと。rawログは変換前、cookedログと受信マクロは変換後の一つの列から通知する。再試行時に変換・ログ・マクロを二重実行しない。

文字変換にはキー判定・カーソル列書換え・Return補正を含めない。受信は端末の通常データだけを変換し、XYZMODEMを分岐した後に処理する。送信は端末出力とアプリのテキスト送信に対して順序を管理し、バイナリ転送を別経路にする。途中の不完全文字と設定交換の境界をテストする。追加codecをboxrigへ寄せるなら汎用codec拡張が候補だが、全iconv encodingの内蔵化は必須としない。

### 貼り付けと速度を指定したテキスト送信

boxrigの`queue_paste()`は入力を正規化し、coreのpaste transactionを進める。行間待機、速度、進捗、送信キャンセルはelder-termsの[送信runner](../../../elder-terms-vte/src/terminal-text-send-runner.cpp)の機能であり、直接対応する一つのAPIはない。

現在の「貼り付けも設定済みのテキスト送信として扱う」機能はアプリに残す。key・mouse routerで選んだ操作とメニューを一つの送信サービスへ渡し、CLIPBOARD／PRIMARY取得の世代を管理する。boxrigのネイティブ選択とCOPY actionはそのまま使う。[公開router契約][boxrig-ui-input]と[GTKサンプルの操作接続][boxrig-native-host]が根拠であり、サンプルのpaste shortcutをwidgetに固定された機能と取り違えない。

通常の標準pasteを採用する場合は、boxrigのbracketed pasteを含む規則に従う。一方、現在のテキスト送信はそれを付加しない。今回の推奨は後者の製品機能を明示的に残すことで、両経路を同時に起動しない。全pasteを標準transactionへ集約する変更を望む場合は、速度・キャンセルと開始／終端の契約を別途boxrigへ追加する。

### 非同期通信とレイアウト完了

boxrigの[session API][boxrig-session]は非ブロッキングread／writeと有限バッファを提供する。Cardioの受信・送信キューをcallbackへ接続すれば、ログやXYZMODEMを迂回せず使える。前版計画の「汎用sessionは使わない」という一律の判断は撤回する。

ただしsessionのresize callbackは同期であり、現行[SSH resize](../../../elder-terms-vte/src/terminal-sessions/ssh-session/ssh-session.cpp)は非同期である。アプリのlayout coordinatorが通信先のresize完了を待ち、その後coreをresizeし、同じrevisionをwidgetへ完了通知する構成にする。非同期系のsessionにはresize callbackを登録せず、この順序で一度だけ反映する。ローカルPTYには同期callbackを使える。これは公開APIで実装でき、新しい端末パーサーや寸法補正を必要としない。

SSHは要求の送信処理完了、TelnetはNAWS送信の完了または未交渉時の保存、シリアルは外部resize不要を、それぞれの成功境界とする。相手が確認応答を持たないプロトコルで、応答待ちを作らない。現行のvoid通知や警告だけで済ませる失敗処理は改める。

タイトル合成、BELの音声ファイルと既定beep、リンクコマンド・パス検証、認証、再接続、BREAK、XYZMODEM、FTP・SFTP・WebDAVもアプリの責任である。boxrigにこれらの製品機能を一括追加する要求は出さない。

VTEのリンク許可setterとmatch cursor名setterにも同名のAPIはないが、製品の有効・無効はリンクactionとhover表示の許可として実装できる。画面候補とOSC 8のhit、annotation、GTKのポインター表示を組み合わせる。DeleteのAuto、カーソル点滅、選択の有無とテキストコピーはboxrigの標準core／UIに従い、旧VTEの出力や見た目の再現を目的に補正しない。TITLE host itemの対象・push／pop flagsはアプリのタイトル管理へ接続する。[GTK3公開API][boxrig-gtk]と[ホスト連携ガイド](https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/docs/ja/host-integration.md)を参照する。

## 起動・共有ビルド・アクセシビリティ

### P1 POSIX PTYの起動結果

[POSIX API][boxrig-posix]へ`spawn_with_diagnostic()`、`spawn_descriptor()`、`get_spawn_result()`が追加された。親側の失敗はstage・errno付きで戻り、成功時はPTYと`PENDING`を返す。診断FDの読み取り可能または切断をCardioで待ち、非ブロッキングな取得操作で`SUCCEEDED`／`FAILED`を確認する。不存在・実行不可を、正常起動後の終了127と区別できる。[sessionガイド][boxrig-session-guide]にも起動・終了・取消の区別がある。

親の引数・環境・PTY準備とforkは同期処理であり、完全な非同期spawn APIが加わったわけではない。GTKを止めないため、上流の[C++20ドライバー][boxrig-spawn-example]と同じくCardioの`promises::start_new()`でこの同期部分を隔離し、独自の待機threadやpollingを作らない。これは公開APIを組み合わせるアプリ側の責任であり、新しいboxrig APIを必須にはしない。親の環境変数変更とspawnは直列化する。

診断FDはPTYの所有であり、直接read・close・設定変更を行わない。確定後に監視を外す。診断取得は子を回収せず、PTY破棄も子を回収しないため、子監視と回収はelder-termsの一つの経路へ集約する。`wait()`をGTKスレッドで呼ばない。起動取消は要求と後始末完了を分け、遅いready通知を接続世代で除外する。

既定シェル、終了要求、接続表示はアプリが担当する。受け入れ試験は不存在・実行不可・通常終了127・即時終了・起動中の取消・終了との競合・FDと子プロセスの回収を含める。診断成功はエラー通知なしで診断チャネルが閉じたことを示し、コマンドの準備完了や継続稼働の保証にはしない。

### B1 共有ビルドとC++公開ヘッダー

[ビルドガイド][boxrig-building]と[共有ビルド定義][boxrig-shared-make]に`shared-gtk3`／`install-shared-gtk3`がある。`BUILD_DIR`を外部の絶対パスにし、`GTK_OPENGL=1`を渡す。`PREFIX`などのインストール先は空白を含まない絶対パスにする。これでcore、UI、session、POSIX、GTK3の5本をPICで生成し、SONAMEはそれぞれ`.so.0`になる。公開ヘッダーのC linkageも整備され、C++20から通常includeできる。

上流は共有ライブラリ自身の`$ORIGIN`、移動可能なpkg-config情報、明示した公開symbolとライブラリ間専用symbol、private codecの非公開化を提供する。`BOXRIG_PRIVATE_0`は内部連携用であり、アプリから使わない。生成ツールはNode.js・Vite・funcityを使い、依存導入は`npm ci --prefix deps/boxrig/tools/codegen`で行う。コンパイル成果物と生成ツール出力は`BUILD_DIR`配下になる。

初回の独自ソース列挙、単一`libelder-terms-boxrig.so`、C linkage用bridge案は不要になった。elder-termsの通常ビルドから上流Makeを起動し、`libboxrig.so.0`、`libboxrig-ui.so.0`、`libboxrig-session.so.0`、`libboxrig-posix.so.0`、`libboxrig-gtk3.so.0`を私有libdirへ同梱する。sharedのregexはcoreへ、端末は必要な各ライブラリへリンクする。生成元のstatic archiveをアプリへ重ねてリンクしない。

codegenの開発依存とMakeの呼び出しはアプリのビルドから用意し、別途システムへboxrigを手動インストールする前提にしない。残る検証は親Mesonとの増分依存、配布時のRUNPATH、`dpkg-shlibdeps`、各対象環境のロードである。外部ソースを修正する必要はない。

ビルド時の残る制約として、共通Makeは対象外のGTK4やnative Waylandなども読込時にpkg-configで照会する。必要な開発パッケージだけを入れた環境では、不要な不足診断が出る可能性がある。[公式ガイド][boxrig-building]にも記載がある。選んだtargetの依存だけを照会する改善は上流候補だが、共有化やC ABIの不足とは分けて扱う。今回のホストでは診断が出ておらず、最小依存のコンテナーでの確認は段階1に残る。

### A1 アクセシビリティ

GTK3 widgetはAtkTextを実装し、表示中の本文、caret、選択、文字属性、座標と変更通知を公開する。上流はVTE比較、AT-SPI経由の取得、Orcaが生成した読み上げ文字列を検証している。実音声再生までの確認ではない。[GTKガイド][boxrig-gtk-guide]と[上流の比較記録][boxrig-feedback-results]を参照する。

本文は表示範囲だけで、履歴全体ではない。自動折り返しにはLFを挿入せず、実改行・空行・明示的な空白を保持する。位置はUnicodeスカラー単位で、IME未確定文字は含めない。snapshotは本文と位置情報を合わせて16 MiBまでとし、上限超過時は直前の本文を保持してframeがエラーを返す。アプリはその診断を扱う。

アクセシビリティ経由のcaret移動・選択変更は受理しない読み取り専用の契約である。reflowによる履歴位置もVTEと同一とは限らない。これらを補正する実装は加えず、boxrigが表示した本文と通知の一致を確認する。elder-termsのfocus、overlay、再接続、ウィンドウ破棄を含むAT-SPIと読み上げの結合試験は残る。端末用accessible実装をアプリに重複させる必要はない。

### IMEの追加対応

developにはIBusに加え、直接fcitx 5／4へ接続する経路と診断がある。GTK3は既定の`BOXRIG_GTK_IME_AUTO`を使い、明示されたGTKモジュールやboxrigの選択規則に従う。入力方式をIBusへ固定せず、`get_ime_state()`で実際の経路と理由を観測する。[IMEガイド][boxrig-ime-guide]と[GTK入力方式の公開型][boxrig-input-method]を根拠とする。

`19be168`で取り込んだ[fcitx試験の変更][boxrig-fcitx-test]は、windowのmap後にnative focusイベントを処理してから試験のfocusを設定する修正である。IMEの公開APIや製品側の実装変更ではない。elder-termsへ待機タイマーやfocus補正を持ち込む根拠にはしない。今回の差分にはIMEの変更はない。

直接fcitx接続はGLib／GIOを使い、fcitx開発パッケージは不要である。実行時のデーモン・エンジンと、GTK委譲時のIMEモジュールは別に必要になる。これは今回の不足APIではないが、入力禁止後の遅延確定、候補表示、DPI変更を含めて結合検証へ加える。

## 追加APIが不要なサイズ・フォント機能

通常のウィンドウサイズ変更、文字サイズ変更、希望行列数、実寸取得、標準スクロールバーには専用の公開APIがある。`set_preferred_grid()`、`take_layout_action()`、`complete_size_command()`、`complete_grid_resize()`、`set_window_state()`、`set_geometry_hints()`を一つの配置経路にする。[GTK3の契約][boxrig-gtk-layout]と[GTKのhost実装例][boxrig-native-host]を参照する。

フォントは`set_font_specification()`、ズーム操作は`visual_zoom()`を使う。visual zoomはraw font specificationを変更せず、進行中のpresentationと次の希望値をboxrigが管理する。保存する文字サイズは、確定した`get_effective_metrics()`の`requested_size_dip`から取得して次回のfont specificationへ渡す。VTEの倍率0.5〜3.0・0.1刻みは維持要件としない。[フォントAPI][boxrig-font]と[配置ガイド][boxrig-font-guide]に従う。

必要なhost処理は、boxrigから要求された内容寸法をGTKの論理座標へ換算し、兄弟widgetの実寸を加えてトップレベルへ伝え、子の割当後にrevisionを完了する部分である。CSDを二重加算したり、希望値に一致するまで`gdk_window_resize()`を繰り返したりしない。削除する既存関数・状態・タイマーは[移行計画](plan2-boxrig-migration.md)で特定する。

描画方針は`BOXRIG_GTK_RENDER_OPENGL_PREFERRED`とする。OpenGLを組み込んで優先使用し、利用できない場合はboxrigのCairo fallbackを許容する。[描画policyの定義][boxrig-rendering]がこの動作を明記している。通常のサイズ・フォント操作は両backendで同じAPIを使う。

## この調査の完了条件と結果

今回の完了条件は、develop HEADへgitlinkを更新し、前回からの全差分と8項目への影響を確認することである。Unicode一般カテゴリの公式文書と公開コメントを確認し、元の共通regex試験と既定リンクへ適用できるかを判断する。対応済み・残る機能差・アプリの担当を計画へ反映する。更新コミットは`3453b3f`。外部の追跡対象ソースは変更していない。

以前の`8ab9bdf`では、次の検証に成功している。今回の差分で、regex以外の公開契約とビルド構成が変わっていないことも確認した。

| 実行した確認 | 結果と範囲 |
| --- | --- |
| `shared-gtk3`と`install-shared-gtk3`、`GTK_OPENGL=1`、外部`BUILD_DIR` | 共有5本の生成・インストール成功。同じ設定での再ビルドは再コンパイル・再リンクなし |
| SDKを別ディレクトリへ移動し、公開ヘッダーをC++20から利用 | `LD_LIBRARY_PATH`なしでリンク・実行成功。SONAMEと`$ORIGIN`も確認 |
| 上流のGTK3共有ライブラリ利用ドライバー、Xvfb・Mesa | PreferredでOpenGLを選択。GL初期化失敗を注入した場合はCairoへ移り、描画完了まで成功 |
| `test-regex`、既定リンク4件、READMEのマクロ2件、ESC付きマクロ1件 | 成功。実アプリのパターンを使い、仕事量1で再開して照合。ESCは`\u{1b}`へ書き換えて検証 |
| `test-input-config`、`test-tty-erase`、`test-input-control`、`test-grid-resize` | 設定交換、TTY変更、入力世代とpaste停止、型付きサイズ要求の試験に成功 |
| `test-posix-spawn` | 起動失敗と終了127、非ブロッキング診断、親準備・子setupの失敗、資源回収の試験に成功 |
| `test-accessible-snapshot`、`test-gtk3-accessible` | 本文、変更通知のsnapshot、破棄を含む試験に成功 |

直前の`19be168`では、名前照会APIと共有成果物を次の範囲で確認した。この版では`\p{L}`はまだ非対応だった。

| 実行した確認 | 結果と範囲 |
| --- | --- |
| 新しい外部`BUILD_DIR`で`shared-gtk3`と`install-shared-gtk3`、`GTK_OPENGL=1` | 共有5本の生成・インストール成功。ビルド警告なし |
| 移動後のSDKからC99・C++20の上流consumer、GTK3公開ヘッダーを利用 | 名前の存在・不在を照合前に取得し、`LD_LIBRARY_PATH`なしで実行成功。新symbolの`BOXRIG_0`公開も確認 |
| `test-regex`と、共有ライブラリへリンクした[capture試験][boxrig-captures-test] | 名前・番号、大小文字、無効名、NULL、optional・反復0回、最大group数、既存capture取得に成功 |
| 現行ソースから取得した既定リンク4ルール | GUI・matcherなしでgroup数、`path`・`line`・`column`の番号、未定義名の不在をC++20の共有consumerから確認 |
| 共通regex試験の`\p{L}`を含む式 | この版のCLIが構文エラーとして拒否することを確認。現在の固定版では後述のとおり対応済み |
| GTK3共有ドライバー、Xvfb・Mesa | PreferredでOpenGL描画に成功。GL初期化失敗を注入した場合はCairoへ移り、描画完了を確認 |
| 変更されたGTK3 fcitx試験 | 専用D-Busの試験サーバーでpreedit・commit・fallback・focusの試験に成功。実fcitx daemonの結合試験ではない |

今回の`4b664b8`では、mergeを含む4コミット・7ファイルの差分を調べ、次の検証に成功した。

| 実行した確認 | 結果と範囲 |
| --- | --- |
| 新しい外部`BUILD_DIR`で`shared-gtk3`と`install-shared-gtk3`、`GTK_OPENGL=1` | 共有5本の生成・インストール成功。追加の外部依存・公開関数はなく、ビルド警告もなし |
| SDKを別ディレクトリへ移動し、C99・C++20の上流consumerを実行 | `LD_LIBRARY_PATH`なしで実行成功。名前照会、core・UI・session・POSIXとprivate codecの分離を確認 |
| `test-regex`、共有版にリンクしたregex・capture試験 | 一般カテゴリ・否定・和集合・診断・線形の処理量・仕事量制限に成功。日本語と結合符号を含むcaptureは独立matcherと通常／代替画面のrecognizerで一致 |
| 現行共通regex試験から取得した元の式と入力 | C++20の共有consumerで事前の名前照会、全体`[5, 30)`、`scheme`と`host`の本文・相対位置を確認 |
| 既定HTTPの2ルールにUnicode空白を明示する案 | 現行elder-termsの共有regexとboxrigを比較し、26種類の空白で画面用の終端とOSC 8用の拒否が一致。日本語・絵文字・結合符号を含むパスの取得も一致 |
| GTK3共有ドライバー、Xvfb・Mesa | PreferredでOpenGL描画に成功。GL初期化失敗を注入した場合はCairoへ移り、描画完了を確認 |

上流の全platform試験、実GPU、elder-termsへ接続したIME・AT-SPI・非同期起動・通信・レイアウト・配布は今回の実測に含まれない。上流の完了記録を、アプリへの結合試験の代用にはしない。アプリ全体テストと文書検査の結果は[計画の確認結果](plan2-boxrig-migration.md#今回の確認結果)に記す。

R1・R2・I1・K1・W1・P1・B1・A1の不足は公開契約上解消しており、今回の更新で`\p{L}`の非対応も解消した。boxrigの文法と責任分担を採用する本計画では、必須の追加API待ちはない。略記のASCII範囲や書記素境界などの意味差、対象外platformの依存照会、実GPUやIMEを含むアプリの結合検証は引き続き残る。通常のサイズ・フォントはboxrigの単一経路、描画はOpenGL Preferred、共有ライブラリは親ビルド中の5本生成という方針を維持する。

[boxrig-text]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/docs/ja/text.md
[boxrig-core]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/include/boxrig/terminal.h
[boxrig-core-regex]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/include/boxrig/terminal.h
[boxrig-gtk]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/include/boxrig/gtk3/terminal_widget.h
[boxrig-gtk-layout]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/include/boxrig/gtk3/terminal_widget.h
[boxrig-io]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/docs/ja/terminal-io.md
[boxrig-ui-input]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/include/boxrig/ui/input.h
[boxrig-host-items]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/include/boxrig/terminal.h
[boxrig-session]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/include/boxrig/session.h
[boxrig-native-host]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/examples/boxrig_term/gtk3/native_host.c
[boxrig-posix]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/include/boxrig/posix.h
[boxrig-building]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/docs/ja/building.md
[boxrig-font]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/include/boxrig/gtk3/terminal_widget.h
[boxrig-font-guide]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/docs/ja/fonts-and-layout.md
[boxrig-rendering]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/include/boxrig/gtk/rendering.h
[boxrig-regex-guide]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/docs/ja/regex.md
[boxrig-regex-api]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/include/boxrig/regex.h
[boxrig-gtk-guide]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/docs/ja/platforms/gtk.md
[boxrig-host-guide]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/docs/ja/host-integration.md
[boxrig-session-guide]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/docs/ja/sessions.md
[boxrig-spawn-example]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/examples/pty_spawn/operation.cpp
[boxrig-shared-make]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/packaging/linux/rules.mk
[boxrig-feedback-results]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/docs/ja/plans/step1-feedback-progress.md#段階11
[boxrig-ime-guide]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/docs/ja/input-ime.md
[boxrig-input-method]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/include/boxrig/gtk/input.h
[boxrig-fcitx-test]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/tests/gtk/test_fcitx_widget.c
[boxrig-captures-test]: https://github.com/kekyo/boxrig/blob/4b664b87cf405dc97477a1403d8a987881a3134f/tests/test_regex_captures.c
[pcre2-pattern]: https://www.pcre.org/current/doc/html/pcre2pattern.html#genericchartypes
