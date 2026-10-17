#pragma once
typedef enum UiText {
    UI_TITLE,UI_BACKEND,UI_SCALE,UI_ASPECT,UI_GAME,UI_REFRESH,UI_SIZE,UI_HINT,
    UI_IMPORT,UI_SAVE,UI_CANCEL,UI_AUTO,UI_WIDE,UI_ULTRAWIDE,UI_IMPORT_TITLE,
    UI_ID,UI_NAME,UI_ID_HINT,UI_CHOOSE,UI_ID_ERROR,UI_DIALOG_ERROR,UI_START_ERROR,
    UI_BUSY,UI_READY,UI_FAILED,UI_IMPORT_HELP,UI_LOG_ERROR,UI_INTEGER_ERROR,UI_CONTROLS,UI_CONTROLS_BODY,UI_CLOSE,UI_SIZE_CUSTOM,UI_FIXED_ASPECT
} UiText;
static const wchar_t *const ui_ids[]={L"Chinese",L"Japanese",L"English"};
static const wchar_t *const ui_text[][3]={
    {L"Shin Megami Tensei - NINE 设置",L"Shin Megami Tensei - NINE 設定",L"Shin Megami Tensei - NINE Settings"},
    {L"渲染后端",L"描画バックエンド",L"Renderer"},
    {L"内部渲染倍率",L"内部解像度倍率",L"Internal resolution"},
    {L"游戏画面比例",L"画面の縦横比",L"Aspect ratio"},
    {L"游戏语言",L"ゲーム言語",L"Game language"},
    {L"画面刷新率",L"リフレッシュレート",L"Refresh rate"},
    {L"窗口尺寸",L"ウィンドウサイズ",L"Window size"},
    {L"游戏画面比例独立于窗口尺寸，多余区域留黑。\n固定宽高比：拖动窗口时保持所选尺寸的比例。\n保存后下次启动游戏时生效。",L"画面比率とウィンドウサイズは独立し、余白は黒になります。\n縦横比を固定すると、リサイズ時に選択したサイズの比率を維持します。\n保存した設定は次回のゲーム起動時に反映されます。",L"Game aspect and window size are independent; unused areas stay black.\nFixed Aspect Ratio preserves the chosen window shape when resizing.\nSaved settings apply on the next game launch."},
    {L"导入或更新语言 ISO…",L"言語 ISO を追加・更新…",L"Import / update language ISO…"},
    {L"保存",L"保存",L"Save"}, {L"取消",L"キャンセル",L"Cancel"},
    {L"自动（Vulkan → OpenGL → D3D11）",L"自動（Vulkan → OpenGL → D3D11）",L"Auto (Vulkan → OpenGL → D3D11)"},
    {L"16:9（宽屏）",L"16:9（ワイド）",L"16:9 (Widescreen)"},
    {L"21:9（超宽屏）",L"21:9（ウルトラワイド）",L"21:9 (Ultrawide)"},
    {L"导入或更新语言",L"言語の追加・更新",L"Import / update language"},
    {L"语言标识",L"言語 ID",L"Language ID"}, {L"显示名称",L"表示名",L"Display name"},
    {L"更新已有语言时，保留相同标识。",L"更新する場合は同じ ID を使用してください。",L"Use the same ID when updating an existing language."},
    {L"选择 ISO…",L"ISO を選択…",L"Choose ISO…"},
    {L"请输入新的语言标识和显示名称。标识须以字母开头，只含字母、数字、下划线或连字符。",L"新しい言語 ID と表示名を入力してください。ID は英字で始め、英数字・_・- のみ使用できます。",L"Enter a new language ID and display name. The ID must start with a letter and contain only ASCII letters, digits, _ or -."},
    {L"无法打开语言名称窗口。",L"言語名のウィンドウを開けません。",L"Could not open the language name dialog."},
    {L"无法启动语言导入工具。",L"言語インポートツールを起動できません。",L"Could not start the language importer."},
    {L"正在提取素材并构建原生语言包，请稍候……",L"素材を展開し、ネイティブ言語パックを構築しています…",L"Extracting assets and building the native language package…"},
    {L"导入完成。选择语言后点“保存”，下次启动生效。",L"追加が完了しました。言語を選んで保存すると次回起動時に反映されます。",L"Import complete. Select the game language and save to use it on the next launch."},
    {L"导入失败，原有语言包保持。",L"インポートに失敗しました。既存の言語パックは保持されています。",L"Import failed. The existing language package is unchanged."},
    {L"可导入翻译后的 ISO；更新时使用相同语言标识。",L"翻訳済み ISO を選択してください。更新時は同じ言語 ID を使用します。",L"Import a translated ISO. Keep the same language ID for updates."},
    {L"导入失败，详情见 runtime/languages 下的导入日志。",L"インポートに失敗しました。runtime/languages 内のログを確認してください。",L"Import failed. See the import log under runtime/languages."},
    {L"窗口尺寸必须为整数。",L"ウィンドウサイズは整数で入力してください。",L"Window dimensions must be integers."},
    {L"操作说明",L"操作説明",L"Controls"},
    {L"双击游戏画面：切换无边框全屏；再次双击恢复窗口位置和尺寸。\r\n\r\nF8：保存当前游戏画面（BMP），位于 pc_recomp/runtime/Screenshots。按游戏画面比例和渲染高度保存，不含标题栏。\r\n\r\nF9：暂停／继续游戏，同时暂停游戏计时和声音。暂停时仍可截图和切换全屏。\r\n\r\nF1–F5：读取即时档。左 Shift + F1–F5：保存即时档。暂停时请先按 F9 继续。\r\n\r\n右 Shift：游戏 Start。\r\n\r\n手柄 LB/RB：游戏 L/R；LT/RT：白/黑。\r\n键盘 Q/E：L/R；2/1：白/黑。\r\n\r\n快捷键在游戏窗口获得焦点时使用。界面语言与游戏语言独立；顶部语言切换立即更新本说明，保存后保留设置。",
     L"ゲーム画面をダブルクリック：ボーダーレス全画面に切り替え。もう一度ダブルクリックすると元の位置とサイズに戻ります。\r\n\r\nF8：現在のゲーム画面を BMP で保存します。保存先：pc_recomp/runtime/Screenshots。ゲームの画面比率と描画高さで保存し、タイトルバーは含みません。\r\n\r\nF9：一時停止／再開。ゲームの時間と音声も停止します。停止中も撮影と全画面切り替えができます。\r\n\r\nF1–F5：ステート読込。左 Shift + F1–F5：ステート保存。停止中は F9 で再開してから操作してください。\r\n\r\n右 Shift：ゲームの Start。\r\n\r\nパッド LB/RB：ゲームの L/R、LT/RT：白/黒。\r\nキーボード Q/E：L/R、2/1：白/黒。\r\n\r\nショートカットはゲームウィンドウがアクティブな時に使用します。表示言語とゲーム言語は独立しています。上部で言語を切り替えると説明も変わり、保存すると設定が保持されます。",
     L"Double-click the game picture to toggle borderless fullscreen. Double-click again to restore the window's position and size.\r\n\r\nF8: Save the current game picture as BMP in pc_recomp/runtime/Screenshots, using the game aspect ratio and render height without the title bar.\r\n\r\nF9: Pause / resume the game, including its clock and audio. Screenshots and fullscreen switching remain available while paused.\r\n\r\nF1–F5: Load instant states. Left Shift + F1–F5: Save instant states. Resume with F9 before using states.\r\n\r\nRight Shift: Game Start.\r\n\r\nController LB/RB: game L/R; LT/RT: White/Black.\r\nKeyboard Q/E: L/R; 2/1: White/Black.\r\n\r\nShortcuts work while the game window is focused. Interface and game languages are independent. The selector at the top updates this help immediately; Save keeps the choice."},
    {L"关闭",L"閉じる",L"Close"},
    {L"自定义尺寸",L"カスタムサイズ",L"Custom size"},
    {L"固定窗口宽高比（Fixed Aspect Ratio）",L"ウィンドウの縦横比を固定",L"Fixed Aspect Ratio (when resizing)"}
};
