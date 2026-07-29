# 静的解析レポート

実施日: 2026-07-29

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

### InputSettingsStore

Input Settings読込を文書検証、Keyboard・Gamepad項目検証、1 Action解析、
重複検査・Binding構築、適用処理へ分けた。`InputSettingsStore::Load` は
98 NLOC / CCN 52から37 NLOC / CCN 12へ縮小した。
Input Settings処理の回帰上限は CCN 12 / 関数長55である。

### EditorScene

14,090 行の単一ファイルから、まず次の責務を分離した。

- `EditorScenePersistence.cpp`: Scene の New/Open/Save、履歴、ファイルダイアログ
- `EditorScenePrefabInstantiation.cpp`: Prefab検証、読込、Hierarchy生成、配置
- `EditorScenePrefabSaving.cpp`: 選択階層の抽出、外部参照除去、Prefab保存
- `EditorSceneRuntime.cpp`: Play Mode、Runtime World、Animator
- `EditorSceneRuntimeAudio.cpp`: Listener、Audio Source、Voiceライフサイクル
- `EditorSceneRuntimeUiEvents.cpp`: Runtime UI Event検証・Behavior dispatch
- `EditorSceneConsole.cpp`: Console と Script の監視・再コンパイル
- `EditorSceneEntityRename.cpp`: Rename Popup、入力、確定、Cancel
- `EditorSceneAssetDiscovery.cpp`: Asset・Sceneの走査、分類、表示フォルダー解決
- `EditorSceneAssetBrowserEntry.cpp`: Asset行、起動、Drag Source、Context Menu
- `EditorSceneAssetPreview.cpp`: Preview状態、Model読込、Cameraフレーミング
- `EditorSceneAssetPreviewPopup.cpp`: Model統計、Animation操作、Preview描画・回転
- `EditorSceneAssets.cpp`: Asset Browser、Import、Rename/Delete、参照更新
- `EditorSceneAudioPreview.cpp`: Audio Previewの状態、操作、読込、情報表示
- `EditorSceneCameraPreview.cpp`: Camera Previewの準備、描画、Overlay
- `EditorSceneHierarchy.cpp`: Hierarchy、Entity 作成、選択、コピー、親子付け
- `EditorSceneHierarchyNode.cpp`: Entity行、Context Menu、Drag & Drop
- `ProjectScriptLibraryValidation.cpp`: Script DLLのAPI・登録内容検証とRegistry登録
- `EditorSceneInspector.cpp`: Inspector の統括、Transform、Script
- `EditorSceneInspectorAddComponent.cpp`: Component追加、Script Drop
- `EditorSceneInspectorAnimator.cpp`: Animator Clip、Preview、再生設定
- `EditorSceneInspectorMaterial.cpp`: Material属性、Texture Slot、Preview
- `EditorSceneInspectorPhysics.cpp`: Collider、Character Controller
- `EditorSceneInspectorRendering.cpp`: Camera、Light、Material
- `EditorSceneInspectorMedia.cpp`: Audio、Animator
- `EditorSceneInspectorUi.cpp`: Canvas、Event System
- `EditorSceneInspectorText.cpp`: Text一般設定、Font、Typography、配置
- `EditorSceneInspectorTransform.cpp`: Entity Header、Layer、Transform編集
- `EditorSceneInspectorImage.cpp`: Image一般設定、Fill、Texture、Layout、Preview
- `EditorSceneInspectorScriptAssets.cpp`: Animation Clip、Input Action、Scene Property
- `EditorSceneInspectorUiControls.cpp`: Button、Toggle、Slider、Dropdown、Input
- `EditorSceneGameUi.cpp`: Game View のランタイムUI入力・描画
- `EditorSceneGameUiEditing.cpp`: Game View上の選択、ドラッグ、リサイズ
- `EditorSceneInitialization.cpp`: 入力、Script、Surface、描画、Camera初期化
- `EditorSceneLighting.cpp`: LightのWorld解決、種類別Scene Lighting反映
- `EditorSceneComponentGizmos.cpp`: Camera・Light・Audio・Physicsアイコン
- `EditorSceneGizmos.cpp`: Scene View のガイド、選択枠、Transform Gizmo
- `EditorSceneMainMenu.cpp`: File・Build・Edit・View・Runtimeメニュー
- `EditorScenePanels.cpp`: Scene・Game・補助パネルのウィンドウ統括
- `EditorSceneProjectSettings.cpp`: General、Player、Physics、Input設定
- `EditorScenePlayerBuild.cpp`: Player構築の検証、出力設定、Package生成
- `EditorScenePlayerPreview.cpp`: Preview起動条件、Project検証、Process起動
- `EditorSceneRendering.cpp`: 描画リソース解決、メッシュ送信、Scene Grid
- `EditorSceneUpdate.cpp`: シミュレーションとViewリソースのフレーム更新
- `EditorSceneGameUiInteraction.cpp`: Game UIの入力、選択、Control操作、Popup描画
- `EditorSceneGameUiVisuals.cpp`: Game UIの画像、文字、Control固有表現の描画
- `EditorSceneViewportNavigation.cpp`: Scene Viewのカメラ操作、フォーカス、位置合わせ
- `EditorSceneViewportSelection.cpp`: Scene Viewのコンポーネント選択、Mesh Raycast
- `internal/EditorSceneGameUiUtils.h`: Canvas配置、UI矩形、描画順の共有計算
- `internal/EditorSceneViewportUtils.h`: Ray、投影、モデル境界などの共有計算

