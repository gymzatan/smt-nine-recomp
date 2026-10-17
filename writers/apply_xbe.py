# -*- coding: utf-8 -*-
"""
SMT NINE XBE文件汉化写入工具

将翻译后的中文写回XBE文件
使用 sysfont_cn.tbl 码表进行编码
"""

import json
import re
import struct
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Set
import io
import hashlib

from fullwidth_ascii import (
    normalize_visible_ascii_like_reference,
    normalize_format_tokens_halfwidth,
    to_fullwidth_char,
    to_halfwidth_char,
)
from xbe_percent_marks import apply_percent_marks, extract_percent_marks

# 配置
SCRIPT_DIR = Path(__file__).parent
TRANSLATED_DIR = SCRIPT_DIR / "translated"
OUTPUT_DIR = SCRIPT_DIR / "output"
BACKUP_XBE_PATH = SCRIPT_DIR.parent / "backup" / "default.xbe"
TBL_CANDIDATES = [
    SCRIPT_DIR.parent / "SMTNINE_f24_clean_script" / "final_game_f24_full.tbl",
    SCRIPT_DIR.parent / "SMTNINE_f24_clean_script" / "final_game_f24.tbl",
    SCRIPT_DIR.parent / "SMTNINE_f24_clean_script" / "sysfont_cn.tbl",
]

OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

# Only allow writing to data sections
SAFE_SECTIONS = {'.rdata', '.data'}
REQUIRED_CODE_PATCHES = {
    0x102B31: b"\x09",
    0x140CB2: b"\x0c",
    0x1412AE: b"\xfc\xfc",
}
FONT_SAMPLE_RANGE_OFFSET = 0x141657

def apply_font_sample_range(data: bytearray) -> None:
    """Original font quad: sample glyph extent, without the extra texel."""
    offset = FONT_SAMPLE_RANGE_OFFSET
    if data[offset:offset + 2] not in (b'\xd9\xe8', b'\xd9\xee'):
        raise ValueError('Original font UV producer differs at VA 0x151657')
    data[offset:offset + 2] = b'\xd9\xee'

def refresh_section_digests(data: bytearray) -> None:
    base = struct.unpack_from('<I', data, 0x104)[0]
    count, table = struct.unpack_from('<II', data, 0x11c)
    for index in range(count):
        header = table - base + index * 56
        offset, size = struct.unpack_from('<II', data, header + 12)
        if offset + size > len(data):
            raise ValueError('XBE section extends beyond the file')
        data[header + 36:header + 56] = hashlib.sha1(
            struct.pack('<I', size) + data[offset:offset + size]).digest()
