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
#define IDS_CMD_COMMIT_SQUASH      2108
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
// ---- 克隆选项（§4.2b 后续补充：深度等）
#define IDS_FLAG_CLONE_DEPTH       3033   // 浅克隆深度 --depth=<n>
#define IDS_FLAG_CLONE_SINGLE      3034   // 只克隆单分支 --single-branch
#define IDS_FLAG_CLONE_BRANCH      3035   // 指定分支 --branch=<name>
#define IDS_FLAG_CLONE_NO_TAGS     3036   // 不拉取标签 --no-tags
#define IDS_FLAG_CLONE_FILTER      3037   // 部分克隆 --filter=<spec>
// ---- 标签 / 发布
#define IDS_FLAG_TAG_ANNOTATED     3044   // 附注标签 (-a -m)
#define IDS_FLAG_TAG_FORCE         3045   // 覆盖同名标签 (-f)【危险】
#define IDS_FLAG_TAG_TARGET        3046   // 目标修订 --target=<rev>
#define IDS_FLAG_PUSH_ALL_TAGS     3047   // 推送全部标签 (--tags)
#define IDS_FLAG_REMOTE_VALUE      3048   // 远端名（值）
#define IDS_FLAG_DELETE_REMOTE     3049   // 同时删除远端标签
#define IDS_FLAG_RELEASE_TITLE     3050   // 发布标题 --title
#define IDS_FLAG_RELEASE_NOTES     3051   // 发布说明 --notes
#define IDS_FLAG_GENERATE_NOTES    3052   // 自动生成说明 --generate-notes
#define IDS_FLAG_DRAFT             3053   // 草稿 --draft
#define IDS_FLAG_PRERELEASE        3054   // 预发布 --prerelease
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
#define IDS_BTN_SQ_MERGE           5009   // 合并所选提交

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
// AI 设置对话框（首选项，2026-09-24：Key 明文存 exe 同目录的 GitRT.ai.json）
// ---- 合并提交（squash）窗口
#define IDS_TITLE_SQUASH        5122
#define IDS_SQ_COL_SUBJECT      5310
#define IDS_SQ_COL_HASH         5311
#define IDS_SQ_COL_DATE         5312
#define IDS_SQ_COL_AUTHOR       5313
#define IDS_SQ_LABEL_MSG        5314
#define IDS_SQ_LABEL_LOG        5315
#define IDS_SQ_SEL_NONE         5316
#define IDS_SQ_SEL_OK           5317   // 已选 N 条：连续
#define IDS_SQ_SEL_GAP          5318
#define IDS_SQ_SEL_MERGE        5319
#define IDS_SQ_CONFIRM_TITLE    5320
#define IDS_SQ_CONFIRM_BODY     5321
#define IDS_SQ_DONE             5322
#define IDS_SQ_RUNNING          5323
#define IDS_SQ_FAILED           5324
#define IDS_SQ_NEED2            5325
#define IDS_SQ_NO_COMMITS       5326
// ---- 查看类命令的"自动显示"预览（提交历史/查看差异/文件历史）
#define IDS_LABEL_CONTENT       5327   // 内容（自动刷新）
#define IDS_MSG_PREVIEW_FOOTER  5328
#define IDS_LABEL_RUN_OUTPUT    5329
// ---- AI 系统提示词（首选项里可改）
#define IDS_AI_SET_LABEL_SYSPROMPT     5330
#define IDS_AI_SET_PROMPT_DEFAULT_HINT 5331
#define IDS_BTN_AI_RESET_PROMPT        5332
// ---- 自动抓取（首选项里的"持续跟踪"）
#define IDS_AI_SET_LABEL_AUTOFETCH     5334
#define IDS_AI_SET_AUTOFETCH_HINT      5335

// ---- 标签与发布（tag.* / release.*）
#define IDS_CMD_TAG_LIST         2307
#define IDS_CMD_TAG_CREATE       2308
#define IDS_CMD_TAG_PUSH         2309
#define IDS_CMD_TAG_DELETE       2310
#define IDS_CMD_RELEASE_LIST     2311
#define IDS_CMD_RELEASE_CREATE   2312
// 标签/发布面板用的提示串
#define IDS_TAG_PANEL_HINT       5660   // 「标签」面板顶部的说明
#define IDS_TAG_EMPTY            5661   // 一个标签都没有
#define IDS_TAG_NEED_NAME        5662   // 先填标签名
#define IDS_TAG_ANNOTATED_HINT   5663   // 附注标签说明
#define IDS_TAG_FORCE_HINT       5664   // 覆盖同名标签说明
#define IDS_TAG_PUSH_ALL_HINT    5665   // --tags 会推全部
#define IDS_TAG_DELETE_WARN      5666   // 删除标签的告警
#define IDS_RELEASE_NEED_GH      5667   // 没装 gh
#define IDS_RELEASE_NEED_TAG     5668   // 标签必须先存在
#define IDS_RELEASE_DRAFT_HINT   5669   // 草稿说明
#define IDS_RELEASE_PUBLIC_HINT  5670   // 公开可见

