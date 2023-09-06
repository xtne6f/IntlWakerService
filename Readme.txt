IntlWakerService - 一部のPCで発生するスリープ解除遅延問題を打ち消すWindowsサービス


説明:

Intel 300シリーズ以降のチップセットを搭載したマザーボードで発生することがある、スリープからの復帰が1時間に約25秒ずつ遅れ
ていく問題を、予測される遅れを差し引いたタイマーを追加することで打ち消すサービスです。
※おおむね参考URL[2]の「個人的バッドノウハウ解法」を一般化してサービス化したものです。

IntlWakerService.exe、IntlWakerService.ini、Install.bat、Remove.batを配置して（システムサービスなので"C:\Windows"フォル
ダ以下などファイルが権限で保護される場所を推奨）Install.batをエクスプローラーの右クリックメニューで「管理者として実行」
すると、IntlWakerServiceサービスがインストールされて開始します。アンインストールはRemove.batを管理者で実行してください。
どのようなタイマーが追加されるかは管理者のコマンドプロンプトで`powercfg /waketimers`を実行して確認してください。システム
の復帰状況に関するログはイベントビューアーで確認できます。

Visual Studio Express 2017 for Windows DesktopでビルドしてWindows10 22H2 64bitで動作を確認しています。

IntlWakerService.iniのMinimumTimeSpanHoursキーはタイマーを追加する最小の復帰間隔のしきい値(最小値=1(時間))を指定します。
既定値は[=3]です。参考URLでは「3.5時間以上の復帰間隔で遅れる」と報告されていますがこちらのMSIマザーボードの環境では復帰
間隔にかかわらず遅れるので、このような環境では[=1]に変更すると良いです。
SubtractMsecPerHourキーは1時間に何ミリ秒ずつ遅れを差し引くかを指定します。既定値は[=25000]です。

必須ではありませんが、たとえば遅れの予測が外れて早めに復帰してしまった場合無操作により2分後に再スリープしてしまうと思う
ので、この挙動が気になる場合はOSの「システム無人スリープタイムアウト」を調整してください（方法は検索してください）。

（余談）
スリープや復帰している最中にタイマー関連のAPIを呼び出すとKP41でクラッシュすることも分かりました。
最近のACPIウェイクアラーム周りはなんかやばそうです。


参考URL:

[1] Intel 300シリーズチップセット以降のスリープ解除遅延問題 - アルチーナの魔法の島
https://lileenchantee.blog.fc2.com/blog-entry-58.html

[2] PCを新調したらマルチディスプレイでカクつき問題とスリープ解除数分遅延でハマった話 (2) - 星崎レンスターズ
https://rennstars.blackcats.jp/entry/2022/07/22/182230

[3] List Currently Active CreateWaitableTimer Events - Stack Overflow
https://stackoverflow.com/questions/44752455/list-currently-active-createwaitabletimer-events


ソース:

https://github.com/xtne6f/IntlWakerService
ライセンスはMITです。
