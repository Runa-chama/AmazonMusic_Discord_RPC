# Amazon Music Discord RPC

Amazon Music デスクトップアプリで再生している曲を、Discord のプロフィールに **Listening** として表示する Windows 用ツールです。

- 曲名・アーティスト・アルバム名を表示
- アルバムジャケットを表示
- 再生位置バー（推定）と再生時間を表示
- 一時停止・再開に追従
- Discord が起動していない場合は自動で再接続

> このツールは Amazon および Discord の非公式ツールです。各社とは一切関係ありません。

---

## 動作環境

| 項目 | 内容 |
| --- | --- |
| OS | Windows 10 22H2 / Windows 11 |
| プレイヤー | Amazon Music デスクトップアプリ（Microsoft Store 版） |
| Discord | Discord デスクトップアプリ（ブラウザ版では動作しません） |
| ビルドする場合 | Visual Studio 2022 以降 + 「C++ によるデスクトップ開発」+ Windows SDK |

`dist\AmazonMusicRPC.exe` は Visual C++ 再頒布可能パッケージ不要の静的リンク版です。

---

## クイックスタート

1. `AmazonMusicRPC.exe` を **書き込み可能なフォルダ** に置きます。
   （`Program Files` などは設定ファイルを作れないため避けてください）
2. Discord デスクトップアプリを起動し、以下を確認します。
   **ユーザー設定 → アクティビティ設定 → 「現在のアクティビティをステータスとして表示」をオン**
3. `AmazonMusicRPC.exe` を実行します。
4. 初回起動時に Discord Application ID の入力を求められます。

### Discord Application ID の取得

1. <https://discord.com/developers/applications> を開く
2. **New Application** をクリック
3. 名前を入力（例: `Amazon Music`）
   この名前が Discord プロフィールに表示されます。
4. 作成したアプリの **Application ID** をコピー
5. コンソールに貼り付けて Enter

設定は exe と同じフォルダの `config.ini` に保存されます。次回以降は入力不要です。

6. Amazon Music で曲を再生すると、Discord プロフィールに表示されます。

> Application ID は「OAuth2」の Client Secret や Public Key ではありません。
> **General Information** にある数字だけの Application ID を使います。

---

## 表示例

```
Listening to Amazon Music
くいしんぼハッカー (feat. くいしんぼあかちゃん)
Neko Hacker
未来茶屋 vol.0 - EP
0:42 / 3:19
```

実際の見た目は Discord のクライアントやプロフィールによって多少異なります。

---

## ジャケット画像について

`config.ini` の `art_source` で選べます。

| 値 | 動作 | 特徴 |
| --- | --- | --- |
| `itunes`（既定） | iTunes Search API で曲名・アーティストから検索 | アップロードなし。手軽だが、別リリースのジャケットになることがある |
| `catbox` | Amazon Music の通知用ジャケット画像を読み、catbox.moe にアップロード | 再生中の曲と完全一致。画像 URL は誰でも見られる公開 URL になる |
| `none` | 画像を表示しない | 通信を減らしたい場合 |

### catbox を使う場合の注意

- Amazon Music の Windows 通知が有効で、曲再生時に通知が生成されている必要があります。
- 画像は **catbox.moe にアップロード** されます。公開 URL になるため、プライバシーが気になる場合は `itunes` のままにしてください。
- アップロードに失敗した場合は iTunes の画像にフォールバックします。
- 通知キャッシュの場所（参考）:
  `%LOCALAPPDATA%\Packages\AmazonMobileLLC.AmazonMusic_kc6t79cpj4tp0\LocalCache\Local\Amazon Music\Data\Artwork Cache`

Discord は外部 HTTPS 画像 URL を自動で取り込みます。別途 Discord 側での画像登録は不要です。

---

## 再生位置バーの仕組みと制限

Amazon Music デスクトップアプリは、Windows のメディアセッション（SMTC）に **再生位置や曲の長さを公開していません**。
そのため、このツールは以下の方法で位置を推定しています。

1. 曲が切り替わったことを検出した時点で、Amazon Music のログから直近の `playbackTerminatedEvent` の時刻を読む
2. その時刻を曲の開始位置とみなし、経過時間を加算
3. 一時停止中は加算を止め、再開時に再開