INVALID_TRANSLATION_PREFIXES = (
    "检查：",
    "**Maintaining ",
    "**Matching ",
)
INVALID_TRANSLATION_SUBSTRINGS = (
    "翻译一致 ✓",
    "准确 ✓",
    "日文正确",
)
SYMBOL_NORMALIZATION = {
    "·": "・",
    "－": "ー",
    "—": "ー",
    "～": "〜",
    "~": "〜",
    "Ⅰ": "Ｉ",
    "Ⅱ": "ＩＩ",
    "Ⅲ": "ＩＩＩ",
    "Ⅳ": "ＩＶ",
    "é": "e",
    "ā": "a",
    "ū": "u",
    "호": "ho",
}
ORIGINAL_GLYPH_BYTES = {
    # These three symbols already exist in the original script/font data.
    # Write them back with their original raw bytes instead of forcing new TBL entries.
    "∮": bytes.fromhex("8793"),
    "∟": bytes.fromhex("8798"),
    "⊿": bytes.fromhex("8799"),
}
FORMAT_TOKEN_RE = re.compile(r'%(?:[-+#0 ]*\d*(?:\.\d+)?)?(?:[A-Za-z]|\d+)')
STRUCTURED_PREFIX_RE = re.compile(r"^(?P<prefix>(?:[%0-9A-Za-z]C)|(?:[%0-9A-Za-z]\x01))(?P<body>.+)$")
OPAQUE_PREFIX_OVERRIDES = {
    "漾LABEL:%s": (bytes.fromhex("E040"), "LABEL:%s"),
}
LEADING_OPAQUE_PREFIX_CHARS = frozenset({"⊇", "⊂", "⊆", "⊿", "∮", "∟"})
XBE_TRANSLATION_OVERRIDES = {
    '回復屋': '回复屋',
    '道具屋': '道具屋',
    '防具屋': '防具屋',
    '武器屋': '武器屋',
    '蓄積所Ａ': '蓄积所A',
    '蓄積所Ｂ': '蓄积所B',
    '道玄坂': '道玄坂',
    '参道': '参道',
    '地下%d階': '地下%d层',
    '地上%d階': '地上%d层',
    '表参道交差点方面': '表参道路口',
    '六本木交差点': '六本木路口',
    '六本木交差点方面': '六本木路口',
    '平行光源設定': '平行光源设置',
    '点光源設定': '点光源设置',
    '詳細': '详情',
    '完了': '完成',
    'ケイ': '景',
    'アズマ': '东',
    '（悪魔名）': '（恶魔名）',
    '（種族名）': '（种族名）',
    '（大種族名）': '（大种族名）',
    '悪魔合体': '恶魔合体',
    '【悪魔合体】': '【恶魔合体】',
    'バトル－ＡＩ': '战斗－ＡＩ',
    '「－」　合体できない悪魔を表します': '「－」　表示无法合体的恶魔',
    'Ｍ．ティーパーティ': '疯狂茶会',
    'クー・フーリンの好敵手、帝王フェルグス愛用の魔剣': '库夫林的宿敌、帝王弗格斯钟爱的魔剑',
    '伝説の書物「ド・ジャンの書」に記されている魔力兵器': '记载于传奇书籍《德·让之书》中的魔力兵器',
    '支配人室': '经理室',
    '地下街予定地': '地下街预定地',
    '無銘滅殺': '无铭灭杀',
    '無銘夢想': '无铭梦想',
    '無銘毒刃': '无铭毒刃',
    '無銘神焔': '无铭神焰',
    '　しかし一見平和に見える仮想': '然而，看似和平的虚拟空间中',
    '　空間にはノイズと呼ばれるコンピュータのバグが発生し、': '却出现了名为“噪声”的电脑漏洞，',
    '　人々の安全を脅かしていた。': '威胁着人们的安全。',
    '　リトルハートは、このノイズ': '“小心脏”于是组建了',
    '　除去を中心に仮想空間におけ': '以清除噪声为主，',
    '　る様々な問題を解決する集団': '在虚拟空间中',
    '　「デバッカー」（仮）を組織': '解决各种问题的团体',
    '　し、安全を確保しようと努め': '“除骇者”（暂定名），',
    '　るようになる。': '并努力维护安全。',
    '身代わり': '替身',
    '未配置': '未配置',
    '運': '运',
    '力': '力',
    '石': '石',
    '魔': '魔',
    '瀕': '濒',
    '死': '死',
    '殖': '殖',
    '毒': '毒',
    '吸': '吸',
    '反': '反',
    '率': '率',
    '銃': '枪',
    '剣': '剑',
    '技': '技',
    '人': '人',
    '男': '男',
    '女': '女',
    '魔封': '魔封',
    '石化': '石化',
    '物理': '物理',
    '防御力': '防御力',
    '攻撃力': '攻击力',
    '破壊????': '破坏',
    '構築????': '构筑',
    '容量????／????': '容量',
    '速度': '速度',
    '暗号化': '加密',
    '剣合体': '剑合体',
    '剣攻': '剑攻',
    '魔率': '魔率',
    '魔攻': '魔攻',
    '物回': '物回',
    '物防': '物防',
    '出撃中': '出击中',
    '切替': '切换',
    '個別': '个别',
    '前衛': '前卫',
    '後衛': '后卫',
    '前衛　右端': '前卫　最右',
    '前衛　右': '前卫　右',
    '前衛　左': '前卫　左',
    '前衛　左端': '前卫　最左',
    '後衛　右端': '后卫　最右',
    '後衛　右': '后卫　右',
    '後衛　左': '后卫　左',
    '後衛　左端': '后卫　最左',
    '遊撃': '游击',
    '工作': '工作',
    '汎用': '泛用',
    '消費　⊂%4d': '消耗　⊂%4d',
    '出撃不可': '无法出击',
    '合計ＣＰ': '总ＣＰ',
    '破魔': '破魔',
    '氷結': '冰结',
    '投具': '投具',
    '斬撃': '斩击',
    '電撃': '电击',
    '打撃': '打击',
    '邪鬼': '邪鬼',
    '妖鬼': '妖鬼',
    '地霊': '地灵',
    '邪神': '邪神',
    '国津神': '国津神',
    '地母神': '地母神',
    '破壊神': '破坏神',
    '悪霊': '恶灵',
    '精霊': '精灵',
    '鬼族': '鬼族',
    '獣族': '兽族',
    '鳥族': '鸟族',
    '飛天族': '飞天族',
    '鬼神族': '鬼神族',
    '神族': '神族',
    '聖獣': '圣兽',
    '神獣': '神兽',
    '凶鳥': '凶鸟',
    '妖鳥': '妖鸟',
    '霊鳥': '灵鸟',
    '邪龍': '邪龙',
    '龍王': '龙王',
    '龍神': '龙神',
    '属性': '属性',
    '五十音': '五十音',
    '天啓': '天启',
    '破軍星': '破军星',
    '宿曜経': '宿曜经',
    '幸運扶翼': '幸运扶翼',
    '富運再起': '富运再起',
    '空間転移': '空间转移',
    '四神相応': '四神相应',
    '敗北条件': '败北条件',
    '勝利条件': '胜利条件',
    '候補選択': '候补选择',
    '候補一覧': '候补一览',
    '変換確定': '转换确定',
    '都市選択': '城市选择',
    '頁切替': '页切换',
    '無変換': '无转换',
    '定型': '定型',
    '改行': '换行',
    '変換': '转换',
    '英数': '英数',
    '全削': '全删',
    '英小': '英小',
    '英大': '英大',
    '漢字': '汉字',
    '掲示板': '公告板',
    '「夢見針」多重、睡眠': '「梦见针」多重、睡眠',
    '「邪毒針」多重、石化': '「邪毒针」多重、石化',
    '「九十九針」多重、破魔': '「九十九针」多重、破魔',
    '氷結、呪殺、神経無効': '冰结、咒杀、神经无效',
    '破魔、呪殺、精神、神経無効': '破魔、咒杀、精神、神经无效',
    '衝撃吸収': '冲击吸收',
    '破魔、神経無効': '破魔、神经无效',
    '呪殺反射': '咒杀反射',
    '破魔反射': '破魔反射',
    '電撃、破魔無効': '电击、破魔无效',
    '氷結反射': '冰结反射',
    '打撃、斬撃、技、破魔無効': '打击、斩击、技、破魔无效',
    '破魔、呪殺、神経反射': '破魔、咒杀、神经反射',
    '破魔、呪殺反射': '破魔、咒杀反射',
    '火炎吸収': '火焰吸收',
    '電撃吸収': '电击吸收',
    '破魔、呪殺無効': '破魔、咒杀无效',
    '氷結吸収': '冰结吸收',
    '火炎反射': '火焰反射',
    '鈍足': '迟缓',
    '魔防LV半分': '魔防减半',
    '魔攻LV半分': '魔攻减半',
    '物防LV半分': '物防减半',
    '物攻LV半分': '物攻减半',
    '魔防LVDown': '魔防降级',
    '魔攻LVDown': '魔攻降级',
    '物防LVDown': '物防降级',
    '物攻LVDown': '物攻降级',
    '８連続攻撃　範囲：敵単体': '８连续攻击　范围：敌单体',
    '魔防下降': '魔防下降',
    '魔攻下降': '魔攻下降',
    '物防下降': '物防下降',
    '物攻下降': '物攻下降',
    '投射': '投射',
    '補助': '辅助',
    '感電': '感电',
    '凍結': '冻结',
    '魅了': '魅惑',
    '反魂香': '返魂香',
    '破魔無効': '破魔无效',
    '呪殺無効': '咒杀无效',
    '電撃反射': '电击反射',
    '物理倍反射': '物理倍反射',
    '衝撃倍反射': '冲击倍反射',
    '電撃倍反射': '电击倍反射',
    '氷結倍反射': '冰结倍反射',
    '火炎倍反射': '火焰倍反射',
    '破魔、呪殺、神経、精神無効': '破魔、咒杀、神经、精神无效',
    '衝撃＋物理防御下降　範囲：敵全体': '冲击＋物理防御下降　范围：敌全体',
    '読み込み': '读取',
    '読み込む': '读取',
    '保存する': '保存',
    '場所を選んで下さい。': '：请选择位置。',
    '%s場所を選んで下さい。': '%s：请选择位置。',
    '%sにアクセスできません。': '无法访问%s。',
    '%sよろしいですか？': '%s，确定吗？',
    'メモリユニット　%s': '记忆单元　%s',
    'メモリユニット%d%c': '记忆单元%d%c',
    'プレイデータ%s中に問題が発生しました。': '游戏数据%s时发生问题。',
    'メモリユニット%s%sに保存中です。': '正在保存到记忆单元%s%s。',
    'コントローラやメモリユニットを抜かないで下さい。': '请勿拔出控制器或记忆单元。',
    '保存して': '要保存',
    '読み込んで': '要读取',
    'よろしいですか？': '确定吗？',
    'プレイデータの%sが終了しました。\r\nＡボタンを押して続けて下さい。': '游戏数据%s已完成。\n请按A键继续。',
    '保存されているプレイデータがありません。': '没有已保存的游戏数据。',
    'プレイデータを読み込めません。\r\nＡボタンを押して続けてください。': '无法读取游戏数据。\n请按A键继续。',
    'プレイデータがありません。': '没有游戏数据。',
    'プレイデータを新たに作ります。': '将新建游戏数据。',
    'プレイデータを検索しています。\r\nしばらくお待ち下さい。\r\nメモリユニットやコントローラを抜かないでください。': '正在搜索游戏数据。\n请稍候。\n请勿拔出记忆单元或控制器。',
    'から読み込みます。\r\n読み込むプレイデータを選んで下さい。': '将从这里读取。\n请选择要读取的游戏数据。',
    'に保存します。\r\nプレイ時間%02d：%02d': '将保存到这里。\n游戏时间%02d：%02d',
    '%sから読み込みます。\r\n読み込むプレイデータを選んで下さい。': '将从%s读取。\n请选择要读取的游戏数据。',
    '%sに保存します。\r\nプレイ時間%02d：%02d': '将保存到%s。\n游戏时间%02d：%02d',
    'プレイ時間': '游戏时间',
    'プレイデータの保存を終了します。': '结束游戏数据保存。',
    '第%d章': '第%d章',
    '第%1d章　終了': '第%1d章　结束',
    '全仲魔装備可能': '全仲魔可装',
    '操作設定': '操作设置',
    '設定終了': '设置完成',
    '主人公': '主角',
    'リーダー': '队长',
    '%sスキル': '%s技能',
    '所持魔法': '持有魔法',
    '追加可能容量%4d': '可追加容量%4d',
    '最大容量%4d': '最大容量%4d',
    '吉祥寺（ＲＥ）': '吉祥寺（RE）',
    '渋谷１００９内部': '渋谷1009内部',
    '上野（ＲＥ）': '上野（RE）',
    '六本木（ＲＥ）': '六本木（RE）',
    '魔界（ＲＥ）': '魔界（RE）',
    '池袋（ＲＥ）': '池袋（RE）',
    '１００９　Ａ棟': '１００９　Ａ栋',
    '１００９　Ｂ棟': '１００９　Ｂ栋',
    'Ｃｏｍａ劇前': 'Ｃｏｍａ剧前',
    '%d体目の悪魔を選んで下さい': '请选择第%d体恶魔',
    '%s後の容量': '%s后的容量',
    '(容量%3d)': '(容量%3d)',
    '%s（%d）': '%s（%d）',
    '%s専用': '%s专用',
    '%sを手に入れた': '获得了%s',
    '%s%sになります': '将变成%s%s',
    '%sを\r\n%sにします': '将%s\r\n设为%s',
    '%sを\r\n%sの\r\nモジュールにします': '将%s\r\n设为%s的\r\n模块',
    '蘇生させると%sが１下がります': '若使其复活，%s会下降１点',
    '空き容量': '空余容量',
    '仲魔がいません': '没有仲魔',
    '%s隊が戦闘へ突入！': '%s队突入战斗！',
    '%s隊が強制帰還！': '%s队被强制归还！',
    '%sが戦闘不能になりました！': '%s陷入战斗不能！',
    '%s隊が援軍として参加！': '%s队作为援军参战！',
    '%s隊がトラップに接触！': '%s队触发了陷阱！',
    '%s隊がトラップを回避！': '%s队避开了陷阱！',
    '%s隊がトラップを解除！': '%s队解除了陷阱！',
    '%sを奪取！': '夺取了%s！',
    '%s隊が戦闘に勝利！': '%s队战斗胜利！',
    '%s隊が戦闘に敗北！': '%s队战斗败北！',
    '%s隊が戦闘から撤退！': '%s队从战斗中撤退！',
    '%s隊が配置されていません！': '%s队尚未配置！',
    '%s隊を配置して下さい': '请配置%s队',
    '%s隊がトラップを発見！': '%s队发现了陷阱！',
    '%s隊を配置します': '配置%s队',
    '%sのブランチを開始します': '开始%s的分支',
    'AKシリーズ同様の堅実な作動をするロシア軍で使用されている狙撃銃': '与AK系列一样作动可靠的俄军制式狙击枪',
    'UZIと同様の機構を持つSMG': '采用与UZI相同机构的SMG',
    'M16の弾倉を使用して射撃をすることもできる機関銃': '也能使用M16弹匣射击的机关枪',
    'SPASと双璧をなすタクティカルショットガン': '与SPAS齐名的战术霰弹枪',
    'アメ横方面': '阿美横丁',
    '現実世界へ': '现实世界',
    'ワークシャツ水': '工装衫水蓝',
    '正道ネルシャツ黒': '正道绒衫黑',
    '正道ネルシャツ赤': '正道绒衫红',
    '正道ネルシャツ青': '正道绒衫蓝',
    'はだけシャツ水': '敞衫水蓝',
    'はだけシャツ桜': '敞衫樱色',
    '縦縞シャツ緑': '竖纹衫绿',
    '縦縞シャツ赤': '竖纹衫红',
    '縦縞シャツ黄': '竖纹衫黄',
    '敵を呪い殺す一撃死攻撃　範囲：敵単体': '诅咒一击必杀　范围：敌单体',
    '魔を払う一撃死攻撃　範囲：敵単体': '驱魔一击必杀　范围：敌单体',
    '物理攻撃に強い': '耐物理攻击',
    '物理攻撃に弱い／魔法に強い／破魔、呪殺無効': '物理弱／魔法强／破魔诅咒无效',
    'ガンバルから仲よくしてね⊆': '我会努力，请多关照⊆',
    '涼風スカート水': '凉风裙水蓝',
    '涼風スカート桜': '凉风裙樱色',
    '民族スカート紺': '民族裙深蓝',
    'ルーズハーフ紺': '宽松半裤深蓝',
    'ケミカルＧパン紺': '化学Ｇ裤深蓝',
    '向き Ｘ%03.01f Ｙ%03.01f Ｚ%03.01f': '向Ｘ%03.01fＹ%03.01fＺ%03.01f',
    '破壊----': '破坏----',
    '構築----': '构筑----',
    '容量----／----': '容量----／----',
    '敵を眠らせる針　範囲：敵単体': '催眠针　范围：敌单体',
    '毒を含んだ針　範囲：敵単体': '毒针　范围：敌单体',
    '敵を襲う無数の針　範囲：敵単体': '万针齐发　范围：敌单体',
    '瀕死と死亡を除く状態治療　範囲：味方全体': '恢复异常状态　范围：我方全体',
    '毒状態にする　範囲：敵全体': '中毒　范围：敌全体',
    '闇の力で全てを呪殺する　範囲：敵全体': '黑暗咒杀全体　范围：敌全体',
    'ムドの広範囲版　範囲：敵全体': '广域咒杀　范围：敌全体',
    '反属性の敵に大ダメージ　範囲：敵全体': '克制属性大伤害　范围：敌全体',
    'アギ（火炎）の強化版　範囲：敵単体': '火焰术强化版　范围：敌单体',
    '物理に強く破魔無効': '耐物理／破魔无效',
    '銃、投具に弱い': '弱枪械／投具',
    '打撃、斬撃、技、破魔無効／銃、投具に弱い': '打击斩击技破魔无效／弱枪投具',
    'マハサマンダ': '玛哈萨曼达',
    '愚者の声': '愚者之声',
    'ペトラアイズ': '石化之眼',
    '毒ガスブレス': '猛毒之息',
}