元の実装ファイルは 14,090 行から約 6,700 行まで縮小した。さらに
3,500 行規模だった Inspector は、最大 1,010 行の9ファイルへ分割した。
`DrawInspectorPanel` は 3,360 NLOC / CCN 764 から 40 NLOC / CCN 6、
Script プロパティの統括処理は 396 NLOC / CCN 113 から
18 NLOC / CCN 6 へ縮小した。Inspector 群には新しい上限
CCN 66 / 関数長 272 を解析スクリプトで固定している。さらにMaterialの
Surface設定、Base Color・Normal・PBR Texture Slot、Previewを
`EditorSceneInspectorMaterial.cpp` へ分離し、
`DrawMaterialOverrideInspector` を263 NLOC / CCN 66から
14 NLOC / CCN 4へ縮小した。Material処理の回帰上限は
CCN 11 / 関数長46である。公開 API と保存形式は変更していない。

Animator InspectorからRuntime状態、Clip選択、Preview操作、再生設定を
`EditorSceneInspectorAnimator.cpp` へ分離した。`DrawAnimatorInspector` は
184 NLOC / CCN 66から16 NLOC / CCN 4へ縮小した。Animator処理の
回帰上限は CCN 26 / 関数長52である。

Image Inspectorを一般設定、Fill、Anchor・Pivot、Texture選択、
Layout・Color、Preview、Canvas要件へ分け、専用ファイルへ移した。
`DrawImageInspector` は211 NLOC / CCN 57から18 NLOC / CCN 3へ縮小した。
Image処理の回帰上限は CCN 12 / 関数長42である。

Text Inspectorを一般設定、Content・Position、Font Picker・検証、
Typography、Color・Alignment、Canvas要件へ分け、ファイル名も
`EditorSceneInspectorText.cpp` へ最適化した。`DrawTextInspector` は
172 NLOC / CCN 49から18 NLOC / CCN 3へ縮小した。
Text処理の回帰上限は CCN 12 / 関数長36である。

Entity HeaderとTransform編集をActive、Identity、Layer、Toolbar、
Reset・Paste、Transform Fieldへ分け、専用ファイルへ移した。
`DrawEntityHeaderAndTransformInspector` は170 NLOC / CCN 48から
17 NLOC / CCN 2へ縮小した。Transform Inspectorの回帰上限は
CCN 14 / 関数長44である。

