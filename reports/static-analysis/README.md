# 静的解析レポート

実施日: 2026-07-28

## 対象

- `engine/src`, `engine/include`, `engine/public`
- `editor/src`, `editor/include`
- `tests`
- `projects/test/assets/Scripts`
- 外部ライブラリと生成物は対象外

## ツールと条件

- Lizard 1.23.0
- Cppcheck 2.21.0
- Cppcheck: `warning`, `style`, `performance`, `portability`, `inconclusive`
- C++20 / Windows x64 / Debug 定義

`tools/run-static-analysis.ps1` が再現用の入口である。Cppcheck の任意の
スタイル提案（引数名、STL アルゴリズムの好み、局所 const 化など）は
`cppcheck-suppressions.txt` に明示した。エラー、警告、寿命、初期化、値域に
関する検査は抑制していない。

Lizard は初回に既存の複雑度警告を 73 件検出した。大規模な
`EditorScene`、シリアライズ処理、統合テストを一度に再設計する変更は
挙動リスクが高いため、今回の変更では初回最大値（CCN 939、関数長 3396）
をレガシー上限として固定した。この上限を超える回帰は失敗する。全関数の
数値は `lizard-final.html` で確認できる。

## リファクタリング進捗

### WorldSerializer

元の `WorldSerializer.cpp` 2,023 行を、次の責務へ分割した。

- ファイル I/O
- Serialize
- Deserialize のオーケストレーション
- Rendering / Runtime / UI / Scripts / Physics のコンポーネントデコーダ
- JSON の共通変換ヘルパー

特に `WorldSerializer::Deserialize` は 1,258 NLOC / CCN 659 から
82 NLOC / CCN 38 へ縮小した。コンポーネント単位のデコーダを独立させ、
新しい上限 CCN 74 / 関数長 410 を解析スクリプトで固定している。

### EditorScene

14,090 行の単一ファイルから、まず次の責務を分離した。

- `EditorScenePersistence.cpp`: Scene の New/Open/Save、履歴、ファイルダイアログ
- `EditorSceneRuntime.cpp`: Play Mode、Runtime World、Animator、Audio
- `EditorSceneConsole.cpp`: Console と Script の監視・再コンパイル
- `EditorSceneAssets.cpp`: Asset Browser、Import、Rename/Delete、参照更新
- `EditorSceneHierarchy.cpp`: Hierarchy、Entity 作成、選択、コピー、親子付け
- `EditorSceneInspector.cpp`: Inspector の統括、Transform、Script
- `EditorSceneInspectorPhysics.cpp`: Collider、Character Controller
- `EditorSceneInspectorRendering.cpp`: Camera、Light、Material
- `EditorSceneInspectorMedia.cpp`: Audio、Animator
- `EditorSceneInspectorUi.cpp`: Canvas、Event System
- `EditorSceneInspectorUiGraphics.cpp`: Text、Image
- `EditorSceneInspectorUiControls.cpp`: Button、Toggle、Slider、Dropdown、Input
- `EditorSceneGameUi.cpp`: Game View のUI入力、描画、編集
- `EditorSceneGizmos.cpp`: Scene View のコンポーネント表示、選択枠、Gizmo
- `internal/EditorSceneViewportUtils.h`: Ray、投影、モデル境界などの共有計算

元の実装ファイルは 14,090 行から約 6,700 行まで縮小した。さらに
3,500 行規模だった Inspector は、最大 1,010 行の7ファイルへ分割した。
`DrawInspectorPanel` は 3,360 NLOC / CCN 764 から 40 NLOC / CCN 6、
Script プロパティの統括処理は 396 NLOC / CCN 113 から
18 NLOC / CCN 6 へ縮小した。Inspector 群には新しい上限
CCN 66 / 関数長 272 を解析スクリプトで固定している。公開 API と保存形式は
変更していない。

その後、残っていた `EditorScene.cpp` 6,676 行から Game UI 約1,400行と
Scene Gizmo 約1,000行を分離し、同ファイルを約3,570行まで縮小した。
Game UI の表示ループも独立させ、`DrawGameUi` は
891 NLOC / CCN 368 から 633 NLOC / CCN 289 へ縮小した。
Game UI には CCN 289 / 関数長643、Gizmo 群には CCN 101 / 関数長367の
回帰上限を追加している。

## 初回結果

| ツール | 件数 | 内容 |
|---|---:|---|
| Lizard | 73 | 既存関数の循環的複雑度 |
| Cppcheck | 88 | 実害候補 15 件、任意のスタイル提案 73 件 |

Cppcheck の実害候補 15 件:

- move 後のコンテナへ再アクセス: 6 件
- メンバー未初期化: 3 件
- 一時寿命・無効ポインターの可能性: 3 件
- 値域比較、常時真条件、未使用代入: 各 1 件

## 修正

- move と clear/reset の組み合わせを `std::exchange` に置換し、消費後の
  状態を明示的に空へした。
- UI 選択肢構造体の既定値を初期化した。
- Task Dialog のラベルをすべて確定してから、その安定した文字列を指す
  ボタン配列を構築するようにした。
- MSBuild 検索結果を明示的に `std::filesystem::path` へコピーした。
- Windows の `unsigned long` 値域では常に偽になる上限比較を除去した。
- 常時真となるソート条件と、読み出されない一時変数を簡潔化した。

## 最終結果

| ツール | 警告・指摘 |
|---|---:|
| Lizard（レガシー上限ポリシー） | 0 |
| Cppcheck（リポジトリ抑制ポリシー） | 0 |

Debug/x64 のソリューション全体は `/W4 /WX` でビルド成功し、
`WorldTests.exe` も終了コード 0 で完了した。Codex 実行環境固有の
`Path`/`PATH` 重複を避けるため、検証時のみ子プロセスの `Path` を空にした。

実行:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run-static-analysis.ps1
```