def resolve_tbl_file(tbl_path: Optional[Path] = None) -> Path:
    """Resolve the preferred write-back TBL, allowing an explicit override."""
    if tbl_path is not None:
        return tbl_path

    for candidate in TBL_CANDIDATES:
        if candidate.exists():
            return candidate

    return TBL_CANDIDATES[0]


def normalize_prefix_for_compare(text: str) -> str:
    return ''.join(to_halfwidth_char(char) for char in text)


def extract_leading_opaque_prefix(text: str) -> tuple[str, str]:
    idx = 0
    while idx < len(text) and text[idx] in LEADING_OPAQUE_PREFIX_CHARS:
        idx += 1
    if idx == 0:
        return "", text
    return text[:idx], text[idx:]


def sanitize_translation_text(reference: str, text: str, percent_marks: Optional[List[str]] = None) -> str:
    """Strip obvious review noise and normalize symbol variants before encoding."""
    if not text:
        return ""
    if not text.strip():
        return text

    cleaned = text.strip()
    if cleaned.startswith("修正后的译文："):
        cleaned = cleaned[len("修正后的译文："):].lstrip()

    if cleaned.startswith("**"):
        return ""
    if any(cleaned.startswith(prefix) for prefix in INVALID_TRANSLATION_PREFIXES):
        return ""
    if any(token in cleaned for token in INVALID_TRANSLATION_SUBSTRINGS):
        return ""
    if "✓" in cleaned or "\ufffd" in cleaned:
        return ""

    out = ''.join(SYMBOL_NORMALIZATION.get(char, char) for char in cleaned)
    out = apply_percent_marks(out, percent_marks)
    out = normalize_format_tokens_halfwidth(out, preserve_control_tags=False)
    return normalize_visible_ascii_like_reference(
        out,
        reference,
        preserve_control_tags=False,
        preserve_format_tokens=True,
    )