Add Componentの共通履歴処理を抽出し、Rendering、Audio・Animation、
UI基盤、UI Controls、Physics、Scriptへ分け、専用ファイルへ移した。
`DrawAddComponentInspector` は157 NLOC / CCN 47から17 NLOC / CCN 3へ
縮小した。Add Component処理の回帰上限は CCN 11 / 関数長22である。

Asset系Script Propertyを共通の値取得・代入と、Animation Clip、
Input Action、Sceneの型別UIへ分け、専用ファイルへ移した。
`DrawAssetScriptPropertyInspector` は164 NLOC / CCN 55から
19 NLOC / CCN 4へ縮小した。Script Asset Property処理の回帰上限は
CCN 16 / 関数長41である。

Runtime Audioの開始・更新・一時停止・終了を専用ファイルへ集約し、
Listener、Voice停止・再生、Command処理、Parameter同期、Source更新へ分けた。
`UpdateRuntimeAudio` は152 NLOC / CCN 49から10 NLOC / CCN 4へ縮小した。
Runtime Audio処理の回帰上限は CCN 19 / 関数長52である。

Runtime WorldのInput Field、Dropdown、Slider、Button・Toggle Eventを
共通Interactable判定と種類別dispatchへ分け、専用ファイルへ移した。
`UpdateRuntimeWorld` は101 NLOC / CCN 47から19 NLOC / CCN 5へ縮小した。
Runtime UI Event処理の回帰上限は CCN 12 / 関数長25である。

Model Preview Popupを利用可否判定、Model統計、Animation選択・再生・シーク、
Preview描画、回転操作へ分け、専用ファイルへ移した。
`DrawAssetPreviewPopup` は185 NLOC / CCN 44から20 NLOC / CCN 3へ縮小し、
`EditorSceneAssets.cpp` は1,020行となった。Model Preview Popup処理の
回帰上限は CCN 9 / 関数長28である。

その後、残っていた `EditorScene.cpp` 6,676 行から Game UI 約1,400行と
Scene Gizmo 約1,000行を分離し、同ファイルを約3,570行まで縮小した。
Game UI の表示ループも独立させ、`DrawGameUi` は
891 NLOC / CCN 368 から 633 NLOC / CCN 289 へ縮小した。
さらに方向ナビゲーション探索を独立させ、`DrawGameUi` は
529 NLOC / CCN 249まで縮小した。続いて入力収集、選択・Navigation、
Button・Slider・Dropdown・InputField操作、Popup描画を
`EditorSceneGameUiInteraction.cpp` へ分離し、`DrawGameUi` を
50 NLOC / CCN 13まで縮小した。さらに画像のAspect・Fill、Button色遷移、
Toggle、Slider、Text描画を `EditorSceneGameUiVisuals.cpp` へ分離し、
`DrawGameUiVisuals` を276 NLOC / CCN 81から64 NLOC / CCN 12へ縮小した。
Gizmo はCamera・Light・Audio・Physicsアイコンを種類別関数へ分け、
専用ファイルへ移した。`DrawSceneComponentGizmos` は
361 NLOC / CCN 101から11 NLOC / CCN 3へ縮小し、Component Gizmoには
CCN 13 / 関数長28の回帰上限を設定した。現在の回帰上限はGame UI描画が
CCN 43 / 関数長116、Game UI固有表現が CCN 20 / 関数長61、
Game UI操作が CCN 32 / 関数長70、
Gizmo 群が CCN 48 / 関数長148である。

Game UI編集処理は専用ファイルへ分離し、Canvas配置・UI矩形計算を共通
ユーティリティ化した。さらにHover・Resize判定、Pointer入力、Keyboard Nudge、
Overlay、Cursorを分け、`HandleGameUiEditing` は334 NLOC / CCN 95から
19 NLOC / CCN 7へ縮小した。編集処理には CCN 25 / 関数長90の
回帰上限を設定している。

Project Settings はカテゴリ別関数へ分けて専用ファイルへ移し、
`EditorScene.cpp` を約3,570行から2,858行まで縮小した。
`DrawProjectSettingsWindow` は 473 NLOC / CCN 106 から15 NLOC / CCN 3、
Input設定も291 NLOC / CCN 73から170 NLOC / CCN 41へ縮小した。
Project Settingsには CCN 41 / 関数長173の回帰上限を設定している。

