# TVTDataBroadcastingWV2

ダウンロード https://github.com/otya128/TVTDataBroadcastingWV2/releases

[web-bml](https://github.com/otya128/web-bml)とWebView2を使ったTVTest用データ放送プラグイン

## 動作環境

* TVTest 0.9.0以上
* Windows 10以上
    * Windows 7用の対応は入れていないのでWindows 7では映像と正常に合成できないはず 8.xなら動くかも
* WebView2 ランタイム
    * 最低限89.0.774.44以上である必要がある
    * もしインストールされていなければ https://go.microsoft.com/fwlink/p/?LinkId=2124703 からインストール
* Visual C++ 2015-2022ランタイム
    * 万が一入っていなければTVTestのアーキテクチャに合わせて https://aka.ms/vs/17/release/vc_redist.x64.exe (x64) https://aka.ms/vs/17/release/vc_redist.x86.exe (x86) からインストール

映像レンダラはEVR, EVR (Custom Presenter), madVR, システムデフォルト, VMR9, VMR9 Renderlessで動作します。 ただし現時点ではVMR9 Renderlessを使うとフルスクリーンでの表示などに支障があります。

字幕やコメントを直接映像に合成するプラグインとは相性が悪いため、同時に正常に表示したい場合にはレイヤードウィンドウを使うように設定するかあきらめるなどしてください。
映像レンダラにVMR9 Renderlessを選択した場合映像に直接合成してもレイヤードウィンドウを使うようにしても字幕やコメントがデータ放送中の映像に合わせて縮小されます。

## 操作

TVTest起動時には有効にならないようになっているため右クリックメニューからプラグインを有効にするか、設定でサイドバーにプラグイン有効アイコンを表示させてそこから有効にしてください。
有効にしたタイミングでWebView2が起動します。

プラグイン有効時に表示されるリモコンかTVTest側の設定でキーなどをデータ放送の操作に割り当てて操作することが出来ます。

## 設定

### プラグイン有効時にリモコンを表示しない

### TVTest起動時にプラグインを有効にする