def strip_duplicate_structured_prefix(reference: str, chinese: str) -> str:
    if not chinese:
        return chinese

    opaque_prefix, _ = extract_leading_opaque_prefix(reference)
    if opaque_prefix and chinese.startswith(opaque_prefix):
        return chinese[len(opaque_prefix):]

    opaque = OPAQUE_PREFIX_OVERRIDES.get(reference)
    if opaque is not None:
        _, body = opaque
        if chinese.startswith(reference[:1]):
            return chinese[1:]
        if chinese.startswith(body):
            return chinese

    match = STRUCTURED_PREFIX_RE.match(reference)
    if not match:
        return chinese

    prefix = match.group("prefix")
    prefix_len = len(prefix)
    if len(chinese) < prefix_len:
        return chinese

    if normalize_prefix_for_compare(chinese[:prefix_len]) == prefix:
        return chinese[prefix_len:]

    return chinese


def restore_missing_leading_opaque_prefix(reference: str, chinese: str) -> str:
    if not chinese:
        return chinese

    opaque_prefix, _ = extract_leading_opaque_prefix(reference)
    if not opaque_prefix:
        return chinese

    if any(char in chinese for char in opaque_prefix):
        return chinese

    return opaque_prefix + chinese


def resolve_translation_text(
    japanese: str,
    chinese: str,
    percent_marks: Optional[List[str]] = None,
) -> str:
    """Prefer explicit local overrides for known bad or overlong XBE rows."""
    override = XBE_TRANSLATION_OVERRIDES.get(japanese)
    source = override if override is not None else chinese
    marks = percent_marks if percent_marks is not None else extract_percent_marks(japanese)
    return sanitize_translation_text(
        japanese,
        restore_missing_leading_opaque_prefix(
            japanese,
            strip_duplicate_structured_prefix(japanese, source),
        ),
        percent_marks=marks,
    )


