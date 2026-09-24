#pragma once
// ---------------------------------------------------------------------------
// 资源 ID 段（《技术实现设计》§3.3）
//   1000-1999 分组名 | 2000-2999 命令标题(2000 + id-1000)
//   3000-3999 参数/复选标签 | 4000-4999 状态与错误消息
//   5000-5099 按钮 | 5100-5199 窗口标题 | 6000-6999 工具提示
// 供 C++ 与 windres 共用（windres --codepage=65001 读取本文件）
// ---------------------------------------------------------------------------

// ------------------------------------------------------------------ 分组名
#define IDS_GROUP_REPO      1000
#define IDS_GROUP_COMMIT    1001
#define IDS_GROUP_SYNC      1002
#define IDS_GROUP_BRANCH    1003
#define IDS_GROUP_INSPECT   1004
#define IDS_GROUP_STASH     1005
#define IDS_GROUP_ADVANCED  1006
#define IDS_GROUP_APP       1007

// -------------------------------------------------------------- 命令标题
#define IDS_CMD_REPO_INIT          2001
#define IDS_CMD_REPO_CLONE         2002
#define IDS_CMD_APP_CONSOLE        2003
#define IDS_CMD_APP_TERMINAL       2004
#define IDS_CMD_COMMIT_STAGE       2101
#define IDS_CMD_COMMIT_UNSTAGE     2102
#define IDS_CMD_COMMIT_DISCARD     2103
#define IDS_CMD_COMMIT_IGNORE      2104
#define IDS_CMD_COMMIT_COMMIT      2105
#define IDS_CMD_COMMIT_AMEND       2106
#define IDS_CMD_COMMIT_UNDO        2107
#define IDS_CMD_SYNC_PULL          2201
#define IDS_CMD_SYNC_PUSH          2202
#define IDS_CMD_SYNC_FETCH         2203
#define IDS_CMD_SYNC_SYNC          2204
#define IDS_CMD_BRANCH_SWITCH      2301
#define IDS_CMD_BRANCH_NEW         2302
#define IDS_CMD_BRANCH_MERGE       2303
#define IDS_CMD_BRANCH_REBASE      2304
#define IDS_CMD_BRANCH_DELETE      2306
#define IDS_CMD_INSPECT_LOG        2401
#define IDS_CMD_INSPECT_DIFF       2402
#define IDS_CMD_INSPECT_FILELOG    2403
#define IDS_CMD_INSPECT_STATUS     2405
#define IDS_CMD_STASH_PUSH         2501
#define IDS_CMD_STASH_POP          2503
#define IDS_CMD_ADV_CLEAN          2601
#define IDS_CMD_ADV_RESET          2602
#define IDS_CMD_ADV_MAINTENANCE    2607
#define IDS_CMD_APP_SETTINGS       2701
#define IDS_CMD_APP_DOCTOR         2702
#define IDS_CMD_APP_AI             2703

// -------------------------------------------------------- 参数 / 复选标签
#define IDS_FLAG_AMEND             3001
#define IDS_FLAG_SIGNOFF           3002
#define IDS_FLAG_NO_VERIFY         3003
#define IDS_FLAG_INCLUDE_UNTRACKED 3004
#define IDS_FLAG_KEEP_INDEX        3005
#define IDS_FLAG_INDEX             3006
#define IDS_FLAG_REBASE            3007
#define IDS_FLAG_FF_ONLY           3008
#define IDS_FLAG_PRUNE             3009
#define IDS_FLAG_AUTOSTASH         3010
#define IDS_FLAG_TAGS              3011
#define IDS_FLAG_SET_UPSTREAM      3012
#define IDS_FLAG_DRY_RUN           3013
#define IDS_FLAG_FORCE_WITH_LEASE  3014
#define IDS_FLAG_ALL               3015
#define IDS_FLAG_NO_FF             3016
#define IDS_FLAG_SQUASH            3017
#define IDS_FLAG_INTERACTIVE       3018
#define IDS_FLAG_FORCE_DELETE      3019
#define IDS_FLAG_INTENT_TO_ADD     3020
#define IDS_FLAG_RECURSE_SUBMODULE 3021
#define IDS_FLAG_STAGED            3022
#define IDS_FLAG_STAT              3023
#define IDS_FLAG_NAME_ONLY         3024
#define IDS_FLAG_CLEAN_DIRS        3025
#define IDS_FLAG_CLEAN_IGNORED     3026
#define IDS_FLAG_RESET_SOFT        3027
#define IDS_FLAG_RESET_MIXED       3028
#define IDS_FLAG_RESET_HARD        3029
#define IDS_FLAG_AGGRESSIVE        3030
#define IDS_FLAG_BRANCH_VALUE      3031
#define IDS_FLAG_TRACK_VALUE       3032
#define IDS_TERM_GIT_BASH          3040
#define IDS_TERM_POWERSHELL        3041
#define IDS_TERM_WT                3042
#define IDS_TERM_CMD               3043

// ------------------------------------------------------------ 参数提示文字
#define IDS_PARAM_URL              3100
#define IDS_PARAM_MSG              3101
#define IDS_PARAM_BRANCH           3102
#define IDS_PARAM_REMOTE           3103
#define IDS_PARAM_REVISION         3104
#define IDS_PARAM_TAG              3105
#define IDS_PARAM_PATTERN          3106