// ---- 标签窗口（阶段二：src/gui/tag_window.cpp，专用 GUI 窗口）  5671-5711
#define IDS_TITLE_TAG            5671   // 窗口标题
#define IDS_TG_LBL_LIST          5672   // 列表上方说明
#define IDS_TG_COL_NAME          5673
#define IDS_TG_COL_HASH          5674
#define IDS_TG_COL_TYPE          5675
#define IDS_TG_COL_DATE          5676
#define IDS_TG_COL_SUBJECT       5677
#define IDS_TG_COL_REMOTE        5678
#define IDS_TG_TYPE_ANNOTATED    5679   // 类型列：附注
#define IDS_TG_TYPE_LIGHT        5680   // 类型列：轻量
#define IDS_TG_REMOTE_YES        5681   // 远端列：有
#define IDS_TG_REMOTE_NO         5682   // 远端列：无
#define IDS_TG_LBL_CREATE        5683
#define IDS_TG_LBL_NAME          5684
#define IDS_TG_LBL_TARGET        5685
#define IDS_TG_LBL_MESSAGE       5686
#define IDS_TG_ANNOTATED         5687   // 复选：附注标签
#define IDS_TG_FORCE             5688   // 复选：覆盖同名
#define IDS_TG_LBL_PUSH          5689
#define IDS_TG_PUSH_ALL          5690   // 复选：推送全部
#define IDS_TG_LBL_DELETE        5691
#define IDS_TG_DEL_LOCAL         5692   // 复选：删本地
#define IDS_TG_LBL_REMOTE        5693   // 远端名（值）
#define IDS_TG_DEL_REMOTE        5694   // 复选：也删远端
#define IDS_TG_BTN_CREATE        5695
#define IDS_TG_BTN_PUSH          5696
#define IDS_TG_BTN_DELETE        5697
#define IDS_TG_SEL_NONE          5698
#define IDS_TG_SEL_OK            5699   // {name} / {hash} / {kind}
#define IDS_TG_RUNNING           5700
#define IDS_TG_DONE              5701
#define IDS_TG_FAILED            5702   // {msg}
#define IDS_TG_NO_TAGS           5703
#define IDS_TG_LBL_LOG           5704
#define IDS_TG_CONFIRM_TITLE     5705
#define IDS_TG_CONFIRM_BODY      5706   // {cmds} / {warns}
#define IDS_TG_DEL_CONFIRM_TITLE 5707
#define IDS_TG_DEL_CONFIRM_BODY  5708   // {name} / {cmds}
#define IDS_TG_NEED_LOCAL        5709   // 只删远端不被 core 支持
#define IDS_TG_NEED_ONE          5710
#define IDS_TG_CREATED_OK        5711   // {name}
#define IDS_TG_PLAN_OK           5712   // 计划已生成（预览框里就是要执行的命令）

// ---- 发布窗口（阶段二：src/gui/release_window.cpp）  5720-5747
#define IDS_TITLE_RELEASE        5720
#define IDS_REL_LBL_LIST         5721
#define IDS_REL_COL_TAG          5722
#define IDS_REL_COL_NAME         5723
#define IDS_REL_COL_DATE         5724
#define IDS_REL_COL_FLAGS        5725
#define IDS_REL_DRAFT_TAG        5726   // 状态列：草稿
#define IDS_REL_PRE_TAG          5727   // 状态列：预发布
#define IDS_REL_LBL_CREATE       5728
#define IDS_REL_LBL_TAG          5729
#define IDS_REL_LBL_TITLE        5730
#define IDS_REL_LBL_NOTES        5731
#define IDS_REL_GENERATE         5732   // 复选：--generate-notes
#define IDS_REL_DRAFT_CHK        5733   // 复选：--draft
#define IDS_REL_PRE_CHK          5734   // 复选：--prerelease
#define IDS_REL_LBL_ASSETS       5735
#define IDS_REL_BTN_PICK         5736   // 选择文件…
#define IDS_REL_BTN_REMOVE       5737   // 移除所选
#define IDS_REL_LBL_LOG          5738
#define IDS_REL_BTN_CREATE       5739
#define IDS_REL_SEL_NONE         5740
#define IDS_REL_SEL_OK           5741   // {tag}
#define IDS_REL_RUNNING          5742
#define IDS_REL_DONE             5743
#define IDS_REL_FAILED           5744   // {msg}
#define IDS_REL_NO_RELEASES      5745
#define IDS_REL_CONFIRM_TITLE    5746
#define IDS_REL_CONFIRM_BODY     5747   // {cmds} / {warns}