def split_structured_prefix(text: str) -> tuple[bytes, str]:
    opaque = OPAQUE_PREFIX_OVERRIDES.get(text)
    if opaque is not None:
        return opaque
    match = STRUCTURED_PREFIX_RE.match(text)
    if not match:
        return b"", text
    return match.group("prefix").encode("latin-1"), match.group("body")


def _count_edge_zensp(text: str) -> tuple[int, int]:
    zen = '\u3000'
    lead = 0
    for ch in text:
        if ch == zen:
            lead += 1
        else:
            break

    trail = 0
    for ch in reversed(text):
        if ch == zen:
            trail += 1
        else:
            break

    return lead, trail


def _match_edge_zensp(jp: str, cn: str) -> str:
    """Match JP padded line width with fullwidth spaces, line by line."""
    if not cn:
        return cn

    jp_lines = jp.replace('\r\n', '\n').replace('\r', '\n').split('\n')
    cn_lines = cn.replace('\r\n', '\n').replace('\r', '\n').split('\n')

    if len(jp_lines) != len(cn_lines):
        return cn

    adjusted: List[str] = []
    zen = '\u3000'

    for jp_line, cn_line in zip(jp_lines, cn_lines):
        jp_lead, jp_trail = _count_edge_zensp(jp_line)
        cn_lead, cn_trail = _count_edge_zensp(cn_line)

        if jp_lead == 0 and jp_trail == 0:
            adjusted.append(cn_line)
            continue

        start = cn_lead
        end = len(cn_line) - cn_trail if cn_trail else len(cn_line)
        core = cn_line[start:end]

        line = zen * jp_lead + core + zen * jp_trail
        delta = len(jp_line) - len(line)
        if delta > 0:
            line += zen * delta

        adjusted.append(line)

    return '\n'.join(adjusted)


