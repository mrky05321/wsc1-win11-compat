# WSC1 Windows 11 Compatibility

PC版初代『鋼鉄の咆哮』向けの非公式互換DLLです。描画変換には未改変のdxwrapper v1.8.8600.25を使用し、ゲーム内の小窓の位置とCD音声再生を補正します。

配布パッケージ **v1.0.3**。小窓・BGM補正、ウィンドウモードでのタイトルバードラッグとAlt+F4、兵装クリックの補正に加え、上の戦闘領域で専用の照準カーソルが表示されない問題を修正します。専用DLLはログファイルを作成せず、描画backendも `DisableLogging=1` を配布時の初期設定にしています。

## 導入

1. [配布ZIP v1.0.3](https://github.com/mrky05321/wsc1-win11-compat/releases/download/v1.0.3/wsc1-win11-compat-v1.0.3.zip) をダウンロードして展開します。SHA-256はGitHub Releaseの説明で確認できます。
2. `ddraw.dll`、`dxwrapper.dll`、`dxwrapper.ini` の**3ファイル**を `BCS.exe` と同じフォルダーへコピーします。
3. いつもどおり `BCS.exe` を起動します。初期設定は枠付きウィンドウです。

ZIP内の4つ目のファイル `README.txt` には導入手順とライセンスをまとめています。ゲームフォルダーへコピーする必要はありません。既存の同名DLLや設定を使っている場合は、先にそれらを別の場所へ退避してください。

ゲーム本体・セーブ・BGM・CDイメージは含みません。通常のゲーム起動条件は引き続き必要です。ゲームのEXE/ICDやセーブをディスク上で書き換えません。

## BGM

- ゲームのBGMはCD音声です。フォルダーに対応音源がなければ、CDドライブの再生経路を使います。ドライブ未指定の場合はボリューム名 `Kurogane` のドライブを選びます。イメージファイルの固定パスは参照しません。
- 所有ディスクから準備した `music/cd-audio.wav` と `music/cd-audio.ini` があれば、その音源を通常の音声出力で再生します。これらの音源は配布ZIPに含めません。
- 確認済み環境ではファイル音源によるBGMの出音と曲切替を確認しました。**CD直接再生の出音は未確認**のため、音源ファイルなしでも必ずBGMが鳴るという保証ではありません。
- 音源準備用の開発ツールは `tools/extract_cd_audio.py` です。Python 3.8以上で `python tools/extract_cd_audio.py <所有ディスクのcue> <ゲームフォルダー>/music` と実行します。対応形式はKuroganeの単一IMG・16トラックのCUEです。

## 表示モード

ゲームを終了して `dxwrapper.ini` の `[d3d9]` を編集し、再起動します。

| 表示 | EnableWindowMode | WindowModeBorder | FullscreenWindowMode |
|---|---:|---:|---:|
| 枠付きウィンドウ（初期設定・環境により点滅あり） | 1 | 1 | 0 |
| 枠なし通常ウィンドウ（環境により点滅あり） | 1 | 0 | 0 |
| ボーダレス全画面 | 0 | 0 | 1 |
| ゲーム標準 | 0 | 0 | 0 |

通常のウィンドウモードでは、タイトルバーをドラッグして移動でき、Alt+F4でゲームを終了できます。自動保存は行わないため、必要なデータはゲーム内で保存してから終了してください。小窓がモーダル表示されている間は、先に小窓を閉じてください。起動中のAlt+Enter切替には対応していません。


### ウィンドウモード時に周囲が点滅する現象

一部の環境や実行時の状態によって、画面に変化があった際に、ゲーム外のデスクトップや他のウィンドウ、特にタスクバーのアイコンが点滅する場合があります。枠付き・枠なしの両方で報告されていますが、毎回発生するわけではなく、発生しなくなる場合もあります。原因と発生条件は未特定で、v1.0.3でも点滅そのものは未修正です。

## 確認範囲・制限

Windows 11上の対象PC版で、タイトル・準備画面、艦船選択／兵装設定の本体内表示、「戻る」の操作、ファイル音源のBGMを確認済みです。全ミッション・全機種での動作を保証するものではありません。

v1.0.1では、タイトル・準備画面のドラッグ移動、小窓の表示と「戻る」、オープニング・準備画面のAlt+F4終了を確認しました。移動・終了処理の回帰試験23項目はv1.0.1時点の結果です。

v1.0.2で兵装クリックを補正しましたが、専用カーソル画像の表示補正が不足していました。v1.0.3では描画用の座標取得も補正しました。複製環境のA-01戦闘で、上の戦闘領域に緑色の照準画像と「CHARGE」が表示され、下の兵装欄では通常カーソルへ戻ることを確認しています。ウィンドウ移動後にも専用画像と通常カーソルの切替、兵装欄のクリック、射撃と弾数減少、戦闘中のAlt+F4終了を確認しました。複数兵装を搭載した艦での兵装間切替、全ミッション、拡大表示や複数モニターの全構成は未確認です。

ゲーム側が全画面表示を前提にデスクトップ座標を使うため、クリック判定がずれ、専用カーソル画像も描画範囲の外に出ていました。識別済みビルドの戦闘入力とカーソル描画の呼び出し各1か所を起動中のメモリ上で差し替え、実際のクライアント原点を引きます。EXE/ICDファイルは書き換えず、通常ウィンドウモードでのみ有効です。対象の命令列が一致しない場合は差し替えません。

専用の小窓／音声補正は `BCS.ICD` のPEタイムスタンプ `0x3855e0a2`、SizeOfImage `0x3a9000` が一致するときに有効になります。別作品や別ビルドへの互換性は未確認です。

専用DLLはログファイルを作成しません。以前の `wsc1-compat.ini` に `[Diagnostics] Enabled=1` が残っていても、専用ログの書き込みは行いません。描画backendも、同梱の `dxwrapper.ini` の `DisableLogging=1` でログを開く前に無効化しています。

小窓や音源の切替設定が必要な場合だけ、任意の `wsc1-compat.ini` を追加できます。通常導入では不要です。

```ini
[Compatibility]
Backend=dxwrapper
FixPopupCoordinates=1
DigitalCDAudio=1
FixWindowMove=1
FixAltF4=1
FixBattleMouse=1
```

小窓補正を止める場合は `FixPopupCoordinates=0`、音源ファイルへの切替を止める場合は `DigitalCDAudio=0` にします。タイトルバー移動／Alt+F4の補正を個別に止める場合は `FixWindowMove=0`／`FixAltF4=0` にします。

戦闘中のマウス座標補正だけを止める場合は `FixBattleMouse=0` にします。通常は追加設定なしで有効です。

Program Files内へコピーする場合はWindowsの管理者許可が必要になることがあります。ログ作成のために管理者としてゲームを起動する必要はありません。

既存利用者はセーブ・Gnt.ini・musicフォルダーを残し、今回の更新は `ddraw.dll` を交換し、現在の `dxwrapper.ini` の `[General]` に `DisableLogging=1` を設定してください。表示モードを含む他の設定を維持する場合、配布用INIで丸ごと上書きしないでください。削除して戻す場合も、追加した互換ファイルだけを取り除きます。

## ソースからビルド

Windows、Visual Studio 2022 C++ Build Tools、Windows SDK、CMake、Python 3.8以上が必要です。ゲームは32bitのため **Win32/x86** でビルドします。

```powershell
# 専用DLLのみ（backendのダウンロードは不要）
.\build.ps1 -BuildOnly

# 上流の正規dxwrapper.zipを入手して配布用ZIPを生成
.\build.ps1 -BackendArchive 'C:\Downloads\dxwrapper.zip'
```

backendは[dxwrapper公式](https://github.com/elishacloud/dxwrapper)のv1.8.8600.25です。受け付けるarchiveのSHA-256は `3894368ebce69f348d08e189c2f0818c49aa3b18ad01e06348743bf6a29af97a`。異なるファイルは使用しません。`.cache/dxwrapper.zip` に配置して引数を省略することもできます。

`package.py` はファイル名を列挙して梱包し、ソース以外のゲームファイル・音源・セーブ・ローカルログを拾いません。生成物は `artifacts/` に出力します。ビルドやパッケージ作成によってゲームを起動したり、テストを実行したりしません。

ウィンドウ処理の回帰試験を実行する場合は、`cmake -S . -B build -A Win32 -DWSC1_BUILD_WINDOW_TESTS=ON`、`cmake --build build --config Release --target wsc1_window_tests`、`.\build\Release\wsc1_window_tests.exe` の順に実行します。ゲーム本体を使わず、非表示の検証用ウィンドウで確認します。

## GitHubへの配置

ソースコードはこのリポジトリで管理し、コンパイル済みの配布ZIPはGitHub Releasesに添付します。ゲームや解析データを含む親フォルダーはアップロードしません。`artifacts/`、`downloads/`、ビルド生成物やbackend DLLはGit管理対象外です。

## ライセンス

専用ソースは [MIT License](LICENSE)。backendは別ライセンスです。[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)に上流の通知全文を保持しています。配布ZIPではこれらをREADME.txtに統合しています。

同梱dxwrapperは未改変です。その対応するソースは[上流リポジトリ](https://github.com/elishacloud/dxwrapper)のv1.8.8600.25、MemoryModule部分は同リポジトリ内の対応ソースです。上流ソースと各コンポーネントにはそれぞれのライセンスが適用されます。本プロジェクトはゲーム発売元と無関係の非公式ツールです。