パネル描画はScene View、Game View、Hierarchy・Project、Console・Inspectorへ
分割して専用ファイルへ移した。`DrawPanels` は263 NLOC / CCN 66から
21 NLOC / CCN 6へ縮小し、`EditorScene.cpp` は2,588行となった。
パネル群には CCN 32 / 関数長131の回帰上限を設定している。

Hierarchyの再帰描画からEntity行、選択、Context Menu、Drag & Dropを
`EditorSceneHierarchyNode.cpp` へ分離した。`DrawEntityNode` は
199 NLOC / CCN 77から33 NLOC / CCN 10へ縮小した。Hierarchy本体の
回帰上限は CCN 39 / 関数長187、Node操作は CCN 27 / 関数長71である。

Project Script DLLの準備・API解決・型とProperty検証・Registry登録を
`ProjectScriptLibraryValidation.cpp` へ分離した。
`ProjectScriptLibrary::Load` は175 NLOC / CCN 72から32 NLOC / CCN 7へ
縮小した。Library本体の回帰上限は CCN 7 / 関数長34、検証処理は
CCN 35 / 関数長44である。

メインメニューはFile、Build、Edit、View、Runtime操作、タイトル表示へ分割し、
専用ファイルへ移した。`DrawMainMenu` は186 NLOC / CCN 53から
13 NLOC / CCN 2へ縮小し、`EditorScene.cpp` は2,401行となった。
メニュー群には CCN 15 / 関数長46の回帰上限を設定している。

フレーム更新はエディターシミュレーション、Scene View、Game View、
Previewリソースへ分割して専用ファイルへ移した。`Update` は
81 NLOC / CCN 52から6 NLOC / CCN 1へ縮小し、`EditorScene.cpp` は
2,319行となった。更新処理には CCN 17 / 関数長27の回帰上限を設定している。

描画構築はリソース種別ごとの解決、エンティティ単位の送信、メッシュ単位の
送信、マテリアル上書きへ分け、専用ファイルへ移した。
`BuildRenderScene` は143 NLOC / CCN 45から10 NLOC / CCN 4、
`ResolveMeshResources` は82 NLOC / CCN 36から10 NLOC / CCN 4、
`SubmitRenderMesh` は90 NLOC / CCN 22から26 NLOC / CCN 6へ縮小した。
`EditorScene.cpp` は2,039行となり、描画処理には CCN 21 / 関数長45の
回帰上限を設定している。

Asset Browser の更新処理はキャッシュ初期化、Scene一覧、現在フォルダー、
全Asset一覧、種類別分類へ分け、走査処理を専用ファイルへ移した。
`RefreshAssetBrowser` は115 NLOC / CCN 47から11 NLOC / CCN 3へ縮小し、
`EditorScene.cpp` は1,902行となった。Asset走査処理には
CCN 18 / 関数長33の回帰上限を設定している。

Asset Browser の各行は表示分類、選択・起動、Drag Source、Texture割り当て、
Context Menuへ分け、専用ファイルへ移した。`DrawAssetBrowserEntry` は
133 NLOC / CCN 61から21 NLOC / CCN 3へ縮小した。Asset行処理には
CCN 24 / 関数長63の回帰上限を設定している。

Scene Viewのカメラ操作は入力状態、カーソル捕捉、回転、キーボード移動、
パン・ズームへ分割し、Assetドロップやカメラ位置合わせとともに
専用ファイルへ移した。`HandleSceneCameraControls` は
120 NLOC / CCN 39から13 NLOC / CCN 3へ縮小し、`EditorScene.cpp` は
1,632行となった。Viewport Navigationには CCN 11 / 関数長50の
回帰上限を設定している。

Scene Viewの選択処理はコンポーネントアイコン探索、Mesh Raycast、選択反映へ
分割して専用ファイルへ移した。`PickSceneEntity` は
92 NLOC / CCN 36から20 NLOC / CCN 7へ縮小し、`EditorScene.cpp` は
1,539行となった。Viewport Selectionには CCN 13 / 関数長42の
回帰上限を設定している。