### 制限

- **シーク（途中送り・戻し）は検出できません。** 次の曲に切り替わるまでバーがずれることがあります。
- 曲の途中からツールを起動した場合、ログに新しい開始時刻がなければ 0:00 から始まります。
- Amazon Music のログが無効・ローテーション済みの場合は 0:00 から始まります。
- 曲の長さは iTunes から取得します。一致する曲が見つからない場合はバーを表示しません。
- ブラウザ版（Chrome / Edge など）のメディアセッションは実際の再生位置を返すことがあり、その場合は実位置を使います。

「だいたい合っていればよい」という前提でお使いください。

---

## config.ini

初回起動時に自動生成されます。`config.example.ini` も参考にしてください。

```ini
client_id=123456789012345678
app_filter=amazon
art_source=itunes
art_country=JP
poll_ms=750
show_paused=true
```

| キー | 既定値 | 説明 |
| --- | --- | --- |
| `client_id` | （空） | Discord Application ID。17〜20 桁の数字 |
| `app_filter` | `amazon` | 対象アプリの判定。セミコロン区切り、大文字小文字を無視した部分一致。空欄で全アプリ |
| `art_source` | `itunes` | `itunes` / `catbox` / `none` |
| `art_country` | `JP` | iTunes のストア（`JP`, `US` など） |
| `poll_ms` | `750` | メディア情報の取得間隔（250〜5000 ms） |
| `show_paused` | `true` | 一時停止中もプロフィールに表示する |

### app_filter の例

```ini
# Amazon Music デスクトップ
app_filter=amazon

# ブラウザ版
app_filter=chrome;msedge;firefox

# Spotify など他のプレイヤーも対象にする
app_filter=amazon;spotify

# すべてのメディアセッション
app_filter=
```

---

## コマンドラインオプション

```text
AmazonMusicRPC.exe [オプション]

  --config <path>            設定ファイルのパスを指定
  --client-id <id>           Application ID を一時的に指定（config.ini は変更しない）
  --no-art                   ジャケット画像を表示しない
  --any                      対象アプリを問わず、再生中のメディアを表示
  --lookup <title> [artist]  iTunes メタデータ検索をテスト
  --probe                    メディアセッションの検出結果だけを表示
  --verbose, -v              デバッグログを表示
  --help, -h                 ヘルプを表示
```

### 使用例

```bat
REM Amazon Music が検出できるか確認
AmazonMusicRPC.exe --probe

REM iTunes のメタデータ検索を確認
AmazonMusicRPC.exe --lookup "くいしんぼハッカー" "Neko Hacker"

REM すべてのプレイヤーを対象にする
AmazonMusicRPC.exe --any

REM 別の Application ID で一時的に起動
AmazonMusicRPC.exe --client-id 123456789012345678 --verbose
```

`--probe` は Discord に接続しないため、Application ID がなくても実行できます。
「何も表示されない」ときはまず `--probe` で対象セッションが取得できているか確認してください。

---

## 自動起動

1. `Win + R` を押し、`shell:startup` と入力して Enter
2. `AmazonMusicRPC.exe` のショートカットをそのフォルダに置く
3. ショートカットのプロパティ → **実行時の大きさ** を **最小化** に設定

コンソールウィンドウを閉じるとツールは終了します。最小化したままにしてください。
タスクスケジューラの「ログオン時」に登録してもかまいません。

---

## トラブルシューティング

### Discord に何も表示されない

- Discord **デスクトップアプリ**が起動していますか？（ブラウザ版 Discord は IPC に対応していません）
- Discord の「現在のアクティビティをステータスとして表示」がオンですか？
- Amazon Music で実際に曲を再生していますか？（アプリを開いただけでは検出されません）
- `AmazonMusicRPC.exe --probe` を実行し、Amazon Music のセッションと曲名が出るか確認してください。
- セッションが出ない場合は Amazon Music を再起動してください。

### `Invalid Client ID` と表示される

- Application ID が間違っています。Developer Portal の **Application ID**（数字のみ）をコピーしてください。
- Client Secret や Public Key を貼り付けていませんか？
- `config.ini` の `client_id=` を確認してください。

### `Discord が見つかりません` と表示される