def split_item_prefix(item: dict) -> tuple[bytes, str]:
    prefix_hex = item.get('prefix_hex')
    text = item.get('text', '')
    if prefix_hex:
        return bytes.fromhex(prefix_hex), text
    return split_structured_prefix(text)


def parse_xbe_safe_ranges(data: bytes) -> List[Tuple[int, int, str]]:
    """Parse XBE sections and return safe write ranges (data sections only)."""
    if data[0:4] != b'XBEH':
        raise ValueError(f"Not a valid XBE file (magic: {data[0:4]})")

    base_addr = struct.unpack_from('<I', data, 0x0104)[0]
    num_sections = struct.unpack_from('<I', data, 0x011C)[0]
    sect_hdr_addr = struct.unpack_from('<I', data, 0x0120)[0]
    sect_hdr_offset = sect_hdr_addr - base_addr

    ranges = []
    for i in range(num_sections):
        off = sect_hdr_offset + i * 56
        raw_addr = struct.unpack_from('<I', data, off + 12)[0]
        raw_size = struct.unpack_from('<I', data, off + 16)[0]
        name_addr = struct.unpack_from('<I', data, off + 20)[0]
        name_off = name_addr - base_addr
        name_end = data.index(0, name_off)
        name = data[name_off:name_end].decode('ascii', errors='replace')

        if name in SAFE_SECTIONS:
            ranges.append((raw_addr, raw_addr + raw_size, name))

    return ranges


def is_safe_offset(offset: int, length: int, safe_ranges: List[Tuple[int, int, str]]) -> bool:
    """Check if writing at offset+length falls entirely within a safe section."""
    for start, end, _ in safe_ranges:
        if offset >= start and offset + length <= end:
            return True
    return False


def load_tbl(tbl_path: Path) -> Dict[str, bytes]:
    """加载TBL字符映射表

    格式: XXXX=字符
    每个字符使用第一个匹配的编码

    Returns:
        字符到编码字节的映射
    """
    char_map = {}
    with open(tbl_path, 'r', encoding='utf-8') as f:
        for line in f:
            # 只去除换行符，不用strip()以保留空白字符如U+3000
            line = line.rstrip('\r\n')
            if not line or line.startswith('#'):
                continue

            if '=' not in line:
                continue

            parts = line.split('=', 1)
            if len(parts) != 2:
                continue

            hex_code = parts[0].strip()
            chars = parts[1]  # 可能是多字符，如 "HN"，不要strip以保留空白

            if not hex_code:
                continue

            # chars可以是空白字符，所以不检查 not chars

            try:
                code_bytes = bytes.fromhex(hex_code)
                # 只使用第一个匹配（如果字符已存在则跳过）
                for char in chars:
                    if char not in char_map:
                        char_map[char] = code_bytes
            except ValueError:
                pass

    for char, code_bytes in ORIGINAL_GLYPH_BYTES.items():
        char_map.setdefault(char, code_bytes)

    return char_map


def half_to_full(text: str) -> str:
    """将半角英数字和符号转换为全角"""
    return ''.join(to_fullwidth_char(char) for char in text)


def iter_encoded_units(text: str):
    """
    Yield either display characters or raw ASCII bytes.

    XBE UI strings still need raw ASCII for runtime format placeholders.
    Other visible ASCII should be converted to fullwidth before encoding.
    """
    i = 0
    while i < len(text):
        if text[i] == '%':
            match = FORMAT_TOKEN_RE.match(text, i)
            if match:
                yield ('raw', match.group(0).encode('ascii'))
                i = match.end()
                continue

        char = text[i]
        code = ord(char)
        if 0x20 <= code <= 0x7E:
            yield ('raw', char.encode('ascii'))
        else:
            yield ('char', char)
        i += 1


def encode_text(text: str, char_map: Dict[str, bytes]) -> Tuple[bytes, List[str]]:
    """使用TBL编码中文文本

    Args:
        text: 要编码的文本
        char_map: 字符到编码的映射

    Returns:
        (编码后的字节, 无法编码的字符列表)
    """
    result = bytearray()
    missing = []

    for kind, value in iter_encoded_units(text):
        if kind == 'raw':
            result.extend(value)
            continue

        char = value
        if char in char_map:
            result.extend(char_map[char])
        elif char == '\n':
            # XBE UI strings use CRLF for visible line breaks.
            result.extend(b'\x0D\x0A')
        elif char == '\r':
            continue
        elif char == '\x00':
            result.append(0x00)
        else:
            missing.append(char)

    return bytes(result), missing


def collect_all_chars(data: dict) -> Set[str]:
    """收集翻译中使用的所有字符"""
    all_chars = set()
    categories = data.get('categories', {})
    contained_overlap_entries = collect_contained_overlap_entries(data)

    for cat_name, items in categories.items():
        for idx, item in enumerate(items):
            if (cat_name, idx) in contained_overlap_entries:
                continue
            if item.get('preserve_original'):
                continue
            chinese = resolve_translation_text(
                item.get('text', ''),
                item.get('chinese') or "",
                percent_marks=item.get('percent_marks'),
            )
            if chinese:
                for kind, value in iter_encoded_units(chinese):
                    if kind != 'char':
                        continue
                    if value not in '\n\r\x00':
                        all_chars.add(value)

    return all_chars