初期化処理は入力設定、Project Script、Docking、RenderSurface、描画リソース、
Cameraへ分割して専用ファイルへ移した。`Initialize` は
75 NLOC / CCN 21から16 NLOC / CCN 4へ縮小し、`EditorScene.cpp` は
1,462行となった。初期化処理には CCN 7 / 関数長22の回帰上限を設定している。

Prefab保存は保存準備、子孫収集、Entity複製、外部Entity参照の除去、保存、
Asset選択更新へ分割して専用ファイルへ移した。`SaveSelectionAsPrefab` は
70 NLOC / CCN 21から15 NLOC / CCN 3へ縮小し、`EditorScene.cpp` は
1,389行となった。Prefab保存処理には CCN 6 / 関数長26の回帰上限を
設定している。

Player Package構築はビルド可否判定、Project検証、実行ファイル解決、
リクエスト構築、結果表示へ分割して専用ファイルへ移した。
`BuildPlayerPackage` は51 NLOC / CCN 16から24 NLOC / CCN 6へ縮小し、
`EditorScene.cpp` は1,334行となった。Player Buildには
CCN 8 / 関数長24の回帰上限を設定している。

Asset Preview更新は状態リセット、対象ファイル解決、Model読込とキャッシュ、
Cameraフレーミングへ分割して専用ファイルへ移した。`UpdateAssetPreview` は
72 NLOC / CCN 14から15 NLOC / CCN 4へ縮小し、`EditorScene.cpp` は
1,260行となった。Asset Previewには CCN 6 / 関数長26の回帰上限を
設定している。

Lighting送信は有効性とWorld変換の解決、Directional、Point、Spotの反映へ
分割して専用ファイルへ移した。`SubmitLighting` は
52 NLOC / CCN 14から26 NLOC / CCN 6へ縮小し、`EditorScene.cpp` は
1,206行となった。Lighting処理には CCN 7 / 関数長26の回帰上限を
設定している。

Player Preview起動は起動可否、Project検証、Editor実行ファイル解決、
プロセス起動へ分割して専用ファイルへ移した。`LaunchPlayerPreview` は
49 NLOC / CCN 13から11 NLOC / CCN 5へ縮小し、`EditorScene.cpp` は
1,156行となった。Player Previewには CCN 8 / 関数長19の回帰上限を
設定している。

Audio Preview描画は選択同期、再生判定、操作UI、音声読込・再生、情報表示へ
分割して専用ファイルへ移した。`DrawAudioAssetPreview` は
44 NLOC / CCN 13から7 NLOC / CCN 2へ縮小し、`EditorScene.cpp` は
1,102行となった。Audio Previewには CCN 5 / 関数長18の回帰上限を
設定している。

Prefab生成はAssetパス検証、Prefab読込、Hierarchy生成、配置、選択反映へ
分割して専用ファイルへ移した。`InstantiatePrefabAsset` は
40 NLOC / CCN 13から23 NLOC / CCN 5へ縮小し、`EditorScene.cpp` は
1,061行となった。Prefab生成処理には CCN 6 / 関数長23の回帰上限を
設定している。

Entity Rename dialogはPopup準備、対象検証、入力、Rename確定、Cancelへ
分割して専用ファイルへ移した。`DrawEntityRenameDialog` は
53 NLOC / CCN 11から18 NLOC / CCN 5へ縮小し、`EditorScene.cpp` は
1,007行となった。Entity Renameには CCN 5 / 関数長18の回帰上限を
設定している。

Camera Preview描画は対象解決、矩形とCamera準備、RenderSurface描画、
ImGui Overlay描画へ分割して専用ファイルへ移した。
`DrawSelectedCameraPreview` は43 NLOC / CCN 11から11 NLOC / CCN 3へ
縮小し、`EditorScene.cpp` は962行となった。Camera Previewには
CCN 9 / 関数長18の回帰上限を設定している。

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