// -------------------------------------------------------- 状态 / 错误消息
#define IDS_MSG_READY              4000
#define IDS_MSG_NO_REPO            4001
#define IDS_MSG_GIT_NOT_FOUND      4002
#define IDS_MSG_CLEAN              4003
#define IDS_MSG_LOADING            4004
#define IDS_MSG_NOT_IMPLEMENTED    4005
#define IDS_MSG_BUILD_FAILED       4006
#define IDS_MSG_NO_CHANGES         4007
#define IDS_MSG_IGNORE_ADDED       4008
#define IDS_MSG_IGNORE_EXISTS      4009
#define IDS_MSG_DANGER_HINT        4010
#define IDS_MSG_DRYRUN_HINT        4011
#define IDS_MSG_RUNNING            4012
#define IDS_MSG_DONE               4013
#define IDS_MSG_FAILED             4014
#define IDS_MSG_CANCELLED          4015
#define IDS_MSG_NEED_REPO          4016
#define IDS_MSG_NEED_SELECTION     4017
#define IDS_MSG_PREVIEW_NOTE       4018
#define IDS_MSG_BRANCH_INVALID     4019
#define IDS_MSG_URL_INVALID        4020
#define IDS_MSG_REV_INVALID        4021
#define IDS_MSG_PARAM_REQUIRED     4022
#define IDS_MSG_CANCELLING         4023
#define IDS_MSG_LOG_TRUNCATED      4024
#define IDS_MSG_INTERNAL_NOTE      4025
#define IDS_MSG_ENTRIES            4026
#define IDS_MSG_DETACHED           4027
#define IDS_MSG_UP_TO_DATE         4028
#define IDS_MSG_COPIED             4029

// ------------------------------------------------------------ 列表 / 操作
#define IDS_COL_STATUS  5300
#define IDS_COL_FILE    5301
#define IDS_BTN_STAGE   5302
#define IDS_BTN_UNSTAGE 5303
#define IDS_BTN_DISCARD 5304
#define IDS_BTN_DIFF    5305

// ------------------------------------------------------------ 状态摘要
#define IDS_SUM_STAGED     5400
#define IDS_SUM_MODIFIED   5401
#define IDS_SUM_UNTRACKED  5402
#define IDS_SUM_CONFLICTED 5403

// ------------------------------------------------------------------- 按钮
#define IDS_BTN_EXECUTE            5000
#define IDS_BTN_CANCEL             5001
#define IDS_BTN_COPY               5002
#define IDS_BTN_CLOSE              5003
#define IDS_BTN_CONFIRM_DANGER     5004
#define IDS_BTN_REFRESH            5005
#define IDS_BTN_PICK_REPO          5006
#define IDS_BTN_OPEN_FOLDER        5007
#define IDS_BTN_CLEAR              5008

// ------------------------------------------------------------- 窗口标题
#define IDS_TITLE_MAIN             5100
#define IDS_TITLE_PROGRESS         5101
#define IDS_TITLE_DIFF             5102
#define IDS_TITLE_DOCTOR           5103
#define IDS_TITLE_LOG              5104
#define IDS_TITLE_SETTINGS         5105
#define IDS_TITLE_PICK_REPO        5110
#define IDS_TITLE_AI               5120

// ------------------------------------------------------------------ AI 助手
#define IDS_AI_LABEL_PROMPT    5500
#define IDS_AI_LABEL_PLAN      5501
#define IDS_AI_LABEL_RESULT    5502
#define IDS_AI_LABEL_CONFIG    5503
#define IDS_AI_LABEL_DETAILS   5504
#define IDS_AI_PROMPT_HINT     5505
#define IDS_BTN_AI_GENERATE    5510
#define IDS_BTN_AI_EXECUTE     5511
#define IDS_MSG_AI_EMPTY       4100
#define IDS_MSG_AI_NO_KEY      4101
#define IDS_MSG_AI_GENERATING  4102
#define IDS_MSG_AI_NONE        4103
#define IDS_MSG_AI_REJECTED    4104
#define IDS_MSG_AI_READY       4105
#define IDS_MSG_AI_EXECUTING   4106

// ------------------------------------------------ 右键菜单（Shell DLL，§4.1）
#define IDS_MENU_ROOT              6100
#define IDS_MENU_MORE              6101
#define IDS_MENU_NO_GIT            6102
#define IDS_MENU_IN_PROGRESS       6103
#define IDS_MENU_STATE_UNKNOWN     6104

// ------------------------------------------------------- 菜单项工具提示
#define IDS_TIP_ROOT               6150
#define IDS_TIP_FLAG               6151
#define IDS_TIP_GROUP              6152
#define IDS_TIP_NEED_REPO          6153
#define IDS_TIP_NEED_SELECTION     6154
#define IDS_TIP_INTERNAL           6155
#define IDS_TIP_ROOT_APP           6156   // App 形态（单一入口）：入口即"打开 GitRT"

// ------------------------------------------------- 菜单请求转发（§4.5/§5.4）
#define IDS_MSG_REQ_MISSING        4031
#define IDS_MSG_REQ_FORWARDED      4032

// --------------------------------------------------------------- 标签文字
#define IDS_LABEL_COMMAND_LINE     5200
#define IDS_LABEL_FLAGS            5201
#define IDS_LABEL_REPO             5202
#define IDS_LABEL_STATUS           5203
#define IDS_LABEL_COMMANDS         5204
#define IDS_LABEL_OUTPUT           5205
#define IDS_LABEL_BRANCH           5206
#define IDS_LABEL_AHEAD_BEHIND     5207
#define IDS_LABEL_NO_REPO_HINT     5208