def check_missing_chars(data: dict, char_map: Dict[str, bytes]) -> Set[str]:
    """检查翻译中是否有码表缺失的字符"""
    all_chars = collect_all_chars(data)
    missing = set()

    for char in all_chars:
        if char not in char_map:
            missing.add(char)

    return missing


def collect_contained_overlap_entries(data: dict) -> Set[Tuple[str, int]]:
    """Find inner XBE rows that are fully contained in an earlier row.

    These are extractor false positives such as:
    - `%sが戦闘不能になりました！`
    - `が戦闘不能になりました！`

    Both cannot be true independent writable fields because the second starts
    inside the first row's byte range. At writeback time we keep only the
    leftmost containing row and skip the contained suffix row.
    """
    rows: List[Tuple[int, int, str, int]] = []
    for cat_name, items in data.get('categories', {}).items():
        if not isinstance(items, list):
            continue
        for idx, item in enumerate(items):
            if item.get('preserve_original'):
                continue
            offset = item.get('offset')
            length = item.get('length')
            if not isinstance(offset, int) or not isinstance(length, int):
                continue
            rows.append((offset, length, cat_name, idx))

    rows.sort(key=lambda row: (row[0], -row[1]))
    skipped: Set[Tuple[str, int]] = set()

    for i, (offset, length, cat_name, idx) in enumerate(rows):
        end = offset + length
        for offset2, length2, cat_name2, idx2 in rows[i + 1:]:
            if offset2 >= end:
                break
            if offset2 > offset and offset2 + length2 <= end:
                skipped.add((cat_name2, idx2))

    return skipped