- Discord デスクトップアプリが起動しているか確認してください。
- Discord を再起動すると自動で再接続します。
- ツールを管理者として実行している場合、Discord と権限が異なると接続できないことがあります。通常権限で実行してください。

### ジャケット画像が表示されない

- 曲が切り替わってから数秒待ってください。
- `art_source=itunes` の場合、iTunes に一致する曲がないと画像が出ません。`--lookup` で確認できます。
- `art_source=catbox` の場合、Amazon Music の通知が有効か確認してください。失敗時は iTunes にフォールバックします。
- Discord 側の画像キャッシュが残っている場合は Discord を再起動してください。

### 再生位置バーがずれる

仕様上の制限です。シークや途中からの起動ではずれます。次の曲に切り替わるとリセットされます。

### コンソールの日本語が文字化けする

Windows Terminal での実行を推奨します。従来のコンソールではフォントの設定によって文字化けすることがあります。
`chcp 65001` を実行してから起動する方法も試してください。

### Windows Defender / SmartScreen の警告が出る

署名なしの exe のため警告が出ることがあります。
「詳細情報」→「実行」で起動できます。気になる場合はソースからビルドしてください。

---

## ビルド

### build.bat（推奨）

必要なもの:

- Visual Studio 2022 以降
- Visual Studio Installer で「C++ によるデスクトップ開発」をインストール
- Windows 10 / 11 SDK

リポジトリ直下で:

```bat
build.bat
```

成功すると `dist\AmazonMusicRPC.exe` が生成されます。
`build.bat` は `vswhere` で最新の Visual Studio を探し、`vcvars64.bat` を読み込んで `cl.exe` でビルドします。

### CMake

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

CMake のジェネレーターはインストールされている Visual Studio に合わせてください。
Visual Studio 18 など、使用中の CMake が対応していないジェネレーターの場合は `build.bat` を使ってください。

### GitHub Actions

`.github/workflows/build.yml` で Windows ランナーによるビルドと exe の artifact アップロードを行います。

---

## 仕組み

| 部分 | 内容 |
| --- | --- |
| メディア情報 | `Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager`（WinRT / GSMTC） |
| Discord 連携 | `\\.\pipe\discord-ipc-0` 〜 `discord-ipc-9` への名前付きパイプ接続。opcode 0 でハンドシェイク、opcode 1 の `SET_ACTIVITY` で送信 |
| アクティビティ種別 | `type: 2`（Listening） |
| メタデータ補完 | iTunes Search API（アルバム、アーティスト、曲の長さ、ジャケット） |
| 再生位置 | Amazon Music のログ `playbackTerminatedEvent` + 経過時間 |
| 完全一致ジャケット | Amazon Music の通知キャッシュ画像（`art_source=catbox` 時のみ） |

Discord の Application ID は、プロフィールに表示されるアプリ名を決めるために使います。
アプリを `Amazon Music` という名前で作成すると、自然な表示になります。

---

## プライバシー

- Amazon アカウントの ID・パスワード・Cookie などは一切扱いません。
- ローカルのメディアセッション、Amazon Music のログ、通知キャッシュを読み取ります。
- iTunes API には曲名・アーティスト名が送信されます（Apple の公開 API）。
- `art_source=catbox` の場合、ジャケット画像が catbox.moe に公開アップロードされます。
- Discord には通常の Rich Presence と同様にアクティビティ情報が送信されます。
- `config.ini` はローカルに保存されます。Application ID 自体は秘密情報ではありませんが、配布する場合は内容を確認してください。

---

## 既知の制限

- Amazon Music デスクトップアプリ専用の作りです。Amazon Music のアップデートで動作しなくなる可能性があります。
- シーク検出はできません（上記「再生位置バー」参照）。
- iTunes に存在しない曲は、ジャケットやアルバム名が表示されないことがあります。
- SMTC がアーティスト名を返さない曲では、iTunes の検索結果に依存します。
- Discord のアプリ名は、ユーザーが Developer Portal で作成したアプリ名になります。
- Linux / macOS には対応していません。

---

## ライセンス

MIT License. 詳細は [LICENSE](LICENSE) を参照してください。

---

## 免責

本ツールは非公式であり、Amazon および Discord とは関係ありません。
利用は自己責任でお願いします。各サービスの利用規約を確認したうえで使用してください。
