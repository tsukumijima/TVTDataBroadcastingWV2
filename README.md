# TVTDataBroadcastingWV2

ダウンロード https://github.com/otya128/TVTDataBroadcastingWV2/releases

[web-bml](https://github.com/otya128/web-bml)とWebView2を使ったTVTest用データ放送プラグイン

![動作画面](https://user-images.githubusercontent.com/4075988/162745408-282fb7ab-9826-4e82-b2ab-b1ab347a42b4.png)

## 動作環境

* TVTest 0.9.0以上
* Windows 10以上
    * Windows 7用の対応は入れていないのでWindows 7では映像と正常に合成できないはず 8.xなら動くかも
* WebView2ランタイム
    * 最低限89.0.774.44以上である必要がある
    * もしインストールされていなければインストール
        * インストーラ: https://go.microsoft.com/fwlink/p/?LinkId=2124703
        * 配布ページ: https://developer.microsoft.com/ja-jp/microsoft-edge/webview2/#download-section
        * `Plugins/TVTDataBroadcastingWV2/WebView2/msedgewebview2.exe` `Plugins/TVTDataBroadcastingWV2/WebView2/EBWebView/x64/EmbeddedBrowserWebView.dll` のようにFixed Versionを直接配置しても可能
* Visual C++ 2015-2022ランタイム
    * 万が一入っていなければTVTestのアーキテクチャに合わせて https://aka.ms/vs/17/release/vc_redist.x64.exe (x64) https://aka.ms/vs/17/release/vc_redist.x86.exe (x86) からインストール

映像レンダラはEVR, EVR (Custom Presenter), madVR, システムデフォルト, VMR9, VMR9 Renderless, VMR7, VMR7 Renderlessで動作します。 ただし現時点ではVMR9 Renderless, VMR7 Renderlessを使うとフルスクリーンでの表示などに支障があります。

字幕やコメントを直接映像に合成するプラグインとは相性が悪いため、同時に正常に表示したい場合にはレイヤードウィンドウを使うように設定するかあきらめるなどしてください。
映像レンダラにVMR9 Renderless, VMR7 Renderlessを選択した場合映像に直接合成してもレイヤードウィンドウを使うようにしても字幕やコメントがデータ放送中の映像に合わせて縮小されます。

## 操作

TVTest起動時には有効にならないようになっているため右クリックメニューからプラグインを有効にするか、設定でサイドバーにプラグイン有効アイコンを表示させてそこから有効にしてください。
有効にしたタイミングでWebView2が起動します。

プラグイン有効時に表示されるリモコンかパネルに追加されるリモコンかTVTest側の設定でキーなどをデータ放送の操作に割り当てて操作することが出来ます。

字幕ボタンを押すと[aribb24.js](https://github.com/monyone/aribb24.js)を使った字幕を表示することが出来ます。

テレ東(BSや系列局含)では初回は50秒ほど待たないとデータ放送が表示されません。

## 設定

キー割り当て、パネル、サイドバー、ステータスバーの設定はTVTestの設定で行えます。

### 通信コンテンツ

Plugins/TVTDataBroadcastingWV2.iniを以下のようにすると通信が有効になります。

```ini
[TVTDataBroadcastingWV2]
EnableNetwork=1
```

### プラグイン有効時にリモコンを表示しない

パネルを使う場合やキー割り当てした場合リモコンウィンドウは不要

### TVTest起動時にプラグインを有効にする

## 制約

おおよそ実装されていますがinputmodeや一部のAPI、イベント、要素は未実装です。

通信機能は規定で無効であり、その場合すべての外部へのリクエストはブロックされます。(ICoreWebView2::add_WebResourceRequestedを呼んでいる部分を参照)

## ビルド方法

### TVTestプラグインのビルド

Visual C++ 2022が必要(2019でもおそらく可能)

NuGetパッケージを復元しTVTDataBroadcastingWV2.slnをビルド

### web-bmlのビルド

現状web-bmlを使うために無理やりサブモジュールで参照していてさらにそのweb-bmlもサブモジュールを持っているため以下のコマンドで初期化/更新

```sh
git submodule update --init --recursive
```

Google Fontsよりフォントをダウンロード 存在しなければ場合ＭＳ ゴシックで代替される

```sh
curl https://raw.githubusercontent.com/googlefonts/kosugi-maru/bd22c671a9ffc10cc4313e6f2fd75f2b86d6b14b/fonts/ttf/KosugiMaru-Regular.ttf -o browser/dist/KosugiMaru-Regular.ttf
curl https://raw.githubusercontent.com/googlefonts/kosugi/75171a2738135ab888549e76a9037e826094f0ce/fonts/ttf/Kosugi-Regular.ttf -o browser/dist/Kosugi-Regular.ttf
```

以下のコマンドでビルド

```sh
cd browser
yarn install
yarn workspace @chinachu/aribts build
yarn run build
```

* Plugins/
    * TVTDataBroadcastingWV2.tvtp
    * TVTDataBroadcastingWV2
        * resources/
            * TVTDataBroadcastingWV2.html
            * dist/
                * TVTDataBroadcastingWV2.js
                * Kosugi-Regular.ttf
                * KosugiMaru-Regular.ttf

のように配置するかTVTDataBroadcastingWV2.tvtpと同じディレクトリにTVTDataBroadcastingWV2.iniを作り以下のようにする

```ini
[TVTDataBroadcastingWV2]
ResourceDirectory=x:\xx\browser\
```