def apply_translations(
    xbe_path: Path,
    output_path: Path,
    dry_run: bool = False,
    tbl_path: Optional[Path] = None,
    translations_path: Optional[Path] = None,
    patch_code: bool = True,
):
    """将翻译写入XBE文件

    Args:
        xbe_path: 原始XBE文件路径
        output_path: 输出XBE文件路径
        dry_run: 只检查不写入
    """
    if translations_path is None:
        translations_path = TRANSLATED_DIR / "xbe_strings.json"

    if not translations_path.exists():
        print(f"错误: 翻译文件不存在: {translations_path}")
        return False

    resolved_tbl = resolve_tbl_file(tbl_path)

    if not resolved_tbl.exists():
        print(f"错误: TBL文件不存在: {resolved_tbl}")
        return False

    # 加载字符映射
    print(f"加载字符表: {resolved_tbl}")
    char_map = load_tbl(resolved_tbl)
    print(f"  字符数: {len(char_map)}")

    # 加载翻译
    print(f"加载翻译: {translations_path}")
    with open(translations_path, 'r', encoding='utf-8') as f:
        data = json.load(f)

    # 首先检查是否有缺失字符
    print("\n检查字符覆盖...")
    missing_chars = check_missing_chars(data, char_map)

    if missing_chars:
        # 按Unicode排序
        sorted_missing = sorted(missing_chars, key=lambda c: ord(c))

        # 先保存缺失字符报告
        report_path = TRANSLATED_DIR / "missing_chars_report.txt"
        with open(report_path, 'w', encoding='utf-8') as f:
            f.write("# SMT NINE 翻译缺失字符报告\n")
            f.write(f"# 共 {len(missing_chars)} 个字符\n\n")
            f.write("# 字符列表（每行一个）:\n")
            for char in sorted_missing:
                f.write(f"{char}\tU+{ord(char):04X}\n")
            f.write("\n# 所有缺失字符（一行）:\n")
            f.write(''.join(sorted_missing) + '\n')

        print(f"\n错误: 码表中缺少 {len(missing_chars)} 个字符!")
        print(f"缺失字符已保存到: {report_path}")
        print(f"\n请将这些字符添加到字库后重新运行。")

        return False

    print("  所有字符都在码表中 [OK]")

    contained_overlap_entries = collect_contained_overlap_entries(data)
    if contained_overlap_entries:
        print(f"  检测到并将跳过 {len(contained_overlap_entries)} 条重叠后缀误提取")

    # 读取原始XBE
    print(f"\n读取XBE: {xbe_path}")
    xbe_data = bytearray(xbe_path.read_bytes())
    print(f"  大小: {len(xbe_data)} bytes")

    # Parse safe write ranges
    safe_ranges = parse_xbe_safe_ranges(xbe_data)
    print(f"\n安全写入区域:")
    for start, end, name in safe_ranges:
        print(f"  {name}: 0x{start:06X} - 0x{end:06X}")

    if patch_code:
        print("\n应用必要代码补丁:")
        if not dry_run:
            apply_font_sample_range(xbe_data)
        for offset, patch_bytes in REQUIRED_CODE_PATCHES.items():
            if not dry_run:
                xbe_data[offset:offset + len(patch_bytes)] = patch_bytes
            print(f"  0x{offset:06X} -> {patch_bytes.hex()}")

    # 统计
    total = 0
    applied = 0
    preserved_length = 0
    preserved_no_trans = 0
    preserved_unsafe = 0
    preserved_requested = 0
    overflow_items = []

    # 处理每个分类
    categories = data.get('categories', {})
    for cat_name, items in categories.items():
        print(f"\n处理分类: {cat_name}")
        cat_applied = 0

        for idx, item in enumerate(items):
            total += 1
            offset = item.get('offset', 0)
            orig_length = item.get('length', 0)
            max_length = item.get('max_length', orig_length)

            if (cat_name, idx) in contained_overlap_entries:
                preserved_requested += 1
                continue

            if item.get('preserve_original'):
                preserved_requested += 1
                continue

            prefix_bytes, japanese = split_item_prefix(item)
            chinese = resolve_translation_text(
                japanese,
                item.get('chinese') or "",
                percent_marks=item.get('percent_marks'),
            )

            if not chinese:
                preserved_no_trans += 1
                continue

            # 补齐 JP 行两侧的全角空格，避免界面布局字符串缩窄
            chinese = _match_edge_zensp(japanese, chinese)

            # 编码中文
            encoded, missing = encode_text(chinese, char_map)
            if prefix_bytes:
                encoded = prefix_bytes + encoded

            # 理论上不应该有缺失（已提前检查）
            if missing:
                print(f"  警告: 意外的缺失字符: {''.join(missing)}")
                continue

            # Section safety check (use full write region: max_length + 1 null)
            write_size = max_length + 1
            if not is_safe_offset(offset, write_size, safe_ranges):
                preserved_unsafe += 1
                overflow_items.append({
                    'category': cat_name,
                    'offset': offset,
                    'orig_length': orig_length,
                    'max_length': max_length,
                    'new_length': len(encoded),
                    'overflow': 0,
                    'error': '保留原文: 非安全节',
                    'japanese': item.get('text', ''),
                    'chinese': chinese
                })
                continue

            # 检查长度（用max_length作为上限）
            if len(encoded) > max_length:
                preserved_length += 1
                overflow = len(encoded) - max_length
                orig_text = item.get('text', '')
                overflow_items.append({
                    'category': cat_name,
                    'offset': offset,
                    'orig_length': orig_length,
                    'max_length': max_length,
                    'new_length': len(encoded),
                    'overflow': overflow,
                    'error': '保留原文: 超长',
                    'japanese': orig_text,
                    'chinese': chinese
                })
                continue

            # 写入数据（填充整个字段，保证null终止）
            if not dry_run:
                new_data = encoded + b'\x00' * (write_size - len(encoded))
                xbe_data[offset:offset + write_size] = new_data

            applied += 1
            cat_applied += 1

        print(f"  应用: {cat_applied}/{len(items)}")

    print(f"\n" + "=" * 60)
    print(f"统计:")
    print(f"  总条目: {total}")
    print(f"  已应用: {applied}")
    print(f"  保留原文(超长): {preserved_length}")
    print(f"  保留原文(无翻译): {preserved_no_trans}")
    if preserved_requested > 0:
        print(f"  保留原文(按规则跳过): {preserved_requested}")
    if preserved_unsafe > 0:
        print(f"  保留原文(非安全节): {preserved_unsafe}")

    # 始终刷新报告，避免遗留过期结果
    report_path = TRANSLATED_DIR / "xbe_overflow_report.json"
    with open(report_path, 'w', encoding='utf-8') as f:
        json.dump(overflow_items, f, ensure_ascii=False, indent=2)
    if overflow_items:
        print(f"\n超长条目已保存: {report_path}")

        # 显示摘要
        print(f"\n超长条目摘要 (共{len(overflow_items)}条):")
        for item in overflow_items[:20]:
            jp = item['japanese'][:15] + '...' if len(item['japanese']) > 15 else item['japanese']
            cn = item['chinese'][:15] + '...' if len(item['chinese']) > 15 else item['chinese']
            ml = item.get('max_length', item['orig_length'])
            print(f"  [{item['category']}] +{item['overflow']}B (max={ml}): {jp} → {cn}")
        if len(overflow_items) > 20:
            print(f"  ... 还有 {len(overflow_items) - 20} 条")
    else:
        print(f"\n超长条目已保存: {report_path} [已清空]")

    if not dry_run:
        # 保存修改后的XBE
        refresh_section_digests(xbe_data)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(xbe_data)
        print(f"\n已保存: {output_path}")
    else:
        print(f"\n[预览模式] 未写入文件")

    return True


def main():
    import argparse

    parser = argparse.ArgumentParser(description='SMT NINE XBE汉化写入工具')
    parser.add_argument('--xbe', type=Path,
                        default=BACKUP_XBE_PATH,
                        help='原始XBE文件路径')
    parser.add_argument('--output', type=Path,
                        default=OUTPUT_DIR / 'default.xbe',
                        help='输出XBE文件路径')
    parser.add_argument('--translations', type=Path,
                        default=TRANSLATED_DIR / 'xbe_strings.json',
                        help='翻译JSON路径')
    parser.add_argument('--tbl', type=Path,
                        help='写回使用的TBL文件路径（默认自动优先选择扩容后的完整表）')
    parser.add_argument('--dry-run', action='store_true',
                        help='只检查不写入')

    args = parser.parse_args()

    if not args.xbe.exists():
        print(f"错误: XBE文件不存在: {args.xbe}")
        sys.exit(1)

    success = apply_translations(
        args.xbe,
        args.output,
        dry_run=args.dry_run,
        tbl_path=args.tbl,
        translations_path=args.translations,
    )

    if not success:
        sys.exit(1)


if __name__ == '__main__':
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
    main()