// ---- 还原到提交（history.restore）
#define IDS_CMD_HISTORY_RESTORE  2109
#define IDS_TITLE_RESTORE        5600
#define IDS_RST_LABEL_MODE       5601
#define IDS_RST_MODE_DETACH      5602
#define IDS_RST_MODE_BRANCH      5603
#define IDS_RST_MODE_SOFT        5604
#define IDS_RST_MODE_MIXED       5605
#define IDS_RST_MODE_HARD        5606
#define IDS_RST_LBL_BRANCH       5607
#define IDS_RST_LBL_COMMITS      5608
#define IDS_RST_LBL_LOG          5609
#define IDS_BTN_RST_DO           5610
#define IDS_BTN_RST_REFRESH      5611
#define IDS_RST_SEL_NONE         5612
#define IDS_RST_SEL_OK           5613
#define IDS_RST_CONFIRM_TITLE    5614
#define IDS_RST_CONFIRM_BODY     5615
#define IDS_RST_HARD_TITLE       5616
#define IDS_RST_HARD_BODY        5617
#define IDS_RST_RUNNING          5618
#define IDS_RST_DONE             5619
#define IDS_RST_FAILED           5620
#define IDS_RST_NEED_COMMIT      5621
#define IDS_RST_NO_COMMITS       5622

// ---- 远端分支与远端地址（remote.panel）
#define IDS_CMD_REMOTE_PANEL     2110
#define IDS_TITLE_REMOTE         5630
#define IDS_RM_LBL_BRANCHES      5631
#define IDS_RM_LBL_REMOTES       5632
#define IDS_RM_LBL_NAME          5633
#define IDS_RM_LBL_URL           5634
#define IDS_RM_LBL_LOG           5635
#define IDS_BTN_RM_FETCH         5636
#define IDS_BTN_RM_CHECKOUT      5637
#define IDS_BTN_RM_TRACK         5638
#define IDS_BTN_RM_UPSTREAM      5639
#define IDS_BTN_RM_SAVE_URL      5640
#define IDS_BTN_RM_ADD_REMOTE    5641
#define IDS_RM_SEL_NONE          5642
#define IDS_RM_SEL_OK            5643
#define IDS_RM_RUNNING           5644
#define IDS_RM_DONE              5645
#define IDS_RM_FAILED            5646
#define IDS_RM_NEED_BRANCH       5647
#define IDS_RM_NO_BRANCHES       5648
#define IDS_RM_URL_OK            5649
#define IDS_RM_ADD_OK            5650
#define IDS_RM_CONFIRM_TRACK     5651
#define IDS_RM_CONFIRM_TITLE     5652
#define IDS_CMD_SYNC_FETCH_ALL   2111
#define IDS_MSG_NO_UPSTREAM      5653   // 未设置上游
#define IDS_BTN_SV_FETCH         5654   // 状态视图里的「抓取」   // 运行结果（执行时在面板里就地显示）   // …（只显示前 N 行，点「执行」看完整内容）

#define IDS_BTN_AI_SAVE        5512
#define IDS_BTN_AI_TEST        5513
#define IDS_BTN_AI_MODELS      5514
#define IDS_AI_SET_LABEL_ENDPOINT 5520
#define IDS_AI_SET_LABEL_MODEL    5521
#define IDS_AI_SET_LABEL_KEY      5522
#define IDS_AI_SET_LABEL_TIMEOUT  5523
#define IDS_AI_SET_LABEL_STORE    5524
#define IDS_AI_SET_HINT_KEY       5525
#define IDS_MSG_AI_SAVED       4107
#define IDS_MSG_AI_SAVE_FAILED 4108
#define IDS_MSG_AI_TESTING     4109
#define IDS_MSG_AI_TEST_OK     4110
#define IDS_MSG_AI_TEST_FAIL   4111
#define IDS_MSG_AI_KEY_FROM_FILE 4112
#define IDS_MSG_AI_KEY_FROM_ENV  4113
#define IDS_MSG_AI_KEY_NONE2     4114
#define IDS_MSG_AI_STORE_READONLY 4115
#define IDS_MSG_AI_MODELS_OK      4116
#define IDS_MSG_AI_MODELS_FAIL    4117
#define IDS_MSG_AI_MODELS_LOADING 4118
#define IDS_MSG_AI_MODELS_EMPTY   4119
#define IDS_TITLE_AI_SETTINGS  5121

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

// ------------------------------------------------- 图标（GUI 与 Shell 共用同一对 .ico）
//   exe / dll 各自内嵌同一份图片，右键菜单用 ",-101"，任务栏用 exe 的资源 101
#define IDI_GRT_APP    101
#define IDI_GRT_WARN   102

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
