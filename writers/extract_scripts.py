# -*- coding: utf-8 -*-
"""
SMT NINE 脚本提取工具

从Media/Script和Media/Scriptf提取日文文本，
将相同部分合并，不同部分分开标记。
"""

import os
import re
import json
import struct
import bisect
import difflib
import copy
import importlib.util
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple, Optional, Set

def load_tbl(tbl_path: Path) -> Dict[int, str]:
    """
    加载TBL码表文件
    支持两种格式：
    1. 文本格式: XXXX=字符 (XXXX为十六进制)
    2. 二进制格式: sysfont.tbl (SMT NINE原始格式)
    返回: {code: char}
    """
    data = tbl_path.read_bytes()

    # 检测是否是二进制格式（以偏移开头）
    if len(data) >= 4:
        first_bytes = data[:4]
        # 二进制TBL: 前4字节是LE uint32偏移，通常含0x00字节
        # 文本TBL: 以hex数字开头如 "8XXX=字"，不含0x00
        has_null = 0x00 in first_bytes
        first_not_text = first_bytes[0] < 0x30 or first_bytes[0] > 0x7E
        if has_null or first_not_text:
            return load_binary_tbl(tbl_path)

    # 文本格式
    return load_text_tbl(tbl_path)


def load_binary_tbl(tbl_path: Path) -> Dict[int, str]:
    """
    加载二进制TBL（sysfont.tbl格式）
    格式：
    - 4字节：第二表偏移
    - 后续是大端2字节SJIS码的数组
    """
    data = tbl_path.read_bytes()

    # 读取第二表偏移
    offset2 = struct.unpack('<I', data[:4])[0]

    mapping = {}

    # 读取第一表（f12，12x12字体）
    # 从字节4开始到offset2
    i = 4
    idx = 0
    while i < offset2 and i + 1 < len(data):
        code = struct.unpack('>H', data[i:i+2])[0]
        if code > 0:
            # 尝试解码为SJIS字符
            try:
                char = struct.pack('>H', code).decode('cp932')
                mapping[code] = char
            except:
                pass
        i += 2
        idx += 1

    # 读取第二表（f24，24x24字体）
    # 跳过8字节头
    i = offset2 + 8
    while i + 1 < len(data):
        code = struct.unpack('>H', data[i:i+2])[0]
        if code > 0:
            try:
                char = struct.pack('>H', code).decode('cp932')
                mapping[code] = char
            except:
                pass
        i += 2

    return mapping


def load_text_tbl(tbl_path: Path) -> Dict[int, str]:
    """
    加载文本格式TBL
    格式: XXXX=字符
    """
    mapping = {}
    with open(tbl_path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or '=' not in line:
                continue
            parts = line.split('=', 1)
            if len(parts) == 2:
                try:
                    code = int(parts[0], 16)
                    char = parts[1]
                    mapping[code] = char
                except ValueError:
                    continue
    return mapping


def is_valid_text(text: str) -> bool:
    """
    检查文本是否是有效的游戏文本（而非误识别的二进制数据）
    """
    if not text:
        return False

    # 统计各类字符
    hiragana = 0  # 平假名
    katakana = 0  # 片假名
    common_kanji = 0  # 常用汉字
    rare_kanji = 0  # 生僻汉字
    jp_punct = 0  # 日文标点
    fullwidth = 0  # 全角ASCII
    ascii_chars = 0  # ASCII字符
    unknown_codes = 0  # 未知码 [XXXX]
    control_codes = 0  # 控制码 <XX>

    # 检测未知码和控制码
    unknown_matches = re.findall(r'\[[0-9A-F]{4}\]', text)
    unknown_codes = len(unknown_matches)
    control_matches = re.findall(r'<[0-9A-F]{2}>', text)
    control_codes = len(control_matches)

    # 移除控制码后统计
    clean_text = re.sub(r'\[[0-9A-F]{4}\]', '', text)
    clean_text = re.sub(r'<[0-9A-F]{2}>', '', clean_text)

    # 常用汉字范围 (一级汉字大致范围)
    common_kanji_set = set()
    # 简单判断：JIS第一水准汉字大约在 0x4E00-0x9FAF 范围
    # 但一些生僻字如 囮(0x56EE) 卆(0x5346) 也在这个范围
    # 更好的方法：检查是否有平假名/片假名，正常对话一定有

    for c in clean_text:
        cp = ord(c)
        if 0x3040 <= cp <= 0x309F:  # 平假名
            hiragana += 1
        elif 0x30A0 <= cp <= 0x30FF:  # 片假名
            katakana += 1
        elif 0x4E00 <= cp <= 0x9FFF:  # CJK汉字
            # 常见汉字检测 - 大多数游戏对话使用常用汉字
            # 生僻字如 囮 卆 等单独计数
            if c in '囮卆罠黻黼黽鼇鼈鼉鋏':
                rare_kanji += 1
            else:
                common_kanji += 1
        elif 0x3000 <= cp <= 0x303F:  # 日文标点
            jp_punct += 1
        elif 0xFF00 <= cp <= 0xFFEF:  # 全角ASCII
            fullwidth += 1
        elif 0x20 <= cp <= 0x7E:  # ASCII可打印
            ascii_chars += 1

    clean_len = len(clean_text)
    jp_chars = hiragana + katakana + common_kanji + rare_kanji + jp_punct + fullwidth

    # 过滤规则：
    # 1. 必须有日文字符
    if jp_chars == 0:
        return False

    # 2. 未知码太多说明是二进制数据
    if unknown_codes >= 1:
        return False

    # 3. 文本太短时需要更严格的检查
    if clean_len <= 4:
        # 短文本必须有平假名或片假名（纯汉字短文本很可能是误识别）
        if hiragana == 0 and katakana == 0:
            return False
        # 有生僻字的短文本很可能是二进制数据
        if rare_kanji > 0:
            return False

    # 4. 正常游戏文本必须有平假名或片假名（纯汉字长文本也可疑）
    if hiragana == 0 and katakana == 0:
        # 纯汉字文本，需要足够长且没有生僻字
        if common_kanji < 6 or rare_kanji > 0:
            return False

    # 5. 生僻汉字不能太多
    if rare_kanji > 0 and rare_kanji >= common_kanji:
        return False

    # 6. 日文字符占比要合理（清理后至少40%）
    if clean_len > 0 and jp_chars / clean_len < 0.4:
        return False

    # 7. 控制码不能比日文字符多
    if control_codes > jp_chars:
        return False

    # 8. 检测重复字符模式（如 fff, 999等）- 二进制数据特征
    if re.search(r'(.)\1{2,}', clean_text):
        repeat_count = len(re.findall(r'(.)\1{2,}', clean_text))
        if repeat_count > 0 and jp_chars < 4:
            return False

    return True


def is_speaker_name(line: str) -> bool:
    """
    检测一行是否是说话人名字
    名字特征：
    - 长度2-7个字符（常见名字如 バラキ、ミランダ、管理局員）
    - 全是片假名、汉字或少量平假名
    - 不含助词（を、の、に、は、が、で、と、も、へ、から、まで等）
    - 不含动词活用形结尾或句尾助词
    """
    line = line.strip()
    if not line:
        return False

    # 说话人名字通常是2-7个字符
    if len(line) < 2 or len(line) > 7:
        return False

    # 助词 - 这些出现在名字里的概率很低
    particles = 'をのにはがでともへ'

    # 句尾助词和动词结尾 - 对话常以这些结尾
    sentence_endings = 'よねなわさぞぜかなぁっ！？…'
    verb_endings = 'てたるくいうえおけせねべめれげぜでねべ'

    # 检查是否包含助词
    for p in particles:
        if p in line:
            return False

    # 检查是否以句尾助词结尾（对话特征）
    if line and line[-1] in sentence_endings:
        return False

    # 检查是否以动词活用形结尾
    if line and line[-1] in verb_endings:
        return False

    # 检查是否全是日文字符（没有标点等）
    for c in line:
        cp = ord(c)
        is_jp = (0x3040 <= cp <= 0x309F or  # 平假名
                 0x30A0 <= cp <= 0x30FF or  # 片假名
                 0x4E00 <= cp <= 0x9FFF or  # 汉字
                 0xFF00 <= cp <= 0xFFEF)    # 全角
        if not is_jp:
            return False

    return True


def split_by_speaker(text: str) -> List[str]:
    """
    将包含多个说话人的文本拆分为多个独立对话

    例如:
    "バラキ\nおい\nスミレ\nなに？" -> ["バラキ\nおい", "スミレ\nなに？"]
    """
    if '\n' not in text:
        return [text]

    lines = text.split('\n')
    result = []
    current_dialogue = []

    for i, line in enumerate(lines):
        # 检查当前行是否是说话人名字
        if is_speaker_name(line):
            # 如果之前有积累的对话，保存它
            if current_dialogue:
                result.append('\n'.join(current_dialogue))
            # 开始新的对话
            current_dialogue = [line]
        else:
            current_dialogue.append(line)

    # 保存最后一段对话
    if current_dialogue:
        # 如果只是一个说话人名字且result不为空，则丢弃（会在下一个条目中出现）
        # 但如果这是唯一的内容，保留它
        text_content = '\n'.join(current_dialogue)
        if result and is_speaker_name(text_content.strip()):
            # 这是一个单独的说话人名字，会在下一个条目中重新出现，可以跳过
            pass
        else:
            result.append(text_content)

    # 如果没有任何结果，返回原文
    if not result:
        return [text]

    return result


def is_speaker_line(text: str) -> bool:
    """
    检测文本是否以说话人名字开头
    格式通常是: 名字\n对话内容
    """
    if '\n' not in text:
        return False

    first_line = text.split('\n')[0].strip()
    return is_speaker_name(first_line)


def merge_adjacent_texts(texts: List[Dict], max_gap: int = 4) -> List[Dict]:
    """
    合并相邻的文本块为完整对话（旧版，仅按gap大小判断）
    保留用于向后兼容。新代码应使用 merge_texts_smart()。
    """
    if not texts:
        return []

    sorted_texts = sorted(texts, key=lambda x: x['offset'])
    merged = []
    current = None

    for t in sorted_texts:
        text = t['text']
        is_new_speaker = is_speaker_line(text)

        if current is None:
            current = {
                'offset': t['offset'],
                'length': t['length'],
                'raw_hex': t['raw_hex'],
                'text': text,
                'label': t.get('label'),
            }
        else:
            prev_end = current['offset'] + current['length']
            gap = t['offset'] - prev_end

            if is_new_speaker:
                merged.append(current)
                current = {
                    'offset': t['offset'],
                    'length': t['length'],
                    'raw_hex': t['raw_hex'],
                    'text': text,
                    'label': t.get('label'),
                }
            elif 0 <= gap <= max_gap and current.get('label') == t.get('label'):
                current['length'] = t['offset'] + t['length'] - current['offset']
                current['raw_hex'] += t['raw_hex']
                current['text'] += '\n' + text
            else:
                merged.append(current)
                current = {
                    'offset': t['offset'],
                    'length': t['length'],
                    'raw_hex': t['raw_hex'],
                    'text': text,
                    'label': t.get('label'),
                }

    if current:
        merged.append(current)

    return merged


def _find_sequential_text_entries(data: bytes, text_start: int, text_end: int) -> List[Dict]:
    """
    构建VM顺序文本条目列表，匹配字节码 02 XX XX 的0基索引。

    扫描文本区域中所有null终止的SJIS字符串（不经过is_valid_text过滤），
    因为VM按顺序索引引用文本，包括那些不通过过滤的条目。
    """
    entries = []
    i = text_start
    while i < text_end - 1:
        b1 = data[i]
        if b1 == 0x00:
            i += 1
            continue
        # 跳过FF控制码（2字节）
        if b1 == 0xFF:
            i += 2
            continue
        # SJIS双字节字符开头
        if (0x81 <= b1 <= 0x9F) or (0xE0 <= b1 <= 0xFC):
            b2 = data[i + 1] if i + 1 < text_end else 0
            if (0x40 <= b2 <= 0x7E) or (0x80 <= b2 <= 0xFC):
                start = i
                end = i
                while end < text_end and data[end] != 0x00:
                    end += 1
                entries.append({'offset': start, 'end': end})
                i = end
                continue
        i += 1
    return entries


def _parse_mgs_bytecode_groups(data: bytes, tbl: Dict[int, str]) -> Optional[Dict[int, int]]:
    """
    解析MGS字节码，提取文本框分组（用于非FF04变体）。

    VM指令：
      02 XX XX (LE uint16) = 按0基索引引用文本条目
      04 00 00 80 BF = 文本框推进/清除边界

    两个连续的02引用之间没有04边界 = 同一文本框。
    返回: {文本条目偏移: 组ID} 或 None（解析失败时）。
    """
    if len(data) < 0x16 or data[:2] != b'\x7e\x00':
        return None

    sections = list(struct.unpack_from('<5I', data, 2))
    s0, s1, s2 = sections[0], sections[1], sections[2]
    bc_start = 0x16
    bc_end = s0

    # 在文本区域中查找第一个SJIS文本
    text_start = None
    i = s1
    while i < s2 - 1:
        b1 = data[i]
        if (0x82 <= b1 <= 0x84) or (0x88 <= b1 <= 0x9F) or (0xE0 <= b1 <= 0xEA):
            b2 = data[i + 1]
            if (0x40 <= b2 <= 0x7E) or (0x80 <= b2 <= 0xFC):
                text_start = i
                break
        i += 1
    if text_start is None:
        return None

    # 构建完整顺序文本条目列表（VM索引用）
    all_entries = _find_sequential_text_entries(data, text_start, s2)
    if not all_entries:
        return None

    def register_entry_group(offset_to_group: Dict[int, int], entry_idx: int, group: int):
        entry = all_entries[entry_idx]
        offsets = [entry['offset']]
        alias = entry['offset']
        while alias + 1 < entry['end']:
            b1 = data[alias]
            b2 = data[alias + 1]
            if b1 == 0x81 and ((0x40 <= b2 <= 0x7E) or (0x80 <= b2 <= 0xFC)):
                alias += 2
                offsets.append(alias)
                continue
            if b1 == 0xFF and b2 != 0x00:
                alias += 2
                offsets.append(alias)
                continue
            break
        for offset in offsets:
            if offset not in offset_to_group:
                offset_to_group[offset] = group

    # 扫描字节码：02 XX XX（文本引用）和 04 00 00 80 BF（文本框边界）
    text_refs = []
    textbox_boundaries = []
    speaker_events = []
    TEXTBOX_MARKER = b'\x04\x00\x00\x80\xbf'

    i = bc_start
    while i < bc_end:
        if i + 12 <= bc_end and data[i:i + 3] == b'\x03\xff\xff':
            speaker_idx = None
            event_len = 0
            if data[i + 3:i + 4] == b'\x01':
                if data[i + 4:i + 6] == b'\xff\xff' and data[i + 8:i + 10] == b'\x10\x27':
                    speaker_idx = struct.unpack_from('<H', data, i + 6)[0]
                    event_len = 10
                elif data[i + 4:i + 6] == b'\x10\x27' and data[i + 8:i + 10] == b'\xff\xff':
                    speaker_idx = struct.unpack_from('<H', data, i + 6)[0]
                    event_len = 10
            elif data[i + 3:i + 6] == b'\x10\x0f\x01':
                if data[i + 6:i + 8] == b'\x10\x27' and data[i + 10:i + 12] == b'\xff\xff':
                    speaker_idx = struct.unpack_from('<H', data, i + 8)[0]
                    event_len = 12
            if speaker_idx is not None:
                speaker_events.append((i, speaker_idx, event_len))
                i += event_len
                continue
        if i + 5 <= bc_end and data[i:i+5] == TEXTBOX_MARKER:
            textbox_boundaries.append(i)
            i += 5
            continue
        if data[i] == 0x02 and i + 3 <= bc_end:
            val = struct.unpack_from('<H', data, i + 1)[0]
            text_refs.append((i, val))
            i += 3
            continue
        i += 1

    if not text_refs:
        return None

    # 过滤有效范围并按字节码偏移排序
    max_idx = len(all_entries)
    sorted_refs = sorted(
        [(off, val) for off, val in text_refs if val < max_idx],
        key=lambda x: x[0]
    )
    if not sorted_refs:
        return None

    # 特殊变体：aku_yuko / aku_teki 这类脚本会显式设置说话人名牌。
    # 这时真正的“发言块”不是单个TB窗口，而是从一个说话人设置开始，
    # 跨过若干个TB，直到下一个不同说话人设置为止。
    speaker_opcode_events = []
    for bc_off, speaker_idx, event_len in speaker_events:
        if not (0 <= speaker_idx < max_idx):
            continue
        speaker_entry = all_entries[speaker_idx]
        speaker_text = decode_sjis_with_tbl(
            data[speaker_entry['offset']:speaker_entry['end']],
            tbl,
        )
        if not is_speaker_name(speaker_text):
            continue
        speaker_opcode_events.append((bc_off, speaker_idx, speaker_text, event_len))

    if len(speaker_opcode_events) >= 10:
        offset_to_group = {}
        group_id = -1
        current_group = None
        current_speaker = None
        speaker_event_by_bc = {
            bc_off: (speaker_idx, speaker_text, event_len)
            for bc_off, speaker_idx, speaker_text, event_len in speaker_opcode_events
        }

        i = bc_start
        while i < bc_end:
            speaker_event = speaker_event_by_bc.get(i)
            if speaker_event is not None:
                speaker_idx, speaker_text, event_len = speaker_event
                if current_group is None or current_speaker != speaker_text:
                    group_id += 1
                    current_group = group_id
                current_speaker = speaker_text
                register_entry_group(offset_to_group, speaker_idx, current_group)
                i += event_len
                continue

            if i + 5 <= bc_end and data[i:i+5] == TEXTBOX_MARKER:
                if current_speaker is None:
                    current_group = None
                i += 5
                continue

            if data[i] == 0x02 and i + 3 <= bc_end:
                text_idx = struct.unpack_from('<H', data, i + 1)[0]
                if text_idx < max_idx:
                    if current_group is None:
                        group_id += 1
                        current_group = group_id
                    register_entry_group(offset_to_group, text_idx, current_group)
                i += 3
                continue

            i += 1

        if offset_to_group:
            return offset_to_group

    sorted_boundaries = sorted(textbox_boundaries)

    # 按文本框边界分组
    offset_to_group = {}
    group_id = 0

    first_text_idx = sorted_refs[0][1]
    register_entry_group(offset_to_group, first_text_idx, group_id)

    for i in range(1, len(sorted_refs)):
        prev_bc_off = sorted_refs[i - 1][0]
        curr_bc_off = sorted_refs[i][0]

        # 检查两个引用之间是否有文本框边界
        lo = bisect.bisect_right(sorted_boundaries, prev_bc_off)
        has_boundary = (lo < len(sorted_boundaries) and
                        sorted_boundaries[lo] < curr_bc_off)

        if has_boundary:
            group_id += 1

        text_idx = sorted_refs[i][1]
        register_entry_group(offset_to_group, text_idx, group_id)

    return offset_to_group


def _is_mgp_internal_label(label: str) -> bool:
    return len(label) >= 3 and all(c.isupper() or c.isdigit() or c in "_-" for c in label)


UNKNOWN_TEXT_MARKER_RE = re.compile(r'\[[0-9A-F]{4}\]')
INLINE_CONTROL_CODE_RE = re.compile(r'<[0-9A-F]{2}>')


def normalized_alignment_text(text: str) -> str:
    text = INLINE_CONTROL_CODE_RE.sub('', text)
    text = UNKNOWN_TEXT_MARKER_RE.sub('', text)
    text = re.sub(r'\s+', '', text)
    return text


def alignment_similarity(left: str, right: str) -> float:
    left_lines = left.splitlines()
    right_lines = right.splitlines()
    if left_lines and right_lines and is_speaker_name(left_lines[0]) and left_lines[0] == right_lines[0]:
        left = "\n".join(left_lines[1:])
        right = "\n".join(right_lines[1:])
    left_norm = normalized_alignment_text(left)
    right_norm = normalized_alignment_text(right)
    if not left_norm or not right_norm:
        return 0.0
    return difflib.SequenceMatcher(None, left_norm, right_norm).ratio()


def _collect_mgp_scene_offsets(data: bytes) -> Optional[List[int]]:
    if len(data) < 8:
        return None

    scene_count = struct.unpack_from('<H', data, 2)[0]
    header_end = 4 + scene_count * 4
    if scene_count <= 0 or header_end > len(data):
        return None

    raw_values: List[int] = []
    for i in range(scene_count):
        table_offset = 4 + i * 4
        if table_offset + 4 > len(data):
            return None
        raw_values.append(struct.unpack_from('<I', data, table_offset)[0])

    scene_offsets = [value for value in raw_values if header_end <= value < len(data)]
    scene_offsets = _dedupe_preserve_order(scene_offsets)
    if not scene_offsets or scene_offsets[0] != header_end:
        return None
    if any(b <= a for a, b in zip(scene_offsets, scene_offsets[1:])):
        return None
    return scene_offsets


def _find_mgp_scene_label_block(scene: bytes) -> Optional[Dict[str, object]]:
    candidate_starts = _dedupe_preserve_order([
        match.start()
        for match in re.finditer(rb'[A-Z0-9_-]{3,}\x00', scene)
    ])

    for label_block_start in reversed(candidate_starts):
        p = label_block_start
        scene_labels: List[str] = []
        while p < len(scene):
            end = scene.find(b'\x00', p)
            if end < 0:
                break
            try:
                label = scene[p:end].decode('ascii')
            except Exception:
                break
            if not _is_mgp_internal_label(label):
                break
            scene_labels.append(label)
            p = end + 1

        if not scene_labels or p + 2 > len(scene):
            continue

        text_count = struct.unpack_from('<H', scene, p)[0]
        if text_count <= 0 or text_count > 0x4000:
            continue

        lengths_start = p + 2
        lengths_end = lengths_start + text_count
        if lengths_end > len(scene):
            continue

        lengths = list(scene[lengths_start:lengths_end])
        if not lengths or sum(1 for value in lengths if value == 0) > text_count // 2:
            continue

        total_bytes = sum(lengths)
        text_start = lengths_end
        if lengths_end + 2 <= len(scene):
            maybe_total = struct.unpack_from('<H', scene, lengths_end)[0]
            if maybe_total == total_bytes:
                text_start += 2

        if text_start + total_bytes > len(scene):
            continue

        return {
            'label_block_start': label_block_start,
            'scene_labels': scene_labels,
            'text_count': text_count,
            'lengths': lengths,
            'text_start': text_start,
        }

    return None


def _parse_mgp_scene_wrapper(scene: bytes) -> Optional[Dict[str, object]]:
    if not scene:
        return None

    scene_name_span = scene[0]
    nested_offset = 1 + scene_name_span
    if scene_name_span <= 0 or nested_offset + 0x12 > len(scene):
        return None

    try:
        scene_name = scene[1:nested_offset].rstrip(b'\x00').decode('cp932')
    except Exception:
        scene_name = ""

    nested = scene[nested_offset:]
    if nested[:2] != b'\x7e\x00':
        return None

    sections = list(struct.unpack_from('<4I', nested, 2))
    code_end, text_start, aux1_start, aux2_start = sections
    meta_start = code_end
    meta_end = text_start
    text_end = aux1_start
    if not (0 < code_end <= meta_end <= text_start <= text_end <= aux2_start <= len(nested)):
        return None

    return {
        'scene_name': scene_name,
        'nested_offset': nested_offset,
        'nested': nested,
        'code_start': 0,
        'code_end': code_end,
        'meta_start': meta_start,
        'meta_end': meta_end,
        'text_start': text_start,
        'text_end': text_end,
        'aux1_start': aux1_start,
        'aux2_start': aux2_start,
        'sections': sections,
    }


def _parse_mgp_scene_labels(
    nested: bytes,
    meta_start: int,
    meta_end: int,
) -> Optional[Dict[str, object]]:
    meta = nested[meta_start:meta_end]
    if len(meta) < 4:
        return None

    label_count = struct.unpack_from('<H', meta, 0)[0]
    table_span = struct.unpack_from('<H', meta, 2)[0]
    if label_count <= 0 or table_span != label_count * 4:
        return None

    offsets_start = 4
    offsets_end = offsets_start + table_span
    if offsets_end > len(meta):
        return None
    label_offsets = [
        struct.unpack_from('<I', meta, offsets_start + i * 4)[0]
        for i in range(label_count)
    ]

    lengths_start = offsets_end
    lengths_end = lengths_start + label_count
    if lengths_end + 2 > len(meta):
        return None

    label_lengths = list(meta[lengths_start:lengths_end])
    label_bytes = struct.unpack_from('<H', meta, lengths_end)[0]
    labels_start = lengths_end + 2
    if labels_start + label_bytes != len(meta):
        return None

    scene_labels: List[str] = []
    cursor = labels_start
    for size in label_lengths:
        if size <= 0 or cursor + size > len(meta):
            return None
        raw = meta[cursor:cursor + size]
        if raw[-1] != 0x00:
            return None
        try:
            scene_labels.append(raw[:-1].decode('ascii'))
        except Exception:
            return None
        cursor += size

    return {
        'scene_labels': scene_labels,
        'label_offsets': label_offsets,
    }


def _parse_mgp_scene_texts(
    nested: bytes,
    text_start: int,
    text_end: int,
    absolute_base_offset: int,
    tbl: Dict[int, str],
    fallback_label_map: Dict[int, str],
    default_label: Optional[str],
) -> Optional[List[Dict]]:
    if text_start + 4 > text_end:
        return None

    text_count = struct.unpack_from('<H', nested, text_start)[0]
    if text_count <= 0:
        return None

    lengths_start = text_start + 2
    lengths_end = lengths_start + text_count
    if lengths_end + 2 > text_end:
        return None

    total_bytes = struct.unpack_from('<H', nested, lengths_end)[0]
    records_start = lengths_end + 2
    if records_start + total_bytes != text_end:
        return None

    local_texts: List[Dict] = []
    cursor = records_start
    for local_idx in range(text_count):
        record_size = nested[lengths_start + local_idx]
        if record_size <= 0 or cursor + record_size > text_end:
            return None
        text_len = record_size - 1
        raw = nested[cursor:cursor + text_len]
        absolute_offset = absolute_base_offset + cursor
        local_texts.append({
            'local_idx': local_idx,
            'offset': absolute_offset,
            'length': text_len,
            'raw_hex': raw.hex(),
            'text': decode_sjis_with_tbl(raw, tbl),
            'label': fallback_label_map.get(absolute_offset) or default_label,
        })
        cursor += record_size

    if cursor != text_end:
        return None
    return local_texts


def _dedupe_preserve_order(values: List[int]) -> List[int]:
    unique_values: List[int] = []
    seen: Set[int] = set()
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        unique_values.append(value)
    return unique_values


def _scene_block_looks_like_placeholder_garbage(block: Dict) -> bool:
    text = str(block.get('text') or "")
    parts = block.get('parts') or []
    if not text or not isinstance(parts, list) or not parts:
        return False

    if not UNKNOWN_TEXT_MARKER_RE.search(text):
        return False

    zero_parts = 0
    for part in parts:
        if not isinstance(part, dict):
            continue
        raw_hex = str(part.get('raw_hex') or "")
        if not raw_hex:
            continue
        try:
            raw = bytes.fromhex(raw_hex)
        except ValueError:
            continue
        if raw and all(byte == 0x00 for byte in raw):
            zero_parts += 1

    return zero_parts > 0


def _raw_hex_is_all_zero(raw_hex: str) -> bool:
    if not raw_hex:
        return False
    try:
        raw = bytes.fromhex(raw_hex)
    except ValueError:
        return False
    return bool(raw) and all(byte == 0x00 for byte in raw)


def _raw_hex_has_embedded_nul(raw_hex: str) -> bool:
    if not raw_hex:
        return False
    try:
        raw = bytes.fromhex(raw_hex)
    except ValueError:
        return False
    return b'\x00' in raw and not all(byte == 0x00 for byte in raw)


def _strip_scene_part_markers(text: str) -> str:
    text = INLINE_CONTROL_CODE_RE.sub('', text)
    text = UNKNOWN_TEXT_MARKER_RE.sub('', text)
    text = text.replace('　', '')
    text = re.sub(r'\s+', '', text)
    return text


def _text_has_private_use_chars(text: str) -> bool:
    return any(0xE000 <= ord(ch) <= 0xF8FF for ch in text)


def _text_has_kana(text: str) -> bool:
    return any(
        0x3040 <= ord(ch) <= 0x309F or
        0x30A0 <= ord(ch) <= 0x30FF
        for ch in text
    )


def _text_looks_like_ascii_label(text: str) -> bool:
    stripped = _strip_scene_part_markers(text)
    return bool(stripped) and re.fullmatch(r'[A-Z0-9_]+', stripped) is not None


def _text_is_long_no_kana_noise(text: str) -> bool:
    stripped = _strip_scene_part_markers(text)
    if len(stripped) < 8:
        return False
    if _text_has_kana(stripped):
        return False
    if _text_looks_like_ascii_label(stripped):
        return False
    if is_speaker_name(stripped):
        return False
    return True


def _part_is_blank_placeholder(text: str, raw_hex: str) -> bool:
    if _raw_hex_is_all_zero(raw_hex):
        return True
    return _strip_scene_part_markers(text) == ''


def _part_is_definite_garbage(text: str, raw_hex: str, aggressive_no_kana_filter: bool = False) -> bool:
    if _part_is_blank_placeholder(text, raw_hex):
        return True
    if UNKNOWN_TEXT_MARKER_RE.search(text):
        return True
    if _text_has_private_use_chars(text):
        return True
    if aggressive_no_kana_filter and _text_is_long_no_kana_noise(text):
        return True
    stripped = _strip_scene_part_markers(text)
    if (
        _raw_hex_has_embedded_nul(raw_hex)
        and len(stripped) >= 4
        and not _text_has_kana(stripped)
        and not is_speaker_name(text)
    ):
        return True
    return False


def _part_has_real_scene_content(text: str) -> bool:
    stripped = _strip_scene_part_markers(text)
    if len(stripped) < 2:
        return False
    if _text_has_private_use_chars(stripped):
        return False
    return is_valid_text(text)


def _slice_mgp_scene_block(
    block: Dict,
    local_texts: List[Dict],
    start_idx: int,
    end_idx: int,
) -> Optional[Dict]:
    local_indices = list(block.get('_local_indices') or [])
    parts = list(block.get('parts') or [])
    if not local_indices or len(local_indices) != len(parts):
        return None
    if start_idx < 0 or end_idx > len(local_indices) or start_idx >= end_idx:
        return None

    new_local_indices = local_indices[start_idx:end_idx]
    new_parts = [dict(part) for part in parts[start_idx:end_idx]]
    new_part_texts = list(block.get('_part_texts') or [])[start_idx:end_idx]
    new_pages: List[Dict] = []
    new_part_cursor = 0
    for page in block.get('pages') or []:
        part_start = int(page.get('part_start', 0))
        part_end = part_start + int(page.get('part_count', 0))
        overlap_start = max(part_start, start_idx)
        overlap_end = min(part_end, end_idx)
        if overlap_start >= overlap_end:
            continue
        new_page = {
            'part_start': new_part_cursor,
            'part_count': overlap_end - overlap_start,
        }
        if 'marker_offset' in page:
            new_page['marker_offset'] = int(page['marker_offset'])
            new_page['marker_length'] = int(page.get('marker_length', 5))
            new_page['marker_raw'] = str(page.get('marker_raw') or '')
        new_pages.append(new_page)
        new_part_cursor += overlap_end - overlap_start

    min_offset = min(int(local_texts[idx]['offset']) for idx in new_local_indices)
    max_end = max(int(local_texts[idx]['offset']) + int(local_texts[idx]['length']) for idx in new_local_indices)
    first = local_texts[new_local_indices[0]]
    sliced = dict(block)
    sliced['offset'] = min_offset
    sliced['length'] = max_end - min_offset
    sliced['raw_hex'] = ''.join(str(local_texts[local_idx]['raw_hex']) for local_idx in new_local_indices)
    sliced['text'] = '\n'.join(str(local_texts[local_idx]['text']) for local_idx in new_local_indices)
    sliced['parts'] = new_parts
    sliced['_part_texts'] = new_part_texts
    if new_pages:
        sliced['pages'] = new_pages
    else:
        sliced.pop('pages', None)
    sliced['_local_indices'] = new_local_indices
    first_text = str(first.get('text') or '')
    sliced['_speaker_name'] = first_text if is_speaker_name(first_text) else None
    return sliced


def _clean_mgp_scene_block(
    block: Dict,
    local_texts: List[Dict],
    aggressive_no_kana_filter: bool = False,
) -> Optional[Dict]:
    local_indices = list(block.get('_local_indices') or [])
    parts = list(block.get('parts') or [])
    if not local_indices or len(local_indices) != len(parts):
        return block

    part_infos: List[Dict] = []
    suspicious = False
    content_indices: List[int] = []
    for idx, (local_idx, part) in enumerate(zip(local_indices, parts)):
        text = str(local_texts[local_idx]['text'] or '')
        raw_hex = str(part.get('raw_hex') or '')
        blank = _part_is_blank_placeholder(text, raw_hex)
        garbage = _part_is_definite_garbage(
            text,
            raw_hex,
            aggressive_no_kana_filter=aggressive_no_kana_filter,
        )
        real_content = (
            not garbage
            and _part_has_real_scene_content(text)
            and not is_speaker_name(text)
        )
        suspicious_part = (
            blank
            or garbage
            or _raw_hex_has_embedded_nul(raw_hex)
            or INLINE_CONTROL_CODE_RE.search(text) is not None
        )
        if suspicious_part:
            suspicious = True
        if real_content:
            content_indices.append(idx)
        part_infos.append({
            'trim_candidate': suspicious_part and not real_content,
        })

    if not suspicious:
        return block
    if not content_indices:
        return None

    start_idx = content_indices[0]
    while start_idx > 0:
        previous = part_infos[start_idx - 1]
        if previous['trim_candidate']:
            break
        start_idx -= 1

    end_idx = content_indices[-1] + 1
    while end_idx < len(part_infos):
        following = part_infos[end_idx]
        if following['trim_candidate']:
            break
        end_idx += 1

    if start_idx == 0 and end_idx == len(part_infos):
        return block
    return _slice_mgp_scene_block(block, local_texts, start_idx, end_idx)


def _fallback_entry_is_low_quality(entry: Dict, aggressive_no_kana_filter: bool = False) -> bool:
    text = str(entry.get('text') or '')
    stripped = _strip_scene_part_markers(text)
    if not stripped:
        return True
    if _text_has_private_use_chars(text):
        return True
    lines = text.splitlines()
    if len(lines) != 1:
        return False
    if any(
        0x3040 <= ord(ch) <= 0x309F or 0x30A0 <= ord(ch) <= 0x30FF
        for ch in stripped
    ):
        return False
    if aggressive_no_kana_filter and _text_is_long_no_kana_noise(stripped):
        return True
    return len(stripped) >= 6 and _raw_hex_has_embedded_nul(str(entry.get('raw_hex') or ''))


def _garbage_lead_token(text: str) -> Optional[str]:
    for line in str(text or '').splitlines():
        token = _strip_scene_part_markers(line)
        if len(token) < 3:
            continue
        if any(
            0x3040 <= ord(ch) <= 0x309F or 0x30A0 <= ord(ch) <= 0x30FF
            for ch in token
        ):
            continue
        return token
    return None


def _is_nearby_page_speaker_ref(
    speaker_ref: int,
    first_content_ref: Optional[int],
) -> bool:
    if first_content_ref is None:
        return False
    distance = first_content_ref - speaker_ref
    return 0 <= distance <= 2


def _mgp_event_opcode_family(event: Dict) -> str:
    raw_hex = str(event.get('bytecode_raw') or "")
    if raw_hex.startswith('010200'):
        return 'wrapped_text'
    if raw_hex.startswith('02') or raw_hex.startswith('55') or raw_hex.startswith('72'):
        return 'bare_text'
    if (
        raw_hex.startswith('010700')
        or raw_hex.startswith('010800')
        or raw_hex.startswith('010900')
    ):
        return 'wrapped_speaker'
    if raw_hex.startswith('07') or raw_hex.startswith('08') or raw_hex.startswith('09'):
        return 'bare_speaker'
    return 'other'


def _should_replace_mgp_duplicate_text_event(
    text_events: List[Dict],
    previous_pos: int,
    current_pos: int,
    local_texts: List[Dict],
) -> bool:
    previous_event = text_events[previous_pos]
    current_event = text_events[current_pos]

    local_idx = int(current_event.get('local_idx', -1))
    if not (0 <= local_idx < len(local_texts)):
        return False

    current_family = _mgp_event_opcode_family(current_event)
    if current_family != 'bare_text':
        return False

    previous_family = _mgp_event_opcode_family(previous_event)
    local_text = str(local_texts[local_idx].get('text') or "")

    # Rule A: a non-name speaker opcode was surfaced as visible text too early.
    if (
        previous_family in ('bare_speaker', 'wrapped_speaker')
        and not is_speaker_name(local_text)
    ):
        return True

    # Rule B: keep the later bare ref when it sits inside the real
    # sequential `idx-1 / idx / idx+1` text cluster.
    if previous_family != 'bare_text':
        return False
    if current_pos <= 0 or current_pos + 1 >= len(text_events):
        return False

    prev_neighbor = text_events[current_pos - 1]
    next_neighbor = text_events[current_pos + 1]
    if (
        _mgp_event_opcode_family(prev_neighbor) != 'bare_text'
        or _mgp_event_opcode_family(next_neighbor) != 'bare_text'
    ):
        return False

    prev_neighbor_idx = int(prev_neighbor.get('local_idx', -1))
    next_neighbor_idx = int(next_neighbor.get('local_idx', -1))
    if prev_neighbor_idx == local_idx or next_neighbor_idx == local_idx:
        return False

    return prev_neighbor_idx + 1 == local_idx and local_idx + 1 == next_neighbor_idx


def _normalize_mgp_page_text_events(
    page_events: List[Dict],
    local_texts: List[Dict],
) -> List[Dict]:
    text_events = [
        event
        for event in page_events
        if event.get('kind') == 'text'
        and isinstance(event.get('local_idx'), int)
    ]
    if len(text_events) <= 1:
        return [dict(event) for event in text_events]

    chosen_positions: Dict[int, int] = {}
    for pos, event in enumerate(text_events):
        local_idx = int(event.get('local_idx', -1))
        if local_idx not in chosen_positions:
            chosen_positions[local_idx] = pos
            continue
        previous_pos = chosen_positions[local_idx]
        if _should_replace_mgp_duplicate_text_event(
            text_events,
            previous_pos,
            pos,
            local_texts,
        ):
            chosen_positions[local_idx] = pos

    normalized = [
        dict(text_events[pos])
        for pos in sorted(chosen_positions.values())
    ]
    normalized.sort(key=lambda event: int(event.get('bytecode_offset', -1)))
    return normalized


def _normalize_mgp_page_refs(
    text_refs: List[int],
    speaker_refs: List[int],
    local_texts: List[Dict],
) -> Tuple[List[int], Optional[int]]:
    """
    普通 MGP 一页里的文本索引归一化。

    这里不再做“按句尾/人名猜窗口”的脏后处理，只做两件事：
    - 去重保序
    - 把显式说话人名和正文拆开；如果这一页本身带名字，就保留在页首
    """
    ordered_text_refs = _dedupe_preserve_order(text_refs)
    ordered_speaker_refs = _dedupe_preserve_order(speaker_refs)

    if len(ordered_text_refs) >= 2:
        first_ref = ordered_text_refs[0]
        second_ref = ordered_text_refs[1]
        first_entry = local_texts[first_ref]
        second_entry = local_texts[second_ref]
        first_end = int(first_entry['offset']) + int(first_entry['length'])
        second_offset = int(second_entry['offset'])
        if (
            is_speaker_name(first_entry['text'])
            and second_offset > first_end + 1
        ):
            ordered_text_refs = ordered_text_refs[1:]

    if not ordered_text_refs and not ordered_speaker_refs:
        return [], None

    first_content_ref = ordered_text_refs[0] if ordered_text_refs else None

    speaker_ref = None
    if ordered_text_refs:
        for ref in reversed(ordered_speaker_refs):
            speaker_text = _strip_scene_part_markers(str(local_texts[ref].get('text') or ''))
            if (
                _is_nearby_page_speaker_ref(ref, first_content_ref)
                and _looks_like_strong_speaker_name(speaker_text)
            ):
                speaker_ref = ref
                break

    normalized: List[int] = list(ordered_text_refs or ordered_speaker_refs)
    if speaker_ref is not None and (not normalized or normalized[0] != speaker_ref):
        normalized.insert(0, speaker_ref)
    return normalized, speaker_ref


def _build_entry(offset: int, text: str, raw_hex: str, length: int,
                 label: Optional[str], parts: Optional[List[Dict]] = None,
                 pages: Optional[List[Dict]] = None,
                 original_offset: Optional[int] = None,
                 bytecode_region_start: Optional[int] = None,
                 bytecode_region_end: Optional[int] = None,
                 scene_idx: Optional[int] = None,
                 scene_entry_index: Optional[int] = None) -> Dict:
    storage_jp_lines: Optional[List[str]] = None
    part_line_map: Optional[List[int]] = None
    mid_speakers: Optional[List[Dict]] = None
    if parts and len(parts) > 1:
        text_lines = text.split('\n')
        if len(text_lines) == len(parts):
            storage_jp_lines, raw_part_line_map = _build_storage_line_layout(parts, text_lines)
            if isinstance(raw_part_line_map, list) and len(raw_part_line_map) == len(parts):
                part_line_map = [int(value) for value in raw_part_line_map if value is not None]
                if len(part_line_map) != len(parts):
                    part_line_map = None
            else:
                part_line_map = None

            if storage_jp_lines is not None and part_line_map is not None:
                line_source_parts: List[List[int]] = [[] for _ in range(len(storage_jp_lines))]
                for part_idx, line_index in enumerate(part_line_map):
                    if 0 <= line_index < len(line_source_parts):
                        line_source_parts[line_index].append(part_idx)

                speaker_line_indices: List[int] = []
                for line_index, line_text in enumerate(storage_jp_lines):
                    source_parts = line_source_parts[line_index]
                    if not source_parts:
                        continue
                    has_speaker_opcode = any(
                        str(parts[part_idx].get('bytecode_kind') or '') == 'speaker'
                        for part_idx in source_parts
                    )
                    if not has_speaker_opcode:
                        continue
                    if not _looks_like_strong_speaker_name(line_text.strip()):
                        continue
                    has_following_body = any(
                        later_index > line_index
                        and line_source_parts[later_index]
                        and not any(
                            str(parts[part_idx].get('bytecode_kind') or '') == 'speaker'
                            for part_idx in line_source_parts[later_index]
                        )
                        for later_index in range(line_index + 1, len(storage_jp_lines))
                    )
                    if not has_following_body:
                        continue
                    speaker_line_indices.append(line_index)

                if speaker_line_indices:
                    mid_speakers = []
                    for line_index in speaker_line_indices:
                        mapped_part_index = next(
                            (
                                part_idx
                                for part_idx, mapped_line_index in enumerate(part_line_map)
                                if mapped_line_index == line_index
                            ),
                            None,
                        )
                        if mapped_part_index is None:
                            continue
                        mid_speakers.append({
                            'part_index': mapped_part_index,
                            'text_line': line_index,
                            'speaker_jp': storage_jp_lines[line_index],
                        })
                    if mid_speakers:
                        text = '\n'.join(
                            line
                            for line_index, line in enumerate(storage_jp_lines)
                            if line_index not in set(speaker_line_indices)
                        )

    entry = {
        'offset': offset,
        'length': length,
        'raw_hex': raw_hex,
        'text': text,
        'label': label,
    }
    if mid_speakers:
        entry['mid_speakers'] = mid_speakers
    if parts and (len(parts) > 1 or pages):
        entry['parts'] = parts
    if pages:
        entry['pages'] = pages
    if storage_jp_lines:
        entry['storage_jp_lines'] = storage_jp_lines
    if part_line_map:
        entry['part_line_map'] = part_line_map
    if original_offset is not None and original_offset != offset:
        entry['original_offset'] = original_offset
    if bytecode_region_start is not None:
        entry['bytecode_region_start'] = int(bytecode_region_start)
    if bytecode_region_end is not None:
        entry['bytecode_region_end'] = int(bytecode_region_end)
    if scene_idx is not None:
        entry['scene_idx'] = int(scene_idx)
    if scene_entry_index is not None:
        entry['scene_entry_index'] = int(scene_entry_index)
    return entry


def _looks_like_strong_speaker_name(text: str) -> bool:
    text = _strip_scene_part_markers(text)
    if not text or '\n' in text:
        return False
    if len(text) < 1 or len(text) > 12:
        return False
    if any(ch in '「」『』（）()、。！？…・：；<>[]' for ch in text):
        return False

    hiragana = 0
    katakana = 0
    kanji = 0

    for ch in text:
        cp = ord(ch)
        if 0x3040 <= cp <= 0x309F:
            hiragana += 1
        elif 0x30A0 <= cp <= 0x30FF:
            katakana += 1
        elif 0x4E00 <= cp <= 0x9FFF:
            kanji += 1
        elif ch == '　':
            continue
        else:
            return False

    if hiragana > 1:
        return False
    if hiragana == 1 and (text.count('の') != 1 or kanji == 0):
        return False
    return (katakana + kanji) > 0


INLINE_SPEAKER_LINE_RE = re.compile(r'^([^「\n]{1,12})「(.*)$')


def _strip_text_markers_for_speaker(text: str) -> str:
    text = INLINE_CONTROL_CODE_RE.sub('', text)
    text = UNKNOWN_TEXT_MARKER_RE.sub('', text)
    return text.strip()


def _split_inline_speaker_text(text: str) -> Optional[Tuple[str, str]]:
    lines = text.splitlines()
    if not lines:
        return None

    match = INLINE_SPEAKER_LINE_RE.match(lines[0])
    if not match:
        return None

    speaker = _strip_text_markers_for_speaker(match.group(1))
    if not _looks_like_strong_speaker_name(speaker):
        return None

    first_body = match.group(2)
    rest = lines[1:]
    body = '\n'.join([first_body] + rest).rstrip('\n')
    return speaker, body


def _decode_entry_part_texts(entry: Dict, data: bytes, tbl: Dict[int, str]) -> List[str]:
    parts = entry.get('parts') or []
    result = []
    for part in parts:
        offset = int(part['offset'])
        length = int(part['length'])
        result.append(decode_sjis_with_tbl(data[offset:offset + length], tbl))
    return result


def _entry_has_complex_dialogue_shape(entry: Dict) -> bool:
    if entry.get('parts') or entry.get('pages'):
        return True
    return '\n' in str(entry.get('text') or '')


def _build_mgp_known_speaker_names(
    texts: List[Dict],
    data: bytes,
    tbl: Dict[int, str],
) -> Set[str]:
    counter: Dict[str, int] = {}

    def add(name: str, weight: int) -> None:
        if not name:
            return
        counter[name] = counter.get(name, 0) + weight

    for entry in texts:
        parts = entry.get('parts') or []
        if not parts:
            continue
        part_texts = _decode_entry_part_texts(entry, data, tbl)
        if not part_texts:
            continue
        first_text = _strip_text_markers_for_speaker(part_texts[0])
        if parts[0].get('bytecode_kind') == 'speaker' and _looks_like_strong_speaker_name(first_text):
            add(first_text, 3)

    for i in range(len(texts) - 1):
        current = texts[i]
        following = texts[i + 1]
        current_text = _strip_text_markers_for_speaker(str(current.get('text') or ''))
        if not _looks_like_strong_speaker_name(current_text):
            continue
        gap = int(following['offset']) - (int(current['offset']) + int(current['length']))
        if gap > 2:
            continue
        if not _entry_has_complex_dialogue_shape(following):
            continue
        add(current_text, 1)

    return {
        name
        for name, weight in counter.items()
        if weight >= 2
    }


def _entry_is_bare_known_speaker(entry: Dict, known_speakers: Set[str]) -> Optional[str]:
    if entry.get('parts') or entry.get('pages') or entry.get('mid_speakers'):
        return None
    if entry.get('part_line_map') or entry.get('storage_jp_lines'):
        return None
    text = str(entry.get('text') or '')
    if not text or '\n' in text:
        return None
    clean = _strip_text_markers_for_speaker(text)
    if clean in known_speakers and _looks_like_strong_speaker_name(clean):
        return clean
    return None


def _make_speaker_part_from_entry(entry: Dict) -> Dict:
    return {
        'offset': int(entry['offset']),
        'length': int(entry['length']),
        'raw_hex': str(entry['raw_hex']),
        'bytecode_kind': 'speaker',
    }


def _merge_leading_speaker_entry_into_dialogue(
    speaker_entry: Dict,
    dialogue_entry: Dict,
) -> Dict:
    merged = copy.deepcopy(dialogue_entry)
    speaker_part = _make_speaker_part_from_entry(speaker_entry)
    speaker_text = str(speaker_entry.get('text') or '')

    if merged.get('parts'):
        merged['parts'] = [speaker_part] + copy.deepcopy(merged['parts'])
        if merged.get('pages'):
            merged['pages'] = copy.deepcopy(merged['pages'])
            merged['pages'][0]['part_count'] = int(merged['pages'][0].get('part_count', 0)) + 1
            for page in merged['pages'][1:]:
                page['part_start'] = int(page['part_start']) + 1
    else:
        body_part = {
            'offset': int(merged['offset']),
            'length': int(merged['length']),
            'raw_hex': str(merged['raw_hex']),
        }
        merged['parts'] = [speaker_part, body_part]
        if not merged.get('pages'):
            merged['pages'] = [{
                'part_start': 0,
                'part_count': 2,
            }]

    existing_storage_jp_lines = merged.get('storage_jp_lines')
    existing_part_line_map = merged.get('part_line_map')
    if isinstance(existing_storage_jp_lines, list) and all(isinstance(line, str) for line in existing_storage_jp_lines):
        merged['storage_jp_lines'] = [speaker_text] + list(existing_storage_jp_lines)
    else:
        dialogue_lines = str(dialogue_entry.get('text') or '').split('\n')
        synthesized_lines = [line for line in dialogue_lines if line != '']
        if speaker_text:
            merged['storage_jp_lines'] = [speaker_text] + synthesized_lines
        elif synthesized_lines:
            merged['storage_jp_lines'] = synthesized_lines
    if isinstance(existing_part_line_map, list) and all(isinstance(value, int) and not isinstance(value, bool) and value >= 0 for value in existing_part_line_map):
        merged['part_line_map'] = [0] + [int(value) + 1 for value in existing_part_line_map]
    else:
        merged['part_line_map'] = list(range(len(merged['parts'])))

    if merged.get('mid_speakers'):
        merged['mid_speakers'] = copy.deepcopy(merged['mid_speakers'])
        for speaker in merged['mid_speakers']:
            if 'part_index' in speaker:
                speaker['part_index'] = int(speaker['part_index']) + 1
            if 'text_line' in speaker:
                speaker['text_line'] = int(speaker['text_line']) + 1

    merged['offset'] = int(speaker_entry['offset'])
    max_end = max(
        int(part['offset']) + int(part['length'])
        for part in merged['parts']
    )
    merged['length'] = max_end - int(merged['offset'])
    merged['raw_hex'] = ''.join(str(part['raw_hex']) for part in merged['parts'])
    return merged


def _build_storage_line_roles(
    entry: Dict,
    *,
    speaker: Optional[str],
    speaker_source: Optional[str],
) -> Optional[List[str]]:
    storage_jp_lines = entry.get('storage_jp_lines')
    if not isinstance(storage_jp_lines, list) or not storage_jp_lines:
        return None

    roles: List[str] = ['text'] * len(storage_jp_lines)
    if (
        speaker
        and speaker_source in ('inline', 'parts', 'text_first_line', 'standalone_merge')
        and _strip_text_markers_for_speaker(str(storage_jp_lines[0] or '')) == speaker
    ):
        roles[0] = 'speaker'

    part_line_map = entry.get('part_line_map')
    mid_speakers = entry.get('mid_speakers') or []
    if isinstance(part_line_map, list):
        for mid_speaker in mid_speakers:
            if not isinstance(mid_speaker, dict):
                continue
            try:
                part_index = int(mid_speaker.get('part_index'))
            except (TypeError, ValueError):
                continue
            if not (0 <= part_index < len(part_line_map)):
                continue
            try:
                storage_line_index = int(part_line_map[part_index])
            except (TypeError, ValueError):
                continue
            if 0 <= storage_line_index < len(roles):
                roles[storage_line_index] = 'speaker'

    return roles


def _normalize_speaker_metadata(
    texts: List[Dict],
    data: bytes,
    tbl: Dict[int, str],
    file_path: Path,
) -> List[Dict]:
    if not texts:
        return texts

    is_mgp = file_path.suffix.lower() == '.mgp'
    known_speakers: Set[str] = set()
    if is_mgp:
        known_speakers = _build_mgp_known_speaker_names(texts, data, tbl)

    analyzed: List[Dict[str, object]] = []
    for entry in texts:
        normalized_entry = copy.deepcopy(entry)
        speaker = None
        body_text = None
        speaker_source = None
        body_text_from_storage_layout = False
        storage_jp_lines = normalized_entry.get('storage_jp_lines')
        logical_storage_lines = (
            [str(line) for line in storage_jp_lines]
            if isinstance(storage_jp_lines, list)
            and storage_jp_lines
            and all(isinstance(line, str) for line in storage_jp_lines)
            else None
        )

        inline_split = _split_inline_speaker_text(str(normalized_entry.get('text') or ''))
        if inline_split is not None:
            speaker, body_text = inline_split
            speaker_source = 'inline'

        part_texts = _decode_entry_part_texts(normalized_entry, data, tbl)
        parts = normalized_entry.get('parts') or []
        if speaker is None and parts and part_texts:
            candidate_lines = logical_storage_lines if is_mgp and logical_storage_lines else part_texts
            first_text = _strip_text_markers_for_speaker(candidate_lines[0])
            if len(candidate_lines) >= 2 and (
                (
                    parts[0].get('bytecode_kind') == 'speaker'
                    and _looks_like_strong_speaker_name(first_text)
                )
                or (is_mgp and first_text in known_speakers)
            ):
                speaker = first_text
                body_text = '\n'.join(candidate_lines[1:])
                speaker_source = 'parts'
            elif (
                len(candidate_lines) == 1
                and parts[0].get('bytecode_kind') == 'speaker'
                and _looks_like_strong_speaker_name(first_text)
            ):
                speaker = first_text
                body_text = ''
                speaker_source = 'parts'

        if speaker is None:
            lines = str(normalized_entry.get('text') or '').splitlines()
            if len(lines) >= 2:
                first_line = _strip_text_markers_for_speaker(lines[0])
                if is_mgp and first_line in known_speakers:
                    speaker = first_line
                    body_text = '\n'.join(lines[1:])
                    speaker_source = 'text_first_line'

        if is_mgp and logical_storage_lines:
            storage_line_roles = _build_storage_line_roles(
                normalized_entry,
                speaker=speaker if isinstance(speaker, str) else None,
                speaker_source=speaker_source if isinstance(speaker_source, str) else None,
            )
            if isinstance(storage_line_roles, list) and len(storage_line_roles) == len(logical_storage_lines):
                if speaker:
                    body_text = '\n'.join(
                        line
                        for line, role in zip(logical_storage_lines, storage_line_roles)
                        if role != 'speaker'
                    )
                else:
                    body_text = '\n'.join(logical_storage_lines)
                body_text_from_storage_layout = True

        analyzed.append({
            'entry': normalized_entry,
            'speaker': speaker,
            'body_text': body_text,
            'speaker_source': speaker_source,
            'body_text_from_storage_layout': body_text_from_storage_layout,
        })

    merged_items: List[Dict[str, object]] = []
    i = 0
    while i < len(analyzed):
        current = analyzed[i]
        current_entry = current['entry']

        if is_mgp and i + 1 < len(analyzed):
            speaker_name = _entry_is_bare_known_speaker(current_entry, known_speakers)
            if speaker_name is not None:
                following = analyzed[i + 1]
                following_entry = following['entry']
                following_text = _strip_text_markers_for_speaker(str(following_entry.get('text') or ''))
                gap = int(following_entry['offset']) - (
                    int(current_entry['offset']) + int(current_entry['length'])
                )
                following_is_bare_speaker = (
                    _entry_is_bare_known_speaker(following_entry, known_speakers) is not None
                )

                if (
                    gap <= 2
                    and following_text
                    and not following.get('speaker')
                    and not following_is_bare_speaker
                ):
                    merged_entry = _merge_leading_speaker_entry_into_dialogue(
                        current_entry,
                        following_entry,
                    )
                    merged_items.append({
                        'entry': merged_entry,
                        'speaker': speaker_name,
                        'body_text': following.get('body_text')
                                     if following.get('body_text') is not None
                                     else str(following_entry.get('text') or ''),
                        'speaker_source': 'standalone_merge',
                    })
                    i += 2
                    continue

        merged_items.append(current)
        i += 1

    result: List[Dict] = []
    last_speaker: Optional[str] = None
    last_original_offset: Optional[int] = None

    for item in merged_items:
        entry = item['entry']
        speaker = item.get('speaker')
        body_text = item.get('body_text')
        speaker_source = item.get('speaker_source')
        body_text_from_storage_layout = bool(item.get('body_text_from_storage_layout'))

        if body_text is None:
            body_text = str(entry.get('text') or '')
        body_text = str(body_text).rstrip('\n')

        if (
            is_mgp
            and not speaker
            and last_speaker
            and entry.get('original_offset') is not None
            and entry.get('original_offset') == last_original_offset
            and entry.get('storage_line_roles') is None
        ):
            speaker = last_speaker
            speaker_source = 'inherited'

        mid_speakers = entry.get('mid_speakers') or []
        if (
            mid_speakers
            and body_text
            and speaker_source == 'parts'
            and not body_text_from_storage_layout
        ):
            body_lines = body_text.split('\n')
            offset = 1 if speaker else 0
            remove_indices = set()
            for ms in mid_speakers:
                tl = int(ms['text_line']) - offset
                if 0 <= tl < len(body_lines):
                    remove_indices.add(tl)
            if remove_indices:
                body_text = '\n'.join(l for i, l in enumerate(body_lines) if i not in remove_indices)

        entry['body_text'] = body_text

        if speaker:
            entry['speaker'] = speaker
            entry['speaker_source'] = speaker_source
            entry['text'] = speaker + ('\n' + body_text if body_text else '')
            last_speaker = speaker
            last_original_offset = entry.get('original_offset')
        else:
            entry['text'] = body_text
            if not is_mgp or entry.get('original_offset') != last_original_offset:
                last_speaker = None
            last_original_offset = entry.get('original_offset')

        storage_line_roles = _build_storage_line_roles(
            entry,
            speaker=speaker if isinstance(speaker, str) else None,
            speaker_source=speaker_source if isinstance(speaker_source, str) else None,
        )
        if storage_line_roles:
            entry['storage_line_roles'] = storage_line_roles

        result.append(entry)

    return result


def _build_storage_line_layout(
    parts: List[Dict],
    text_lines: List[str],
) -> Tuple[Optional[List[str]], Optional[List[int]]]:
    offset_groups: Dict[object, List[int]] = {}
    for part_index, part in enumerate(parts):
        offset_groups.setdefault(part.get('offset'), []).append(part_index)

    phantom_indices: Set[int] = set()
    phantom_aliases: Dict[int, int] = {}

    def is_text_family(raw_hex: str) -> bool:
        return (
            raw_hex.startswith('02')
            or raw_hex.startswith('55')
            or raw_hex.startswith('72')
            or raw_hex.startswith('010200')
            or raw_hex.startswith('015500')
            or raw_hex.startswith('017200')
        )

    def is_speaker_family(raw_hex: str) -> bool:
        return raw_hex.startswith('07') or raw_hex.startswith('08') or raw_hex.startswith('09')

    for indices in offset_groups.values():
        if len(indices) <= 1:
            continue

        text_aliases = [
            idx for idx in indices
            if is_text_family(str(parts[idx].get('bytecode_raw') or ''))
        ]
        if text_aliases:
            alias_index = text_aliases[0]
            for idx in indices:
                if idx == alias_index:
                    continue
                raw_hex = str(parts[idx].get('bytecode_raw') or '')
                if is_speaker_family(raw_hex):
                    phantom_indices.add(idx)
                    phantom_aliases[idx] = alias_index
            continue

        speaker_aliases = [
            idx for idx in indices
            if is_speaker_family(str(parts[idx].get('bytecode_raw') or ''))
        ]
        if len(speaker_aliases) == len(indices):
            alias_index = speaker_aliases[0]
            for idx in speaker_aliases[1:]:
                phantom_indices.add(idx)
                phantom_aliases[idx] = alias_index

    storage_jp_lines: List[str] = []
    part_line_map: List[Optional[int]] = [None] * len(parts)
    for part_index in range(len(parts)):
        if part_index in phantom_indices:
            continue
        part_line_map[part_index] = len(storage_jp_lines)
        storage_jp_lines.append(text_lines[part_index])

    for phantom_index, alias_index in phantom_aliases.items():
        alias_line_index = part_line_map[alias_index]
        if alias_line_index is None:
            return None, None
        part_line_map[phantom_index] = alias_line_index

    if any(line_index is None for line_index in part_line_map):
        return None, None
    return storage_jp_lines, [int(value) for value in part_line_map]


def _scene_block_speaker_name(block: Dict) -> Optional[str]:
    text = str(block.get('text') or "")
    if not text:
        return None
    first_line = text.splitlines()[0].strip()
    if is_speaker_name(first_line):
        return first_line
    return None


def _clone_scene_part(part: Dict) -> Dict:
    cloned = {
        'offset': int(part['offset']),
        'length': int(part['length']),
        'raw_hex': str(part['raw_hex']),
    }
    for key in ('bytecode_offset', 'bytecode_length', 'bytecode_kind', 'bytecode_raw'):
        if key in part:
            cloned[key] = part[key]
    return cloned


def _clone_scene_page(page: Dict) -> Dict:
    cloned = {
        'part_start': int(page['part_start']),
        'part_count': int(page['part_count']),
    }
    if 'marker_offset' in page:
        cloned['marker_offset'] = page['marker_offset']
    if 'marker_length' in page:
        cloned['marker_length'] = page['marker_length']
    if 'marker_raw' in page:
        cloned['marker_raw'] = page['marker_raw']
    return cloned


def _select_page_event(
    local_idx: int,
    kind: str,
    page_events: List[Dict],
) -> Optional[Dict]:
    for event in page_events:
        if event.get('local_idx') == local_idx and event.get('kind') == kind:
            return event
    return None


def _select_part_event(
    local_idx: int,
    speaker_ref: Optional[int],
    page_events: List[Dict],
) -> Optional[Dict]:
    if speaker_ref is not None and local_idx == speaker_ref:
        event = _select_page_event(local_idx, 'speaker', page_events)
        if event is not None:
            return event
    event = _select_page_event(local_idx, 'text', page_events)
    if event is not None:
        return event
    return _select_page_event(local_idx, 'speaker', page_events)


def _parse_mgp_region_event(
    region: bytes,
    pos: int,
    local_texts: List[Dict],
    textbox_marker: bytes,
) -> Optional[Dict]:
    n_local = len(local_texts)

    # Opcode 04 + float32 (5 bytes) = page/textbox advance marker.
    # The standard marker is 04 00 00 80 BF (float -1.0), but other float
    # values also mark page boundaries (e.g. 04 bc bb bb 3e = ~0.367).
    if pos + 5 <= len(region) and region[pos] == 0x04:
        candidate = region[pos:pos + 5]
        # Avoid false positives: the next 4 bytes should look like a float
        # (standard marker or non-zero float), AND should not start a valid
        # text/speaker opcode sequence.
        next_byte = candidate[1]
        is_standard = candidate == textbox_marker
        is_float_like = not (next_byte in (0x02, 0x07, 0x08, 0x09, 0x55, 0x72)
                             and pos + 8 <= len(region)
                             and 0 <= struct.unpack_from('<H', region, pos + 2)[0] < n_local)
        if is_standard or is_float_like:
            return {
                'kind': 'page',
                'length': 5,
                'raw_hex': candidate.hex(),
            }

    if pos + 7 <= len(region) and region[pos] == 0x01:
        wrapped_kind = region[pos + 1]
        if wrapped_kind in (0x02, 0x07, 0x08, 0x09) and region[pos + 2] == 0x00:
            local_idx = struct.unpack_from('<H', region, pos + 3)[0]
            if 0 <= local_idx < n_local:
                event_kind = 'text' if wrapped_kind == 0x02 else 'speaker'
                # Check if bytes at pos+5 form a valid text/speaker opcode
                # that would be swallowed by 7-byte length.  If so, use
                # 5-byte length so the following opcode is parsed separately.
                use_short = False
                b5 = region[pos + 5]
                if b5 in (0x02, 0x55, 0x72) and pos + 8 <= len(region):
                    next_idx = struct.unpack_from('<H', region, pos + 6)[0]
                    if 0 <= next_idx < n_local:
                        use_short = True
                elif b5 in (0x07, 0x08, 0x09) and pos + 9 <= len(region) and region[pos + 6] == 0x00:
                    next_idx = struct.unpack_from('<H', region, pos + 7)[0]
                    if 0 <= next_idx < n_local:
                        use_short = True
                length = 5 if use_short else 7
                return {
                    'kind': event_kind,
                    'local_idx': local_idx,
                    'length': length,
                    'raw_hex': region[pos:pos + length].hex(),
                }

    if pos + 4 <= len(region) and region[pos] in (0x07, 0x08, 0x09) and region[pos + 1] == 0x00:
        local_idx = struct.unpack_from('<H', region, pos + 2)[0]
        if 0 <= local_idx < n_local:
            return {
                'kind': 'speaker',
                'local_idx': local_idx,
                'length': 4,
                'raw_hex': region[pos:pos + 4].hex(),
            }

    if pos + 3 <= len(region) and region[pos] in (0x02, 0x55, 0x72):
        local_idx = struct.unpack_from('<H', region, pos + 1)[0]
        if 0 <= local_idx < n_local:
            return {
                'kind': 'text',
                'local_idx': local_idx,
                'length': 3,
                'raw_hex': region[pos:pos + 3].hex(),
            }

    return None


def _looks_like_mgp_leading_setup_opcode(byte_value: int) -> bool:
    return byte_value in (0x45, 0x46, 0x47, 0x48, 0x49, 0x53, 0x54, 0x56)


def _merge_scene_blocks(left: Dict, right: Dict) -> Dict:
    merged_parts = [_clone_scene_part(part) for part in left['parts']]
    part_base = len(merged_parts)
    merged_parts.extend(_clone_scene_part(part) for part in right['parts'])

    merged_pages = [_clone_scene_page(page) for page in left['pages']]
    for page in right['pages']:
        cloned_page = _clone_scene_page(page)
        cloned_page['part_start'] = part_base + int(page['part_start'])
        merged_pages.append(cloned_page)

    merged_local_indices = list(left['_local_indices'])
    merged_local_indices.extend(right['_local_indices'])
    merged_part_texts = list(left.get('_part_texts') or [])
    merged_part_texts.extend(right.get('_part_texts') or [])

    merged_text_parts = [str(left.get('text') or ""), str(right.get('text') or "")]
    merged_text = '\n'.join(part for part in merged_text_parts if part)

    return {
        'offset': min(int(p['offset']) for p in merged_parts),
        'length': max(int(p['offset']) + int(p['length']) for p in merged_parts) - min(int(p['offset']) for p in merged_parts),
        'raw_hex': ''.join(part['raw_hex'] for part in merged_parts),
        'text': merged_text,
        'label': left.get('label'),
        'parts': merged_parts,
        'pages': merged_pages,
        '_local_indices': merged_local_indices,
        '_part_texts': merged_part_texts,
        '_region_offset': int(left['_region_offset']),
        '_region_start': int(left['_region_start']),
        '_region_end': int(right['_region_end']),
        '_tail_is_page_boundary': bool(right.get('_tail_is_page_boundary')),
        '_speaker_name': right.get('_speaker_name') or left.get('_speaker_name'),
        '_has_leading_setup_opcode': bool(left.get('_has_leading_setup_opcode')),
    }


def _dedup_subset_scene_blocks(scene_blocks: List[Dict]) -> List[Dict]:
    """
    Remove blocks whose _local_indices are a subset of or equal to another block's.

    After splitting non-contiguous blocks, the same text record can appear
    as a standalone fragment in one region and as part of a complete textbox
    in another region. The fragment is redundant and should be removed.

    Also removes exact duplicates (same set of indices) — keeps the first.
    """
    if len(scene_blocks) <= 1:
        return scene_blocks

    index_sets = []
    for block in scene_blocks:
        indices = block.get('_local_indices') or []
        index_sets.append(frozenset(indices))

    keep = []
    seen_sets: List[frozenset] = []
    for i, block in enumerate(scene_blocks):
        if not index_sets[i]:
            keep.append(block)
            continue
        # Skip exact duplicate (same frozenset already kept)
        already_seen = False
        for s in seen_sets:
            if index_sets[i] == s:
                already_seen = True
                break
        if already_seen:
            continue
        # Skip strict subset of any other block
        is_subset = False
        for j, other_set in enumerate(index_sets):
            if i == j:
                continue
            if index_sets[i] < other_set:
                is_subset = True
                break
        if not is_subset:
            keep.append(block)
            seen_sets.append(index_sets[i])

    return keep


def _resolve_overlapping_indices(
    scene_blocks: List[Dict],
    local_texts: List[Dict],
) -> List[Dict]:
    """
    When a local_idx appears in multiple scene blocks (neither a strict subset
    of the other), assign it exclusively to the block with the most total
    indices.  Remove it from all other blocks, rebuilding their parts/pages/
    text as needed.  Drop blocks that become empty.

    This prevents cross-entry overlap in the output JSON — a single text
    record offset should belong to at most one extracted entry.
    """
    if len(scene_blocks) <= 1:
        return scene_blocks

    # Map local_idx → list of block indices that contain it
    idx_to_blocks: Dict[int, List[int]] = {}
    for bi, block in enumerate(scene_blocks):
        for local_idx in (block.get('_local_indices') or []):
            idx_to_blocks.setdefault(local_idx, []).append(bi)

    # For each shared local_idx, decide the owner (most indices; tiebreak: lower offset)
    remove_from_block: Dict[int, Set[int]] = {}   # bi → set of local_idx to remove
    for local_idx, block_indices in idx_to_blocks.items():
        if len(block_indices) <= 1:
            continue
        owner = max(
            block_indices,
            key=lambda bi: (
                len(scene_blocks[bi].get('_local_indices') or []),
                -scene_blocks[bi].get('offset', 0),
            ),
        )
        for bi in block_indices:
            if bi != owner:
                remove_from_block.setdefault(bi, set()).add(local_idx)

    if not remove_from_block:
        return scene_blocks

    result: List[Dict] = []
    for bi, block in enumerate(scene_blocks):
        if bi not in remove_from_block:
            result.append(block)
            continue

        to_remove = remove_from_block[bi]
        old_indices = list(block.get('_local_indices') or [])
        old_parts = list(block.get('parts') or [])
        old_part_texts = list(block.get('_part_texts') or [])

        keep_positions = [i for i, idx in enumerate(old_indices) if idx not in to_remove]
        if not keep_positions:
            continue  # block entirely consumed by larger blocks

        new_indices = [old_indices[i] for i in keep_positions]
        new_parts = [dict(old_parts[i]) for i in keep_positions]
        new_part_texts = [old_part_texts[i] for i in keep_positions] if old_part_texts else []

        # Rebuild pages — map old positions to new positions
        old_to_new = {old_pos: new_pos for new_pos, old_pos in enumerate(keep_positions)}
        new_pages: List[Dict] = []
        for page in (block.get('pages') or []):
            ps = int(page.get('part_start', 0))
            pc = int(page.get('part_count', 0))
            surviving = [old_to_new[i] for i in range(ps, ps + pc) if i in old_to_new]
            if not surviving:
                continue
            new_page: Dict = {
                'part_start': surviving[0],
                'part_count': len(surviving),
            }
            if 'marker_offset' in page:
                new_page['marker_offset'] = int(page['marker_offset'])
                new_page['marker_length'] = int(page.get('marker_length', 5))
                new_page['marker_raw'] = str(page.get('marker_raw') or '')
            new_pages.append(new_page)

        # Rebuild derived fields
        min_offset = min(int(local_texts[idx]['offset']) for idx in new_indices)
        max_end = max(
            int(local_texts[idx]['offset']) + int(local_texts[idx]['length'])
            for idx in new_indices
        )

        new_block = dict(block)
        new_block['_local_indices'] = new_indices
        new_block['parts'] = new_parts
        new_block['_part_texts'] = new_part_texts
        new_block['offset'] = min_offset
        new_block['length'] = max_end - min_offset
        new_block['raw_hex'] = ''.join(str(local_texts[idx]['raw_hex']) for idx in new_indices)
        new_block['text'] = '\n'.join(str(local_texts[idx]['text']) for idx in new_indices)
        if new_pages:
            new_block['pages'] = new_pages
        else:
            new_block.pop('pages', None)

        first_text = str(local_texts[new_indices[0]].get('text') or '')
        new_block['_speaker_name'] = first_text if is_speaker_name(first_text) else None

        result.append(new_block)

    return result


def _split_non_contiguous_scene_blocks(
    scene_blocks: List[Dict],
    local_texts: List[Dict],
) -> List[Dict]:
    """
    Split blocks whose parts reference non-contiguous local_idx values.

    Linear bytecode scanning can mix refs from different code paths (branches)
    into one block. This function detects non-contiguous local_idx groups and
    splits them into separate blocks.
    """
    result: List[Dict] = []
    for block in scene_blocks:
        local_indices = list(block.get('_local_indices') or [])
        if len(local_indices) <= 1:
            result.append(block)
            continue

        # Build (position, local_idx) pairs and sort by local_idx
        pos_idx_pairs = sorted(
            enumerate(local_indices),
            key=lambda pair: pair[1],
        )

        # Find contiguous groups by local_idx
        groups: List[List[int]] = [[pos_idx_pairs[0][0]]]  # groups of positions
        for k in range(1, len(pos_idx_pairs)):
            prev_local = pos_idx_pairs[k - 1][1]
            curr_local = pos_idx_pairs[k][1]
            if curr_local - prev_local <= 1:
                groups[-1].append(pos_idx_pairs[k][0])
            else:
                groups.append([pos_idx_pairs[k][0]])

        if len(groups) == 1:
            result.append(block)
            continue

        # Split: create sub-block for each contiguous group
        for group_positions in groups:
            sub = _build_scene_sub_block(block, local_texts, group_positions)
            if sub is not None:
                result.append(sub)

    return result


def _build_scene_sub_block(
    block: Dict,
    local_texts: List[Dict],
    positions: List[int],
) -> Optional[Dict]:
    """
    Build a sub-block from a parent block, keeping only the parts at the
    given positions (indices into the block's parallel arrays).
    """
    local_indices = list(block.get('_local_indices') or [])
    parts = list(block.get('parts') or [])
    part_texts = list(block.get('_part_texts') or [])
    pages = list(block.get('pages') or [])

    if not local_indices or not parts:
        return None

    # Sort positions to preserve their relative order within the block
    sorted_positions = sorted(positions)

    new_local_indices = [local_indices[p] for p in sorted_positions]
    new_parts = [dict(parts[p]) for p in sorted_positions]
    new_part_texts = [part_texts[p] for p in sorted_positions] if len(part_texts) == len(local_indices) else []

    # Rebuild pages: for each page, keep only parts that overlap with our positions
    position_set = set(sorted_positions)
    new_pages: List[Dict] = []
    new_part_cursor = 0
    for page in pages:
        page_start = int(page.get('part_start', 0))
        page_count = int(page.get('part_count', 0))
        page_end = page_start + page_count
        # Count how many of our positions fall within this page's range
        overlap_positions = [p for p in sorted_positions if page_start <= p < page_end]
        if not overlap_positions:
            continue
        new_page = {
            'part_start': new_part_cursor,
            'part_count': len(overlap_positions),
        }
        if 'marker_offset' in page:
            new_page['marker_offset'] = int(page['marker_offset'])
            new_page['marker_length'] = int(page.get('marker_length', 5))
            new_page['marker_raw'] = str(page.get('marker_raw') or '')
        new_pages.append(new_page)
        new_part_cursor += len(overlap_positions)

    if not new_local_indices:
        return None

    min_offset = min(int(local_texts[idx]['offset']) for idx in new_local_indices)
    max_end = max(int(local_texts[idx]['offset']) + int(local_texts[idx]['length']) for idx in new_local_indices)

    first_text = str(local_texts[new_local_indices[0]].get('text') or '')
    speaker_name = first_text if is_speaker_name(first_text) else None

    sub = dict(block)
    sub['offset'] = min_offset
    sub['length'] = max_end - min_offset
    sub['raw_hex'] = ''.join(str(local_texts[idx]['raw_hex']) for idx in new_local_indices)
    sub['text'] = '\n'.join(str(local_texts[idx]['text']) for idx in new_local_indices)
    sub['parts'] = new_parts
    sub['_local_indices'] = new_local_indices
    sub['_part_texts'] = new_part_texts
    sub['_speaker_name'] = speaker_name
    if new_pages:
        sub['pages'] = new_pages
    else:
        sub.pop('pages', None)
    return sub


def _merge_mgp_scene_blocks(scene_blocks: List[Dict]) -> List[Dict]:
    """
    Merge region-local dialogue fragments into real window blocks.

    `03 FF FF` starts many VM setup chunks, but treating every such chunk as a
    new visible window is garbage. A continuation chunk should stay inside the
    same window when:
    - the previous chunk ended exactly on a page boundary, and
    - the next chunk starts immediately after it, and
    - the next chunk does not begin with a fresh speaker/control setup, and
    - it does not introduce a different explicit speaker.

    Choice/branch setup leaves extra VM bytes after the last page; those chunks
    therefore stop merging naturally without text heuristics.
    """
    if len(scene_blocks) <= 1:
        return scene_blocks

    merged: List[Dict] = []
    current = scene_blocks[0]
    active_speaker = current.get('_speaker_name')

    for block in scene_blocks[1:]:
        block_speaker = block.get('_speaker_name')
        same_speaker_or_unspecified = (
            block_speaker is None
            or active_speaker is None
            or block_speaker == active_speaker
        )
        contiguous_region = int(current.get('_region_end', -1)) == int(block.get('_region_start', -2))
        can_merge = (
            bool(current.get('_tail_is_page_boundary'))
            and contiguous_region
            and not bool(block.get('_has_leading_speaker_opcode'))
            and not bool(block.get('_has_leading_setup_opcode'))
            and same_speaker_or_unspecified
            and current.get('label') == block.get('label')
        )

        if can_merge:
            current = _merge_scene_blocks(current, block)
            if block_speaker is not None:
                active_speaker = block_speaker
            continue

        merged.append(current)
        current = block
        active_speaker = block_speaker

    merged.append(current)
    return merged


def _normalize_mgp_block_speaker_name(block: Dict, known_speaker_names: Set[str]) -> Dict:
    part_texts = list(block.get('_part_texts') or [])
    first_text = part_texts[0].strip() if part_texts else ""
    block['_speaker_name'] = first_text if first_text in known_speaker_names else None
    return block


def _split_mgp_scene_block_on_known_speaker_pages(
    block: Dict,
    local_texts: List[Dict],
    known_speaker_names: Set[str],
) -> List[Dict]:
    pages = list(block.get('pages') or [])
    part_texts = list(block.get('_part_texts') or [])
    parts = list(block.get('parts') or [])
    local_indices = list(block.get('_local_indices') or [])
    if (
        len(pages) <= 1
        or not part_texts
        or len(part_texts) != len(local_indices)
        or len(parts) != len(local_indices)
    ):
        return [block]

    split_points: List[int] = []
    for prev_page, page in zip(pages, pages[1:]):
        part_start = int(page.get('part_start', 0))
        if not (0 < part_start < len(part_texts)):
            continue
        next_part = parts[part_start]
        next_offset = next_part.get('bytecode_offset')
        next_raw = str(next_part.get('bytecode_raw') or '')
        marker_offset = prev_page.get('marker_offset')
        marker_length = int(prev_page.get('marker_length', 5))
        has_window_gap = (
            marker_offset is not None
            and next_offset is not None
            and int(next_offset) > int(marker_offset) + marker_length
        )
        has_wrapped_page_start = next_raw.startswith('010200')
        if has_window_gap or has_wrapped_page_start:
            split_points.append(part_start)

    if not split_points:
        return [block]

    bounds = [0]
    bounds.extend(split_points)
    bounds.append(len(part_texts))

    split_blocks: List[Dict] = []
    for start_idx, end_idx in zip(bounds, bounds[1:]):
        sliced = _slice_mgp_scene_block(block, local_texts, start_idx, end_idx)
        if sliced is not None:
            split_blocks.append(sliced)
    return split_blocks or [block]


def _split_mgp_scene_blocks_on_known_speaker_pages(
    scene_blocks: List[Dict],
    local_texts: List[Dict],
    known_speaker_names: Set[str],
) -> List[Dict]:
    split_blocks: List[Dict] = []
    for block in scene_blocks:
        split_blocks.extend(
            _split_mgp_scene_block_on_known_speaker_pages(
                block,
                local_texts,
                known_speaker_names,
            )
        )
    return split_blocks


def _is_blank_local_text_entry(local_texts: List[Dict], local_idx: int) -> bool:
    if not (0 <= local_idx < len(local_texts)):
        return True
    text = str(local_texts[local_idx].get('text') or '')
    return not text.replace('　', '').strip()


def _is_suspicious_sparse_mgp_region(raw_pages: List[Dict], local_texts: List[Dict]) -> bool:
    page_marker_count = sum(1 for page in raw_pages if page.get('marker_offset') is not None)
    if page_marker_count != 1:
        return False

    valid_events: List[Dict] = []
    for page in raw_pages:
        for event in page.get('events', []):
            local_idx = int(event.get('local_idx', -1))
            if not (0 <= local_idx < len(local_texts)):
                continue
            valid_events.append(event)

    if len(valid_events) < 40:
        return False

    valid_events.sort(key=lambda event: int(event.get('bytecode_offset', -1)))
    large_gap_count = 0
    prev_end: Optional[int] = None
    text_ref_counts: Dict[int, int] = {}
    speaker_ref_counts: Dict[int, int] = {}

    for event in valid_events:
        bytecode_offset = int(event.get('bytecode_offset', -1))
        bytecode_length = int(event.get('bytecode_length', 0))
        if prev_end is not None and bytecode_offset - prev_end >= 100:
            large_gap_count += 1
        prev_end = bytecode_offset + bytecode_length

        local_idx = int(event.get('local_idx', -1))
        if event.get('kind') == 'speaker':
            speaker_ref_counts[local_idx] = speaker_ref_counts.get(local_idx, 0) + 1
            continue
        if _is_blank_local_text_entry(local_texts, local_idx):
            continue
        text_ref_counts[local_idx] = text_ref_counts.get(local_idx, 0) + 1

    if large_gap_count < 8 or not text_ref_counts:
        return False

    repeated_text_refs = sum(1 for count in text_ref_counts.values() if count >= 4)
    max_text_ref_count = max(text_ref_counts.values())
    max_speaker_ref_count = max(speaker_ref_counts.values()) if speaker_ref_counts else 0

    return (
        len(text_ref_counts) <= 8
        and repeated_text_refs >= 2
        and max_text_ref_count >= 10
        and max_speaker_ref_count >= 8
    )


def _extract_mgp_scene_texts(
    data: bytes,
    tbl: Dict[int, str],
    merge: bool,
    aggressive_no_kana_filter: bool = False,
) -> Optional[List[Dict]]:
    """
    按普通 MGP 的 scene 局部结构提取文本。

    已确认的结构特征：
    - 文件头 [0x02:0x04] 是 scene 偏移数量，随后是 LE uint32 偏移表
    - 每个 scene 自己带一层 `len + label + nested 7E00` 包装
    - nested 头里有稳定 section offsets
    - meta section 存内部标签表
    - text section 是 `u16 count + count个长度字节 + u16 total + records`
    - bytecode 中用 02 <u16 local_idx> 引用 scene 局部文本
    - 04 00 00 80 BF 是文本框边界

    这里按 scene 内的真实长度表建立 local_idx，不再用顺序扫描压缩文本池。
    """
    scene_offsets = _collect_mgp_scene_offsets(data)
    if not scene_offsets:
        return None

    # 旧扫描仅作为标签兜底；真正的文本边界不再信它。
    fallback_label_map = {
        entry['offset']: entry.get('label')
        for entry in extract_labeled_texts(data, tbl)
    }

    extracted_entries: List[Dict] = []
    covered_offsets: Set[int] = set()
    covered_offsets_with_prefix: Set[int] = set()

    def _extend_attachable_prefix(offset: int) -> int:
        new_start = offset
        while new_start >= 2 and data[new_start - 1] != 0x00:
            pb1 = data[new_start - 2]
            pb2 = data[new_start - 1]
            if pb1 == 0x81 and ((0x40 <= pb2 <= 0x7E) or (0x80 <= pb2 <= 0xFC)):
                new_start -= 2
                continue
            if pb1 == 0xFF:
                new_start -= 2
                continue
            break
        return new_start

    for scene_idx, scene_offset in enumerate(scene_offsets):
        scene_end = scene_offsets[scene_idx + 1] if scene_idx + 1 < len(scene_offsets) else len(data)
        scene = data[scene_offset:scene_end]

        scene_info = _parse_mgp_scene_wrapper(scene)
        if scene_info is None:
            continue

        nested = scene_info['nested']
        nested_offset = int(scene_info['nested_offset'])
        code_end = int(scene_info['code_end'])
        meta_start = int(scene_info['meta_start'])
        meta_end = int(scene_info['meta_end'])
        text_start = int(scene_info['text_start'])
        text_end = int(scene_info['text_end'])

        label_meta = _parse_mgp_scene_labels(nested, meta_start, meta_end)
        scene_labels = list(label_meta['scene_labels']) if label_meta is not None else []
        label_offsets = list(label_meta['label_offsets']) if label_meta is not None else []
        code_start = int(label_offsets[0]) if label_offsets else 0
        if not (0 <= code_start < code_end):
            code_start = 0
        default_label = fallback_label_map.get(scene_offset) or (scene_labels[0] if scene_labels else scene_info.get('scene_name'))
        local_texts = _parse_mgp_scene_texts(
            nested,
            text_start,
            text_end,
            scene_offset + nested_offset,
            tbl,
            fallback_label_map,
            default_label,
        )
        if not local_texts:
            continue

        if not merge:
            extracted_entries.extend(
                _build_entry(
                    offset=entry['offset'],
                    text=entry['text'],
                    raw_hex=entry['raw_hex'],
                    length=entry['length'],
                    label=entry.get('label'),
                )
                for entry in local_texts
            )
            continue

        code = nested[code_start:code_end]
        region_marks = [0]
        region_marks.extend(
            i for i in range(len(code) - 3)
            if code[i:i + 3] == b'\x03\xff\xff'
        )
        region_marks.append(len(code))

        scene_blocks: List[Dict] = []
        scene_known_speaker_names: Set[str] = set()
        textbox_marker = b'\x04\x00\x00\x80\xbf'
        for region_start, region_end in zip(region_marks, region_marks[1:]):
            region = code[region_start:region_end]
            raw_pages: List[Dict] = [{
                'text_refs': [],
                'speaker_refs': [],
                'events': [],
            }]
            last_event_end = 0
            last_event_kind: Optional[str] = None
            first_content_event_seen = False
            has_leading_speaker_opcode = False
            has_leading_setup_opcode = False
            i = 0
            while i < len(region) - 2:
                event = _parse_mgp_region_event(region, i, local_texts, textbox_marker)
                if event is None:
                    if not first_content_event_seen and _looks_like_mgp_leading_setup_opcode(region[i]):
                        has_leading_setup_opcode = True
                    i += 1
                    continue

                event_kind = str(event['kind'])
                event_length = int(event['length'])
                if event_kind == 'page':
                    current_page = raw_pages[-1]
                    current_page['marker_offset'] = scene_offset + nested_offset + code_start + region_start + i
                    current_page['marker_length'] = event_length
                    current_page['marker_raw'] = str(event['raw_hex'])
                    raw_pages.append({
                        'text_refs': [],
                        'speaker_refs': [],
                        'events': [],
                    })
                    last_event_end = i + event_length
                    last_event_kind = 'page'
                    i += event_length
                    continue
                local_idx = int(event.get('local_idx', -1))
                if event_kind == 'speaker':
                    if not first_content_event_seen:
                        has_leading_speaker_opcode = True
                    if 0 <= local_idx < len(local_texts):
                        if is_speaker_name(local_texts[local_idx]['text']):
                            scene_known_speaker_names.add(str(local_texts[local_idx]['text']))
                        raw_pages[-1]['speaker_refs'].append(local_idx)
                        raw_pages[-1]['events'].append({
                            'local_idx': local_idx,
                            'kind': 'speaker',
                            'bytecode_offset': scene_offset + nested_offset + code_start + region_start + i,
                            'bytecode_length': event_length,
                            'bytecode_raw': str(event['raw_hex']),
                        })
                    last_event_end = i + event_length
                    last_event_kind = 'speaker'
                    i += event_length
                    continue
                if event_kind == 'text':
                    if 0 <= local_idx < len(local_texts):
                        raw_pages[-1]['text_refs'].append(local_idx)
                        raw_pages[-1]['events'].append({
                            'local_idx': local_idx,
                            'kind': 'text',
                            'bytecode_offset': scene_offset + nested_offset + code_start + region_start + i,
                            'bytecode_length': event_length,
                            'bytecode_raw': str(event['raw_hex']),
                        })
                        last_event_end = i + event_length
                        last_event_kind = 'text'
                        if not is_speaker_name(local_texts[local_idx]['text']):
                            first_content_event_seen = True
                    i += event_length
                    continue

            if _is_suspicious_sparse_mgp_region(raw_pages, local_texts):
                continue

            normalized_pages: List[Dict] = []

            for raw_page in raw_pages:
                normalized_text_events = _normalize_mgp_page_text_events(
                    raw_page['events'],
                    local_texts,
                )
                ordered_indices, speaker_ref = _normalize_mgp_page_refs(
                    [int(event['local_idx']) for event in normalized_text_events],
                    raw_page['speaker_refs'],
                    local_texts,
                )
                if not ordered_indices:
                    continue

                content_only = [
                    local_idx for local_idx in ordered_indices
                    if local_idx != speaker_ref
                ]
                if not content_only:
                    continue

                normalized_pages.append({
                    'indices': ordered_indices,
                    'speaker_ref': speaker_ref,
                    'page': {
                        **raw_page,
                        'events': sorted(
                            [
                                dict(event)
                                for event in raw_page['events']
                                if event.get('kind') == 'speaker'
                            ] + normalized_text_events,
                            key=lambda event: int(event.get('bytecode_offset', -1)),
                        ),
                    },
                })

            if not normalized_pages:
                continue

            flattened_indices: List[int] = []
            group_parts: List[Dict] = []
            page_groups: List[Dict] = []
            for normalized_page in normalized_pages:
                page_indices = normalized_page['indices']
                speaker_ref = normalized_page['speaker_ref']
                page_events = normalized_page['page']['events']
                part_start = len(group_parts)
                for local_idx in page_indices:
                    flattened_indices.append(local_idx)
                    part_entry = {
                        'offset': local_texts[local_idx]['offset'],
                        'length': local_texts[local_idx]['length'],
                        'raw_hex': local_texts[local_idx]['raw_hex'],
                    }
                    event = _select_part_event(local_idx, speaker_ref, page_events)
                    if event is not None:
                        part_entry['bytecode_offset'] = int(event['bytecode_offset'])
                        part_entry['bytecode_length'] = int(event['bytecode_length'])
                        part_entry['bytecode_kind'] = str(event['kind'])
                        part_entry['bytecode_raw'] = str(event['bytecode_raw'])
                    group_parts.append(part_entry)
                page_group = {
                    'part_start': part_start,
                    'part_count': len(group_parts) - part_start,
                }
                marker_offset = normalized_page['page'].get('marker_offset')
                if marker_offset is not None:
                    page_group['marker_offset'] = int(marker_offset)
                    page_group['marker_length'] = int(normalized_page['page'].get('marker_length', 5))
                    page_group['marker_raw'] = str(normalized_page['page'].get('marker_raw') or "")
                page_groups.append(page_group)

            if flattened_indices and all(
                is_speaker_name(local_texts[local_idx]['text'])
                for local_idx in flattened_indices
            ):
                continue

            min_offset = min(int(local_texts[idx]['offset']) for idx in flattened_indices)
            max_end = max(int(local_texts[idx]['offset']) + int(local_texts[idx]['length']) for idx in flattened_indices)
            speaker_name = None
            first_line = local_texts[flattened_indices[0]]['text']
            if is_speaker_name(first_line):
                speaker_name = first_line

            scene_blocks.append({
                'offset': min_offset,
                'length': max_end - min_offset,
                'raw_hex': ''.join(local_texts[local_idx]['raw_hex'] for local_idx in flattened_indices),
                'text': '\n'.join(local_texts[local_idx]['text'] for local_idx in flattened_indices),
                'label': local_texts[flattened_indices[0]].get('label'),
                'parts': group_parts,
                'pages': page_groups,
                '_local_indices': flattened_indices,
                '_part_texts': [str(local_texts[local_idx]['text']) for local_idx in flattened_indices],
                '_region_offset': scene_offset + nested_offset + code_start + region_start,
                '_region_start': scene_offset + nested_offset + code_start + region_start,
                '_region_end': scene_offset + nested_offset + code_start + region_end,
                '_tail_is_page_boundary': last_event_kind == 'page' and last_event_end == len(region),
                '_speaker_name': speaker_name,
                '_has_leading_speaker_opcode': has_leading_speaker_opcode,
                '_has_leading_setup_opcode': has_leading_setup_opcode,
            })

        scene_blocks = _split_non_contiguous_scene_blocks(scene_blocks, local_texts)
        for block in scene_blocks:
            _normalize_mgp_block_speaker_name(block, scene_known_speaker_names)
        scene_blocks = _merge_mgp_scene_blocks(scene_blocks)
        scene_blocks = _split_mgp_scene_blocks_on_known_speaker_pages(
            scene_blocks,
            local_texts,
            scene_known_speaker_names,
        )
        for block in scene_blocks:
            _normalize_mgp_block_speaker_name(block, scene_known_speaker_names)
        garbage_lead_counts: Dict[str, int] = {}
        cleaned_scene_blocks: List[Dict] = []
        for block in scene_blocks:
            original_block = block
            block = _clean_mgp_scene_block(
                block,
                local_texts,
                aggressive_no_kana_filter=aggressive_no_kana_filter,
            )
            if block is None:
                token = _garbage_lead_token(original_block.get('text') or '')
                if token is not None:
                    garbage_lead_counts[token] = garbage_lead_counts.get(token, 0) + 1
                continue
            if _scene_block_looks_like_placeholder_garbage(block):
                token = _garbage_lead_token(block.get('text') or '')
                if token is not None:
                    garbage_lead_counts[token] = garbage_lead_counts.get(token, 0) + 1
                continue
            cleaned_scene_blocks.append(block)

        cleaned_scene_blocks = _dedup_subset_scene_blocks(cleaned_scene_blocks)
        cleaned_scene_blocks = _resolve_overlapping_indices(cleaned_scene_blocks, local_texts)

        scene_entry_index = 0
        for block in cleaned_scene_blocks:
            token = _garbage_lead_token(block.get('text') or '')
            first_part = (block.get('parts') or [{}])[0]
            lines = str(block.get('text') or '').splitlines()
            tail_text = '\n'.join(lines[1:]) if len(lines) > 1 else ''
            looks_like_bad_lead_text = (
                len(token or '') >= 4
                and not is_valid_text(token or '')
                and not any(
                    0x3040 <= ord(ch) <= 0x309F  # hiragana
                    or 0x30A0 <= ord(ch) <= 0x30FF  # katakana
                    or 0x4E00 <= ord(ch) <= 0x9FFF  # CJK (kanji / hanzi)
                    for ch in (token or '')
                )
            )
            if (
                len(lines) > 1
                and first_part.get('bytecode_kind') != 'speaker'
                and _part_has_real_scene_content(tail_text)
                and (
                    (token is not None and garbage_lead_counts.get(token, 0) >= 1)
                    or looks_like_bad_lead_text
                )
            ):
                trimmed = _slice_mgp_scene_block(block, local_texts, 1, len(block.get('_local_indices') or []))
                if trimmed is not None:
                    block = trimmed
            for local_idx in block['_local_indices']:
                local_offset = int(local_texts[local_idx]['offset'])
                covered_offsets.add(local_offset)
                covered_offsets_with_prefix.add(local_offset)
                covered_offsets_with_prefix.add(_extend_attachable_prefix(local_offset))
            extracted_entries.append(_build_entry(
                offset=block['offset'],
                text=block['text'],
                raw_hex=block['raw_hex'],
                length=block['length'],
                label=block.get('label'),
                parts=block.get('parts'),
                pages=block.get('pages'),
                original_offset=block.get('_region_offset'),
                bytecode_region_start=block.get('_region_start'),
                bytecode_region_end=block.get('_region_end'),
                scene_idx=scene_idx,
                scene_entry_index=scene_entry_index,
            ))
            scene_entry_index += 1
            block.pop('_local_indices', None)
            block.pop('_part_texts', None)
            block.pop('_region_offset', None)
            block.pop('_region_start', None)
            block.pop('_region_end', None)
            block.pop('_tail_is_page_boundary', None)
            block.pop('_speaker_name', None)
            block.pop('_has_leading_speaker_opcode', None)

        # Add unreferenced text records as standalone entries.
        # Some MGP scenes reference text records through opcodes not
        # recognised by the bytecode parser (e.g. menu/choice systems).
        # Emit them individually so they can be translated.
        referenced_local_indices: Set[int] = set()
        for block in cleaned_scene_blocks:
            for idx in block.get('_local_indices') or []:
                referenced_local_indices.add(idx)
        for local_idx in range(len(local_texts)):
            if local_idx in referenced_local_indices:
                continue
            lt = local_texts[local_idx]
            local_offset = int(lt['offset'])
            if local_offset in covered_offsets:
                continue
            text = str(lt.get('text') or '').strip()
            if not text:
                continue
            covered_offsets.add(local_offset)
            covered_offsets_with_prefix.add(local_offset)
            covered_offsets_with_prefix.add(_extend_attachable_prefix(local_offset))
            extracted_entries.append(_build_entry(
                offset=local_offset,
                text=text,
                raw_hex=str(lt.get('raw_hex') or ''),
                length=int(lt.get('length', 0)),
                label=default_label,
                parts=[{
                    'offset': local_offset,
                    'length': int(lt.get('length', 0)),
                    'raw_hex': str(lt.get('raw_hex') or ''),
                }],
                scene_idx=scene_idx,
                scene_entry_index=scene_entry_index,
            ))
            scene_entry_index += 1

    fallback_entries = []
    for entry in extract_labeled_texts(data, tbl):
        entry_offset = int(entry['offset'])
        extended_offset = _extend_attachable_prefix(entry_offset)
        if entry_offset in covered_offsets or entry_offset in covered_offsets_with_prefix:
            continue
        if extended_offset in covered_offsets or extended_offset in covered_offsets_with_prefix:
            continue
        if merge and is_speaker_name(entry['text']):
            continue
        if _fallback_entry_is_low_quality(
            entry,
            aggressive_no_kana_filter=aggressive_no_kana_filter,
        ):
            continue
        fallback_entries.append(entry)
    extracted_entries.extend(fallback_entries)

    if not extracted_entries:
        return None

    extracted_entries.sort(key=lambda entry: entry['offset'])
    return extracted_entries


_EVE_SLOT_TEXT_FAMS = {0x01: '01'}
_EVE_BODY_TEXT_FAMS = {0x02: '02', 0x55: '55', 0x72: '72'}
_EVE_TEXTLIKE_FAMILIES = {'01', '02', '55', '72'}
_EVE_BODY_TEXT_FAMILIES = {'02', '55', '72'}
_EVE_FIXED_SKIP_LENGTHS = {
    # Verified from default.xbe jump-table handlers:
    # 0x11 = 1 word, 0x12 = 1 word, 0x13 = 2 words,
    # 0x07 = 2 words + 5 floats, 0x08 = 2 words, 0x09 = 3 words + 5 floats,
    # 0x45 = 2 words + float + byte + byte, 0x4B = 2 words + float,
    # 0x6B = 3 words. None of these handlers call the local-text converter.
    0x11: 3,
    0x12: 3,
    0x13: 5,
    0x07: 25,
    0x08: 5,
    0x09: 27,
    0x45: 11,
    0x4B: 9,
    0x6B: 7,
}
_EVE_SAME_PAGE_MAX_GAP = 16
_EVE_SETUP_STARTERS_AFTER_PAGE: Set[int] = {
    0x10, 0x11, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A,
    0x1E, 0x2E, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4C,
    0x53, 0x54, 0x56, 0x71, 0x73,
}


def _load_eve_inline_speaker_names() -> Set[str]:
    module_path = Path(__file__).with_name('eve_inline_speaker_table.py')
    if not module_path.exists():
        return set()
    spec = importlib.util.spec_from_file_location('_eve_inline_speaker_table', module_path)
    if spec is None or spec.loader is None:
        return set()
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    table = getattr(module, 'EVE_INLINE_SPEAKER_TABLE', None)
    if not isinstance(table, dict):
        return set()
    return {
        str(key)
        for key in table.keys()
        if isinstance(key, str) and key
    }


_EVE_INLINE_SPEAKER_NAMES = _load_eve_inline_speaker_names()


@dataclass
class _EveToken:
    kind: str
    start: int
    end: int
    family: str = ''
    local_idx: int = -1
    slot_id: int = -1
    extra_value: int = -1
    text: str = ''
    name_like: bool = False
    role_hint: str = ''


def _build_eve_scenes(data: bytes, tbl: Dict[int, str]) -> List[Dict[str, object]]:
    scene_offsets = _collect_mgp_scene_offsets(data)
    if not scene_offsets:
        return []

    fallback_label_map = {
        entry['offset']: entry.get('label')
        for entry in extract_labeled_texts(data, tbl)
    }

    scenes: List[Dict[str, object]] = []
    for scene_idx, scene_offset in enumerate(scene_offsets):
        scene_end = scene_offsets[scene_idx + 1] if scene_idx + 1 < len(scene_offsets) else len(data)
        raw_scene = data[scene_offset:scene_end]
        scene_info = _parse_mgp_scene_wrapper(raw_scene)
        scene_dict: Dict[str, object] = {
            'scene_idx': scene_idx,
            'scene_start': scene_offset,
            'scene_end': scene_end,
            'parsed': scene_info is not None,
            'scene_name': '',
            'labels': [],
            'label_offsets': [],
            'texts': [],
            'nested_offset': 0,
            'code_start': 0,
            'code_end': 0,
            'code': b'',
        }
        if scene_info is None:
            scenes.append(scene_dict)
            continue

        nested = scene_info['nested']
        meta = _parse_mgp_scene_labels(
            nested,
            int(scene_info['meta_start']),
            int(scene_info['meta_end']),
        )
        scene_labels = list(meta['scene_labels']) if meta is not None else []
        raw_label_offsets = list(meta['label_offsets']) if meta is not None else []
        code_start = int(raw_label_offsets[0]) if raw_label_offsets else 0
        if not (0 <= code_start < int(scene_info['code_end'])):
            code_start = 0
        label_offsets = [max(0, int(offset) - code_start) for offset in raw_label_offsets]
        default_label = (
            fallback_label_map.get(scene_offset)
            or (scene_labels[0] if scene_labels else scene_info.get('scene_name'))
        )
        local_texts = _parse_mgp_scene_texts(
            nested,
            int(scene_info['text_start']),
            int(scene_info['text_end']),
            scene_offset + int(scene_info['nested_offset']),
            tbl,
            fallback_label_map,
            default_label,
        ) or []

        scene_dict.update({
            'scene_name': str(scene_info.get('scene_name') or ''),
            'nested_offset': int(scene_info['nested_offset']),
            'nested': nested,
            'code_start': code_start,
            'code_end': int(scene_info['code_end']),
            'meta_start': int(scene_info['meta_start']),
            'meta_end': int(scene_info['meta_end']),
            'text_start': int(scene_info['text_start']),
            'text_end': int(scene_info['text_end']),
            'code': nested[code_start:int(scene_info['code_end'])],
            'labels': scene_labels,
            'label_offsets': label_offsets,
            'texts': local_texts,
        })
        scenes.append(scene_dict)

    return scenes


def _scene_label_for_offset(scene: Dict[str, object], rel_code_off: int) -> Optional[str]:
    labels = scene.get('labels') or []
    offsets = scene.get('label_offsets') or []
    scene_name = scene.get('scene_name')
    if (
        isinstance(labels, list)
        and isinstance(offsets, list)
        and labels
        and len(labels) == len(offsets)
    ):
        idx = bisect.bisect_right(offsets, rel_code_off) - 1
        if idx < 0:
            return str(labels[0])
        return str(labels[idx])
    return str(scene_name) if scene_name else None


def _split_eve_regions(code: bytes) -> List[Tuple[int, int]]:
    marks = [0]
    marks.extend(i for i in range(len(code) - 2) if code[i:i + 3] == b'\x03\xff\xff')
    marks.append(len(code))
    out: List[Tuple[int, int]] = []
    for start, end in zip(marks, marks[1:]):
        if end > start:
            out.append((start, end))
    return out


def _maybe_skip_eve_setup(region: bytes, pos: int) -> int:
    remain = len(region) - pos
    first = region[pos]
    fixed = _EVE_FIXED_SKIP_LENGTHS.get(first)
    if fixed is not None and remain >= fixed:
        return fixed
    if first == 0x15:
        # default.xbe op 0x15:
        #   dword mode
        #   operand A: tag + (0 or 4 bytes) + compare byte
        #   operand B: tag + (0 or 4 bytes) + compare byte
        # The handler may convert local_text into scratch buffers for string
        # comparison, but it never routes text into the dialogue buffers used
        # by 0x01/0x02/0x55/0x72.
        cur = pos + 1
        if cur + 4 > len(region):
            return 0
        cur += 4
        for _ in range(2):
            if cur >= len(region):
                return 0
            tag = region[cur]
            cur += 1
            if tag != 0:
                if cur + 4 > len(region):
                    return 0
                cur += 4
            if cur >= len(region):
                return 0
            cur += 1
        return cur - pos
    if remain >= 11 and first == 0x14:
        return 11
    if remain >= 5 and first in (0x46, 0x54):
        return 5
    if remain >= 3 and first in (
        0x53, 0x56, 0x71, 0x73,
        0x0D, 0x0E, 0x10,
        0x16, 0x17, 0x18, 0x19, 0x1A,
        0x1E, 0x2E,
    ):
        return 3
    if remain >= 5 and region[pos:pos + 5].startswith(b'\x04') and region[pos:pos + 5] != b'\x04\x00\x00\x80\xbf':
        return 5
    if remain >= 27 and first == 0x47 and region[pos + 25:pos + 27] == b'\x01\x01':
        return 27
    if remain >= 23 and first == 0x48 and region[pos + 21:pos + 23] in (b'\x01\x01', b'\x00\x01'):
        return 23
    return 0


def _scan_eve_region_tokens(region: bytes, text_entries: List[Dict[str, object]]) -> List[_EveToken]:
    n_local = len(text_entries)
    tokens: List[_EveToken] = []
    pos = 0
    while pos < len(region):
        if pos + 3 <= len(region) and region[pos:pos + 3] == b'\x03\xff\xff':
            pos += 3
            continue
        if pos + 5 <= len(region) and region[pos:pos + 5] == b'\x04\x00\x00\x80\xbf':
            tokens.append(_EveToken(kind='page', start=pos, end=pos + 5))
            pos += 5
            continue

        skip = _maybe_skip_eve_setup(region, pos)
        if skip:
            pos += skip
            continue

        if pos + 7 <= len(region) and region[pos] in _EVE_SLOT_TEXT_FAMS:
            slot_id = struct.unpack_from('<H', region, pos + 1)[0]
            idx = struct.unpack_from('<H', region, pos + 3)[0]
            extra_value = struct.unpack_from('<H', region, pos + 5)[0]
            if 0 <= idx < n_local:
                text = str(text_entries[idx]['text'])
                name_like = bool(is_speaker_name(text))
                tokens.append(_EveToken(
                    kind='event',
                    start=pos,
                    end=pos + 7,
                    family=_EVE_SLOT_TEXT_FAMS[region[pos]],
                    local_idx=idx,
                    slot_id=slot_id,
                    extra_value=extra_value,
                    text=text,
                    name_like=name_like,
                    role_hint='speaker' if name_like else 'text',
                ))
                pos += 7
                continue

        if pos + 3 <= len(region) and region[pos] in _EVE_BODY_TEXT_FAMS:
            idx = struct.unpack_from('<H', region, pos + 1)[0]
            if 0 <= idx < n_local:
                text = str(text_entries[idx]['text'])
                name_like = bool(is_speaker_name(text))
                role_hint = 'speaker' if name_like else 'text'
                tokens.append(_EveToken(
                    kind='event',
                    start=pos,
                    end=pos + 3,
                    family=_EVE_BODY_TEXT_FAMS[region[pos]],
                    local_idx=idx,
                    text=text,
                    name_like=name_like,
                    role_hint=role_hint,
                ))
                pos += 3
                continue

        pos += 1

    return tokens


def _score_eve_event(
    ev: _EveToken,
    *,
    first_in_page: bool = False,
    next_ev: Optional[_EveToken] = None,
) -> int:
    text = ev.text.strip()
    if not text:
        return -5
    if ev.family == '72':
        return 3

    score = 0
    if ev.role_hint == 'speaker':
        score += 2 if ev.name_like else 1
    else:
        score += 3 if not ev.name_like else 1

    if ev.family == '01':
        if first_in_page:
            score += 1
        elif not ev.name_like:
            score -= 1

    if ev.family in ('02', '55') and ev.name_like:
        if first_in_page and next_ev is not None and not next_ev.name_like:
            score += 2
        else:
            score -= 1
    return score


def _validate_eve_cluster(cluster: Dict[str, object]) -> Tuple[bool, int]:
    pages = cluster['pages']
    events = [ev for page in pages for ev in page]
    if not events:
        return False, 0

    textlike = [ev for ev in events if ev.family in _EVE_TEXTLIKE_FAMILIES]
    body_events = [
        ev for ev in events
        if ev.family == '72' or (ev.family in _EVE_BODY_TEXT_FAMILIES and not ev.name_like)
    ]

    score = 0
    for page in pages:
        for idx, ev in enumerate(page):
            next_ev = page[idx + 1] if idx + 1 < len(page) else None
            score += _score_eve_event(ev, first_in_page=(idx == 0), next_ev=next_ev)

    score += len(body_events) * 2 + (len(pages) - 1) * 2
    if not textlike:
        score -= 12
    if not body_events:
        score -= 10
    if len(events) == 1:
        score -= 4
    if len(events) == 2 and all(ev.name_like for ev in events):
        score -= 6

    return score >= 8 and bool(textlike) and bool(body_events), score


def _build_eve_clusters(tokens: List[_EveToken], region: bytes) -> List[Dict[str, object]]:
    clusters: List[Dict[str, object]] = []
    idx = 0
    while idx < len(tokens):
        while idx < len(tokens) and tokens[idx].kind != 'event':
            idx += 1
        if idx >= len(tokens):
            break

        start_idx = idx
        pages: List[List[_EveToken]] = [[]]
        j = idx
        while j < len(tokens):
            tok = tokens[j]
            if tok.kind != 'event':
                break
            pages[-1].append(tok)
            j += 1
            if j >= len(tokens):
                break

            next_tok = tokens[j]
            if next_tok.kind == 'page':
                after = next_tok.end
                k = j + 1
                while k < len(tokens) and tokens[k].kind != 'event':
                    k += 1
                if k >= len(tokens):
                    j = k
                    break
                next_ev = tokens[k]
                gap_bytes = region[after:next_ev.start]
                if len(gap_bytes) == 0 or (len(gap_bytes) <= 2 and all(b == 0 for b in gap_bytes)):
                    pages.append([])
                    j = k
                    continue
                if gap_bytes and gap_bytes[0] in _EVE_SETUP_STARTERS_AFTER_PAGE:
                    break
                if len(gap_bytes) <= 3 and gap_bytes[0] not in _EVE_SETUP_STARTERS_AFTER_PAGE:
                    pages.append([])
                    j = k
                    continue
                break

            gap = next_tok.start - tok.end
            if gap <= _EVE_SAME_PAGE_MAX_GAP:
                continue
            break

        if pages and not pages[-1]:
            pages.pop()
        if pages and any(pages):
            clusters.append({
                'start_token_index': start_idx,
                'end_token_index': j,
                'pages': pages,
            })
        idx = max(j, start_idx + 1)

    return clusters


def _classify_eve_cluster_raw_parts(cluster: Dict[str, object]) -> Tuple[List[Dict[str, object]], bool]:
    pages = cluster['pages']
    raw_parts: List[Dict[str, object]] = []
    explicit_speaker_seen = False

    for page_idx, page in enumerate(pages):
        body_started = False
        page_speaker_text: Optional[str] = None
        for ev in page:
            role = 'text'
            speaker_whitelisted = ev.text in _EVE_INLINE_SPEAKER_NAMES
            if ev.family == '01':
                if not body_started:
                    if page_speaker_text is None and (speaker_whitelisted or ev.name_like):
                        role = 'speaker'
                        page_speaker_text = ev.text
                    elif page_speaker_text is not None and ev.text == page_speaker_text:
                        role = 'speaker'
                    else:
                        role = 'text'
                        body_started = True
                else:
                    role = 'text'
            elif ev.family == '72':
                role = 'text'
                body_started = True
            elif not body_started:
                if ev.family in _EVE_TEXTLIKE_FAMILIES:
                    # Text-family name lines only look like real speaker labels on
                    # the entry's first page; later pages often use the same
                    # encoding for vocatives like "ムビオラ".
                    allow_text_family_speaker = (
                        page_idx == 0
                        and page_speaker_text is None
                        and speaker_whitelisted
                    )
                    if allow_text_family_speaker:
                        role = 'speaker'
                        page_speaker_text = ev.text
                    elif page_speaker_text is not None and ev.text == page_speaker_text:
                        role = 'speaker'
                    else:
                        role = 'text'
                        body_started = True
                else:
                    role = 'text'
            else:
                role = 'text'

            if role == 'speaker':
                explicit_speaker_seen = True

            raw_parts.append({
                'event': ev,
                'page_idx': page_idx,
                'role': role,
            })

    return raw_parts, explicit_speaker_seen


def _merge_eve_display_lines(raw_parts: List[Dict[str, object]]) -> Tuple[List[str], List[str], List[int]]:
    display_lines: List[str] = []
    display_roles: List[str] = []
    part_line_map: List[int] = []
    page_body_started = defaultdict(bool)
    last_page: Optional[int] = None
    last_role: Optional[str] = None
    last_text: Optional[str] = None

    for raw_part in raw_parts:
        ev = raw_part['event']
        page_idx = int(raw_part['page_idx'])
        role = str(raw_part['role'])
        text = str(ev.text)

        merge = False
        if display_lines and page_idx == last_page:
            if role == last_role and text == last_text and not page_body_started[page_idx]:
                merge = True

        if merge:
            part_line_map.append(len(display_lines) - 1)
        else:
            display_lines.append(text)
            display_roles.append(role)
            part_line_map.append(len(display_lines) - 1)
            last_page = page_idx
            last_role = role
            last_text = text

        if role == 'text':
            page_body_started[page_idx] = True
        last_page = page_idx
        last_role = role
        last_text = text

    return display_lines, display_roles, part_line_map


def _cluster_to_eve_entry(
    scene: Dict[str, object],
    cluster: Dict[str, object],
    region_abs_start: int,
    region_abs_end: int,
    *,
    inherited_speaker: Optional[str] = None,
) -> Dict[str, object]:
    raw_parts, explicit_speaker = _classify_eve_cluster_raw_parts(cluster)
    display_lines, display_roles, part_line_map = _merge_eve_display_lines(raw_parts)

    parts: List[Dict[str, object]] = []
    pages: List[Dict[str, object]] = []
    raw_part_cursor = 0
    code = scene['code']
    code_abs_start = int(scene['scene_start']) + int(scene['nested_offset']) + int(scene['code_start'])

    for page_idx, page in enumerate(cluster['pages']):
        part_start = raw_part_cursor
        for ev in page:
            raw_part = raw_parts[raw_part_cursor]
            local_text = scene['texts'][ev.local_idx]
            rel_bc_start = int(cluster['region_start']) + ev.start
            rel_bc_end = int(cluster['region_start']) + ev.end
            parts.append({
                'offset': int(local_text['offset']),
                'length': int(local_text['length']),
                'raw_hex': str(local_text['raw_hex']),
                'bytecode_offset': code_abs_start + rel_bc_start,
                'bytecode_length': rel_bc_end - rel_bc_start,
                'bytecode_kind': 'speaker' if raw_part['role'] == 'speaker' else 'text',
                'bytecode_raw': code[rel_bc_start:rel_bc_end].hex(),
            })
            raw_part_cursor += 1

        page_info: Dict[str, object] = {
            'part_start': part_start,
            'part_count': raw_part_cursor - part_start,
        }
        last_ev = page[-1]
        marker_rel = int(cluster['region_start']) + last_ev.end
        if marker_rel + 5 <= int(cluster['region_end']) and code[marker_rel:marker_rel + 5] == b'\x04\x00\x00\x80\xbf':
            page_info['marker_offset'] = code_abs_start + marker_rel
            page_info['marker_length'] = 5
            page_info['marker_raw'] = '04000080bf'
        pages.append(page_info)

    speaker: Optional[str] = None
    speaker_source: Optional[str] = None
    for line, role in zip(display_lines, display_roles):
        if role == 'speaker':
            speaker = line
            speaker_source = 'parts'
            break

    allow_inherited_speaker = (
        not explicit_speaker
        and bool(raw_parts)
        and raw_parts[0]['event'].family in _EVE_TEXTLIKE_FAMILIES
    )
    if speaker is None and inherited_speaker and allow_inherited_speaker:
        speaker = inherited_speaker
        speaker_source = 'inherited'

    body_lines = [line for line, role in zip(display_lines, display_roles) if role != 'speaker']
    text_lines = list(display_lines)
    if speaker and speaker_source == 'inherited':
        text_lines = [speaker] + text_lines

    first_bytecode_offset = int(parts[0]['bytecode_offset']) if parts else region_abs_start
    last_bytecode_end = (
        int(parts[-1]['bytecode_offset']) + int(parts[-1]['bytecode_length'])
        if parts else region_abs_end
    )
    rel_first = int(cluster['region_start']) + cluster['pages'][0][0].start
    label = _scene_label_for_offset(scene, rel_first)

    return {
        'offset': first_bytecode_offset,
        'length': max(0, last_bytecode_end - first_bytecode_offset),
        'raw_hex': ''.join(str(part['raw_hex']) for part in parts),
        'text': '\n'.join(text_lines),
        'label': label,
        'parts': parts,
        'pages': pages,
        'original_offset': first_bytecode_offset,
        'bytecode_region_start': region_abs_start,
        'bytecode_region_end': region_abs_end,
        'body_text': '\n'.join(body_lines),
        'speaker': speaker,
        'speaker_source': speaker_source,
        'part_line_map': part_line_map,
        'storage_jp_lines': display_lines,
        'storage_line_roles': display_roles,
    }


def _extract_eve_entries(data: bytes, scenes: List[Dict[str, object]]) -> List[Dict[str, object]]:
    entries: List[Dict[str, object]] = []
    for scene in scenes:
        if not scene.get('parsed'):
            continue
        local_texts = scene.get('texts') or []
        if not local_texts:
            continue

        code = scene['code']
        regions = _split_eve_regions(code)
        scene_abs_code_start = int(scene['scene_start']) + int(scene['nested_offset']) + int(scene['code_start'])

        previous_region_speaker: Optional[str] = None
        scene_entries: List[Dict[str, object]] = []
        for region_start, region_end in regions:
            region = code[region_start:region_end]
            tokens = _scan_eve_region_tokens(region, local_texts)
            clusters = _build_eve_clusters(tokens, region)

            valid_clusters: List[Dict[str, object]] = []
            for cluster in clusters:
                is_valid, score = _validate_eve_cluster(cluster)
                if not is_valid:
                    continue
                cluster = dict(cluster)
                cluster['score'] = score
                cluster['region_start'] = region_start
                cluster['region_end'] = region_end
                valid_clusters.append(cluster)

            valid_clusters.sort(key=lambda candidate: candidate['pages'][0][0].start)
            current_speaker = previous_region_speaker
            for cluster in valid_clusters:
                entry = _cluster_to_eve_entry(
                    scene,
                    cluster,
                    scene_abs_code_start + region_start,
                    scene_abs_code_start + region_end,
                    inherited_speaker=current_speaker,
                )
                scene_entries.append(entry)
                if entry.get('speaker'):
                    current_speaker = str(entry['speaker'])
            previous_region_speaker = current_speaker

        scene_entries.sort(key=lambda entry: int(entry['offset']))
        for scene_entry_index, entry in enumerate(scene_entries):
            entry['scene_idx'] = int(scene['scene_idx'])
            entry['scene_entry_index'] = scene_entry_index
        entries.extend(scene_entries)

    entries.sort(key=lambda entry: int(entry['offset']))
    return entries


def _is_0x81_prefix_gap(gap_bytes: bytes) -> bool:
    """gap が null(1)+0x81XX対(N) のみで構成されていればTrue。
    RL.mgp等で全角空格インデント（0x8140）が検出範囲外のため
    gap>1になる継続行を正しくマージするために使用。"""
    if len(gap_bytes) < 3 or gap_bytes[0] != 0x00:
        return False
    i = 1
    while i + 1 < len(gap_bytes):
        if gap_bytes[i] == 0x81 and ((0x40 <= gap_bytes[i + 1] <= 0x7E) or (0x80 <= gap_bytes[i + 1] <= 0xFC)):
            i += 2
        else:
            return False
    return i == len(gap_bytes)


def merge_texts_smart(texts: List[Dict], data: bytes, is_mgs: bool, tbl: Dict[int, str]) -> List[Dict]:
    """
    格式感知的智能合并——将null终止字符串合并为完整文本框。

    MGS脚本有两种编译变体（经全部21个MGS文件验证）：

    变体1 — FF04嵌入文本流（aku_00-06, 08-15, ride_on/off）：
      编译器在文本区域的文本框边界处插入 FF04 控制码。
      规则：gap字节中含 FF04 或 FF07 → 不合并（文本框/选择分支边界）
            gap ≤ 50 且无上述控制码 → 合并（同一文本框内的多行）
      准确率：对FF04边界 99.6-99.9%，零漏检。

    变体2 — 纯字节码模式（aku_07, aku_teki, aku_yuko）：
      编译器不在文本流中嵌入 FF04。文本框推进完全由VM字节码指令控制：
        02 XX XX (LE uint16) = 按0基索引引用文本条目
        04 00 00 80 BF = 文本框推进/清除边界
      两个连续02引用之间没有04边界 = 同一文本框。
      规则：解析字节码分组，同组条目合并。若字节码解析失败则回退gap=1。

    MGP规则：仅合并gap=1。

    检测方法：扫描文本区域中FF04出现次数。阈值 >10 区分两种变体。
    每个合并条目包含 parts 数组，用于翻译后按行写回。
    """
    if not texts:
        return []

    sorted_texts = sorted(texts, key=lambda x: x['offset'])

    # 对MGS文件，先检测文本区域内是否有FF04
    use_ff04_merge = False
    bytecode_groups = None
    if is_mgs and sorted_texts:
        text_start = sorted_texts[0]['offset']
        text_end = sorted_texts[-1]['offset'] + sorted_texts[-1]['length']
        ff04_count = 0
        i = text_start
        while i < text_end - 1:
            if data[i] == 0xFF and data[i + 1] == 0x04:
                ff04_count += 1
                i += 2
            else:
                i += 1
        if ff04_count > 10:
            use_ff04_merge = True
        else:
            # 非FF04变体：使用字节码文本框分组
            bytecode_groups = _parse_mgs_bytecode_groups(data, tbl)

    merged = []
    current_parts = []

    def flush_current():
        if not current_parts:
            return
        combined_text = '\n'.join(p['text'] for p in current_parts)
        combined_raw = ''.join(p['raw_hex'] for p in current_parts)
        first = current_parts[0]
        last = current_parts[-1]
        entry = {
            'offset': first['offset'],
            'length': last['offset'] + last['length'] - first['offset'],
            'raw_hex': combined_raw,
            'text': combined_text,
            'label': first.get('label'),
        }
        if len(current_parts) > 1:
            entry['parts'] = [
                {'offset': p['offset'], 'length': p['length'], 'raw_hex': p['raw_hex']}
                for p in current_parts
            ]
        merged.append(entry)

    for t in sorted_texts:
        if not current_parts:
            current_parts = [t]
            continue

        prev = current_parts[-1]
        prev_end = prev['offset'] + prev['length']
        gap = t['offset'] - prev_end

        should_merge = False

        if gap <= 0:
            should_merge = False
        elif use_ff04_merge:
            # 变体1: 除非gap含FF04/FF07或过大，否则合并
            if gap > 50:
                should_merge = False
            else:
                gap_bytes = data[prev_end:t['offset']]
                has_ff04 = b'\xff\x04' in gap_bytes
                has_ff07 = b'\xff\x07' in gap_bytes
                should_merge = not has_ff04 and not has_ff07
        elif bytecode_groups is not None:
            # 变体2: 字节码分组——同组则合并
            prev_group = bytecode_groups.get(prev['offset'])
            curr_group = bytecode_groups.get(t['offset'])
            should_merge = (prev_group is not None and
                            curr_group is not None and
                            prev_group == curr_group)
        else:
            # 非MGS（RL.mgp等）: 仅合并gap全由 null+0x81XX对 组成的情况
            # (全角空格缩进/】分隔的续行)。不合并gap=1，因为gap=1
            # 会跨说话人边界链式合并整个对话场景
            if 1 < gap <= 50:
                should_merge = _is_0x81_prefix_gap(data[prev_end:t['offset']])

        if should_merge:
            current_parts.append(t)
        else:
            flush_current()
            current_parts = [t]

    flush_current()
    return merged


def decode_sjis_with_tbl(data: bytes, tbl: Dict[int, str]) -> str:
    """
    使用TBL码表解码SJIS编码的字节数据
    """
    result = []
    i = 0
    while i < len(data):
        b1 = data[i]

        # 检查是否是双字节SJIS范围
        if (0x81 <= b1 <= 0x9F) or (0xE0 <= b1 <= 0xFC):
            if i + 1 < len(data):
                b2 = data[i + 1]
                code = (b1 << 8) | b2

                if code in tbl:
                    result.append(tbl[code])
                else:
                    # 尝试CP932解码
                    try:
                        char = bytes([b1, b2]).decode('cp932')
                        result.append(char)
                    except:
                        result.append(f'[{code:04X}]')
                i += 2
            else:
                result.append(f'[{b1:02X}]')
                i += 1
        elif b1 == 0x00:
            # 空字节结束
            break
        elif b1 == 0xFF:
            # FF 参数是一个变量控制对；0A／0D 参数不能解释为换行。
            if i + 1 < len(data):
                result.append(f'<FF><{data[i + 1]:02X}>')
                i += 2
            else:
                result.append('<FF>')
                i += 1
        elif 0x20 <= b1 <= 0x7E:
            # ASCII可打印字符
            result.append(chr(b1))
            i += 1
        elif b1 == 0x0A:
            result.append('\n')
            i += 1
        elif b1 == 0x0D:
            i += 1  # 跳过CR
        else:
            # 控制码或其他
            result.append(f'<{b1:02X}>')
            i += 1

    return ''.join(result)


def extract_text_blocks_from_mgp(data: bytes, tbl: Dict[int, str]) -> List[Dict]:
    """
    从MGP文件提取文本块
    MGP文件格式：
    - 开头是 ~ 0x00 后跟偏移表
    - 文本通常在文件后半部分，以特定模式开始
    """
    texts = []

    # 查找所有可能的日文文本区域
    i = 0
    while i < len(data) - 2:
        b1 = data[i]

        # 检查是否是SJIS日文字符开头
        if (0x82 <= b1 <= 0x84) or (0x88 <= b1 <= 0x9F) or (0xE0 <= b1 <= 0xEA):
            if i + 1 < len(data):
                b2 = data[i + 1]
                # 验证是有效的SJIS第二字节
                if (0x40 <= b2 <= 0x7E) or (0x80 <= b2 <= 0xFC):
                    # 找到可能的文本开始，向后扫描到结束
                    start = i
                    end = i

                    while end < len(data):
                        if data[end] == 0x00:
                            break
                        end += 1

                    # 至少2字节
                    if end - start >= 2:
                        text_bytes = data[start:end]
                        text = decode_sjis_with_tbl(text_bytes, tbl)

                        # 过滤：至少包含一个日文字符
                        if any(ord(c) > 0x3000 for c in text):
                            texts.append({
                                'offset': start,
                                'length': end - start,
                                'raw': text_bytes.hex(),
                                'text': text
                            })

                    i = end + 1
                    continue
        i += 1

    texts = _normalize_speaker_metadata(texts, data, tbl, file_path)
    for entry in texts:
        if not isinstance(entry, dict):
            continue
        if entry.get('storage_line_roles'):
            continue
        storage_line_roles = _build_storage_line_roles(
            entry,
            speaker=str(entry.get('speaker') or '') or None,
            speaker_source=str(entry.get('speaker_source') or '') or None,
        )
        if storage_line_roles:
            entry['storage_line_roles'] = storage_line_roles
    for entry in texts:
        if not isinstance(entry, dict):
            continue
        if entry.get('storage_line_roles'):
            continue
        storage_line_roles = _build_storage_line_roles(
            entry,
            speaker=str(entry.get('speaker') or '') or None,
            speaker_source=str(entry.get('speaker_source') or '') or None,
        )
        if storage_line_roles:
            entry['storage_line_roles'] = storage_line_roles

    for entry in texts:
        if not isinstance(entry, dict):
            continue
        if entry.get('storage_line_roles'):
            continue
        storage_line_roles = _build_storage_line_roles(
            entry,
            speaker=str(entry.get('speaker') or '') or None,
            speaker_source=str(entry.get('speaker_source') or '') or None,
        )
        if storage_line_roles:
            entry['storage_line_roles'] = storage_line_roles

    texts = _normalize_speaker_metadata(texts, data, tbl, file_path)
    for entry in texts:
        if not isinstance(entry, dict):
            continue
        storage_jp_lines = entry.get('storage_jp_lines')
        storage_line_roles = entry.get('storage_line_roles')
        if (
            not isinstance(storage_jp_lines, list)
            or not isinstance(storage_line_roles, list)
            or not storage_jp_lines
            or len(storage_jp_lines) != len(storage_line_roles)
        ):
            continue
        normalized_roles: List[str] = []
        valid = True
        for raw_role in storage_line_roles:
            role = str(raw_role or '')
            if role not in ('speaker', 'text'):
                valid = False
                break
            normalized_roles.append(role)
        if not valid:
            continue

        speaker = str(entry.get('speaker') or '')
        body_lines = [
            str(line)
            for line, role in zip(storage_jp_lines, normalized_roles)
            if role != 'speaker'
        ]
        body_text = '\n'.join(body_lines)
        entry['body_text'] = body_text
        if speaker:
            entry['text'] = speaker + ('\n' + body_text if body_text else '')
        else:
            entry['text'] = body_text
    return texts


def extract_labeled_texts(data: bytes, tbl: Dict[int, str]) -> List[Dict]:
    """
    提取带标签的文本（如角色名 + 对话）
    格式通常是: 角色名 空格 对话内容
    """
    texts = []

    # 在文件中搜索文本模式
    # MGP/MGS文件的文本通常在文件末尾附近

    # 首先找到标签区域（START, AFTER_MOVIE等）
    label_pattern = rb'[A-Z][A-Z_0-9]+\x00'

    # 查找所有日文文本
    i = 0
    current_label = None

    while i < len(data) - 2:
        # 检查是否是标签
        if data[i:i+5] == b'START' or data[i:i+5] == b'AFTER':
            # 找标签结尾
            end = i
            while end < len(data) and data[end] != 0:
                end += 1
            try:
                current_label = data[i:end].decode('ascii')
            except:
                pass
            i = end + 1
            continue

        b1 = data[i]

        # FF前缀控制码记录检测（如 FF 0A を、FF 04 だ 等）
        # 与 apply_scripts_json.find_sequential_text_entries() 逻辑一致
        if b1 == 0xFF and i + 3 < len(data):
            start = i
            end = i
            while end < len(data) and data[end] != 0x00:
                end += 1
            # 检查记录中是否包含有效 SJIS 文本
            has_sjis = False
            probe = start
            while probe < end - 1:
                p1 = data[probe]
                if (0x82 <= p1 <= 0x84) or (0x88 <= p1 <= 0x9F) or (0xE0 <= p1 <= 0xEA):
                    p2 = data[probe + 1]
                    if (0x40 <= p2 <= 0x7E) or (0x80 <= p2 <= 0xFC):
                        has_sjis = True
                        break
                probe += 1
            if has_sjis and end - start >= 3:
                text_bytes = data[start:end]
                text = decode_sjis_with_tbl(text_bytes, tbl)
                texts.append({
                    'offset': start,
                    'length': end - start,
                    'raw_hex': text_bytes.hex(),
                    'text': text,
                    'label': current_label
                })
                i = end + 1
                continue

        # SJIS日文字符检测
        if (0x82 <= b1 <= 0x84) or (0x88 <= b1 <= 0x9F) or (0xE0 <= b1 <= 0xEA):
            if i + 1 < len(data):
                b2 = data[i + 1]
                if (0x40 <= b2 <= 0x7E) or (0x80 <= b2 <= 0xFC):
                    start = i
                    end = i

                    while end < len(data) and data[end] != 0x00:
                        end += 1

                    if end - start >= 4:  # 至少2个双字节字符
                        text_bytes = data[start:end]
                        text = decode_sjis_with_tbl(text_bytes, tbl)

                        # 包含日文且是有效文本
                        if is_valid_text(text):
                            texts.append({
                                'offset': start,
                                'length': end - start,
                                'raw_hex': text_bytes.hex(),
                                'text': text,
                                'label': current_label
                            })

                    i = end + 1
                    continue
        i += 1

    return texts


def parse_script_file(file_path: Path, tbl: Dict[int, str], merge: bool = True) -> List[Dict]:
    """
    解析单个脚本文件

    参数:
        file_path: 脚本文件路径
        tbl: 字符码表
        merge: 是否合并相邻文本块（True=智能合并, False=不合并）
    """
    if (
        merge
        and file_path.suffix.lower() == '.mgp'
        and file_path.name.upper() == 'EVE.MGP'
    ):
        data = file_path.read_bytes()
        scenes = _build_eve_scenes(data, tbl)
        return _extract_eve_entries(data, scenes)

    data = file_path.read_bytes()
    is_mgs = file_path.suffix.lower() == '.mgs'
    is_mgp = file_path.suffix.lower() == '.mgp'

    texts: Optional[List[Dict]] = None
    if is_mgp and file_path.name.upper() != 'RL.MGP':
        texts = _extract_mgp_scene_texts(
            data,
            tbl,
            merge=merge,
            aggressive_no_kana_filter=False,
        )

    if texts is None:
        texts = extract_labeled_texts(data, tbl)
        if merge:
            texts = merge_texts_smart(texts, data, is_mgs, tbl)

    # 后处理：扩展被跳过的前缀字节
    # 1) 0x81XX 标点（『』「」…～等）
    # 2) 连续 FFxx 占位/变量控制码（例如 <FF><01><FF><02>クン）
    # 放在 merge 之后，避免改变 offset 影响 merge 的 gap/bytecode 分组计算。
    # 对合并条目的每个 part 都做后向扫描（不只是第一个 part）。
    # 保存 original_offset 供 compare_scripts 匹配用。
    def _extend_0x81(offset, length):
        """从 offset 向前扫描可附着的 0x81XX/FFxx 前缀，返回 (new_offset, new_length)"""
        new_start = offset
        while new_start >= 2 and data[new_start - 1] != 0x00:
            pb1 = data[new_start - 2]
            pb2 = data[new_start - 1]
            if pb1 == 0x81 and ((0x40 <= pb2 <= 0x7E) or (0x80 <= pb2 <= 0xFC)):
                new_start -= 2
            elif pb1 == 0xFF:
                new_start -= 2
            else:
                break
        if new_start < offset:
            return new_start, length + (offset - new_start)
        return offset, length

    for entry in texts:
        if 'parts' in entry:
            # 合并条目：对每个part做0x81扩展
            any_extended = False
            for p in entry['parts']:
                new_off, new_len = _extend_0x81(p['offset'], p['length'])
                if new_off < p['offset']:
                    p['offset'] = new_off
                    p['length'] = new_len
                    p['raw_hex'] = data[new_off:new_off + new_len].hex()
                    any_extended = True
            if any_extended:
                # 重建合并条目的text/offset/length/raw_hex
                entry['original_offset'] = entry['offset']
                entry['offset'] = entry['parts'][0]['offset']
                last_p = entry['parts'][-1]
                entry['length'] = last_p['offset'] + last_p['length'] - entry['offset']
                entry['raw_hex'] = ''.join(p['raw_hex'] for p in entry['parts'])
                part_texts = []
                for p in entry['parts']:
                    p_bytes = data[p['offset']:p['offset'] + p['length']]
                    part_texts.append(decode_sjis_with_tbl(p_bytes, tbl))
                entry['text'] = '\n'.join(part_texts)
        else:
            # 非合并条目：直接扩展
            new_off, new_len = _extend_0x81(entry['offset'], entry['length'])
            if new_off < entry['offset']:
                entry['original_offset'] = entry['offset']
                entry['offset'] = new_off
                entry['length'] = new_len
                entry['raw_hex'] = data[new_off:new_off + new_len].hex()
                entry['text'] = decode_sjis_with_tbl(data[new_off:new_off + new_len], tbl)

    for entry in texts:
        if not isinstance(entry, dict):
            continue
        if entry.get('storage_line_roles'):
            continue
        storage_line_roles = _build_storage_line_roles(
            entry,
            speaker=str(entry.get('speaker') or '') or None,
            speaker_source=str(entry.get('speaker_source') or '') or None,
        )
        if storage_line_roles:
            entry['storage_line_roles'] = storage_line_roles

    texts = _normalize_speaker_metadata(texts, data, tbl, file_path)
    for entry in texts:
        if not isinstance(entry, dict):
            continue
        storage_jp_lines = entry.get('storage_jp_lines')
        storage_line_roles = entry.get('storage_line_roles')
        if (
            not isinstance(storage_jp_lines, list)
            or not isinstance(storage_line_roles, list)
            or not storage_jp_lines
            or len(storage_jp_lines) != len(storage_line_roles)
        ):
            continue
        normalized_roles: List[str] = []
        valid = True
        for raw_role in storage_line_roles:
            role = str(raw_role or '')
            if role not in ('speaker', 'text'):
                valid = False
                break
            normalized_roles.append(role)
        if not valid:
            continue

        speaker = str(entry.get('speaker') or '')
        body_lines = [
            str(line)
            for line, role in zip(storage_jp_lines, normalized_roles)
            if role != 'speaker'
        ]
        body_text = '\n'.join(body_lines)
        entry['body_text'] = body_text
        if speaker:
            entry['text'] = speaker + ('\n' + body_text if body_text else '')
        else:
            entry['text'] = body_text
    return texts


def compare_scripts(
    script_texts: List[Dict],
    scriptf_texts: List[Dict],
    force_sequence_alignment: bool = False,
) -> Dict:
    """
    比较Script和Scriptf的文本，找出相同和不同的部分
    返回: {
        'common': [...],  # 相同的文本
        'script_only': [...],  # 仅Script有
        'scriptf_only': [...],  # 仅Scriptf有
        'different': [...]  # 同位置但内容不同
    }
    """
    result = {
        'common': [],
        'script_only': [],
        'scriptf_only': [],
        'different': []
    }

    def copy_structural_fields(src: Dict, dst: Dict, prefix: str = '') -> None:
        for key in (
            'length',
            'parts',
            'pages',
            'storage_jp_lines',
            'storage_line_roles',
            'part_line_map',
            'original_offset',
            'bytecode_region_start',
            'bytecode_region_end',
            'scene_idx',
            'scene_entry_index',
            'mid_speakers',
            'speaker',
            'speaker_source',
            'body_text',
        ):
            if key in src:
                dst[f'{prefix}{key}'] = src[key]

    def append_common(s_text: Dict, sf_text: Dict) -> None:
        common_entry = {
            'offset': s_text['offset'],
            'script_offset': s_text['offset'],
            'scriptf_offset': sf_text['offset'],
            'text': s_text['text'],
            'script_text': s_text['text'],
            'scriptf_text': sf_text['text'],
            'raw_hex': s_text['raw_hex'],
            'script_raw': s_text['raw_hex'],
            'scriptf_raw': sf_text['raw_hex'],
            'label': s_text.get('label'),
        }
        copy_structural_fields(s_text, common_entry)
        copy_structural_fields(s_text, common_entry, 'script_')
        copy_structural_fields(sf_text, common_entry, 'scriptf_')
        result['common'].append(common_entry)

    def append_different(s_text: Dict, sf_text: Dict) -> None:
        diff_entry = {
            'offset': s_text['offset'],
            'script_offset': s_text['offset'],
            'scriptf_offset': sf_text['offset'],
            'script_text': s_text['text'],
            'scriptf_text': sf_text['text'],
            'script_length': s_text.get('length', 0),
            'scriptf_length': sf_text.get('length', 0),
            'script_raw': s_text['raw_hex'],
            'scriptf_raw': sf_text['raw_hex'],
            'label': s_text.get('label'),
        }
        copy_structural_fields(s_text, diff_entry)
        copy_structural_fields(s_text, diff_entry, 'script_')
        copy_structural_fields(sf_text, diff_entry, 'scriptf_')
        result['different'].append(diff_entry)

    def placeholder_content_part_count(entry: Dict) -> int:
        parts = entry.get('parts') or []
        if not isinstance(parts, list) or not parts:
            return 0

        first_line = str(entry.get('text') or "").splitlines()[0] if str(entry.get('text') or "") else ""
        speaker_offset = None
        if is_speaker_name(first_line):
            for part in parts:
                if part.get('bytecode_kind') == 'speaker':
                    speaker_offset = part.get('offset')
                    break

        placeholder_count = 0
        for part in parts:
            if not isinstance(part, dict):
                continue
            if speaker_offset is not None and part.get('offset') == speaker_offset:
                continue
            raw_hex = str(part.get('raw_hex') or "")
            if not raw_hex:
                continue
            try:
                raw = bytes.fromhex(raw_hex)
            except ValueError:
                continue
            if raw and all(byte == 0x00 for byte in raw):
                placeholder_count += 1
        return placeholder_count

    def scene_key(entry: Dict) -> Optional[int]:
        raw_scene_idx = entry.get('scene_idx')
        return int(raw_scene_idx) if isinstance(raw_scene_idx, int) else None

    def part_opcode_family(part: Dict) -> str:
        bc_raw = str(part.get('bytecode_raw') or '')
        bc_kind = str(part.get('bytecode_kind') or '')
        if bc_kind == 'speaker':
            return 'speaker'
        if bc_raw.startswith('01') and len(bc_raw) >= 4 and bc_raw[2:4] in ('07', '08', '09'):
            return 'wrapped_speaker'
        if bc_raw[:2] in ('07', '08', '09'):
            return 'bare_speaker'
        if bc_raw.startswith('01') and len(bc_raw) >= 4 and bc_raw[2:4] == '02':
            return 'wrapped_text'
        if bc_raw.startswith('02'):
            return 'text'
        return bc_kind or bc_raw[:2]

    def exact_mgp_signature(entry: Dict) -> Tuple:
        parts = entry.get('parts') or []
        if not isinstance(parts, list) or not parts:
            return ('no_parts', str(entry.get('text') or ''))

        pages = entry.get('pages') or []
        mid_speakers = entry.get('mid_speakers') or []
        return (
            tuple(int(page.get('part_count', 0)) for page in pages if isinstance(page, dict)),
            tuple(part_opcode_family(part) for part in parts if isinstance(part, dict)),
            tuple(
                int(mid.get('part_index', -1))
                for mid in mid_speakers
                if isinstance(mid, dict)
            ),
            bool(entry.get('speaker')),
            str(entry.get('speaker_source') or ''),
            placeholder_content_part_count(entry),
            len(parts),
        )

    def exact_lcs_alignment(
        left_entries: List[Dict],
        right_entries: List[Dict],
    ) -> List[Tuple[str, Optional[Dict], Optional[Dict]]]:
        if not left_entries:
            return [('right_only', None, entry) for entry in right_entries]
        if not right_entries:
            return [('left_only', entry, None) for entry in left_entries]

        left_sig = [exact_mgp_signature(entry) for entry in left_entries]
        right_sig = [exact_mgp_signature(entry) for entry in right_entries]
        left_len = len(left_entries)
        right_len = len(right_entries)
        dp = [[0] * (right_len + 1) for _ in range(left_len + 1)]

        for left_idx in range(left_len - 1, -1, -1):
            for right_idx in range(right_len - 1, -1, -1):
                if left_sig[left_idx] == right_sig[right_idx]:
                    dp[left_idx][right_idx] = dp[left_idx + 1][right_idx + 1] + 1
                else:
                    dp[left_idx][right_idx] = max(
                        dp[left_idx + 1][right_idx],
                        dp[left_idx][right_idx + 1],
                    )

        aligned: List[Tuple[str, Optional[Dict], Optional[Dict]]] = []
        left_idx = 0
        right_idx = 0
        while left_idx < left_len and right_idx < right_len:
            if left_sig[left_idx] == right_sig[right_idx]:
                aligned.append(('paired', left_entries[left_idx], right_entries[right_idx]))
                left_idx += 1
                right_idx += 1
                continue
            if dp[left_idx + 1][right_idx] >= dp[left_idx][right_idx + 1]:
                aligned.append(('left_only', left_entries[left_idx], None))
                left_idx += 1
                continue
            aligned.append(('right_only', None, right_entries[right_idx]))
            right_idx += 1

        while left_idx < left_len:
            aligned.append(('left_only', left_entries[left_idx], None))
            left_idx += 1
        while right_idx < right_len:
            aligned.append(('right_only', None, right_entries[right_idx]))
            right_idx += 1
        return aligned

    # 按original_offset对齐比较（有0x81前缀扩展时用原始offset匹配）
    def match_key(t):
        return t.get('original_offset', t['offset'])

    use_sequence_alignment = force_sequence_alignment

    if use_sequence_alignment:
        script_by_scene: Dict[Optional[int], List[Dict]] = {}
        scriptf_by_scene: Dict[Optional[int], List[Dict]] = {}
        scene_order: List[Optional[int]] = []
        seen_scenes: Set[Optional[int]] = set()

        for entry in script_texts:
            key = scene_key(entry)
            script_by_scene.setdefault(key, []).append(entry)
            if key not in seen_scenes:
                seen_scenes.add(key)
                scene_order.append(key)

        for entry in scriptf_texts:
            key = scene_key(entry)
            scriptf_by_scene.setdefault(key, []).append(entry)
            if key not in seen_scenes:
                seen_scenes.add(key)
                scene_order.append(key)

        for key in scene_order:
            aligned = exact_lcs_alignment(
                script_by_scene.get(key, []),
                scriptf_by_scene.get(key, []),
            )
            for kind, s_text, sf_text in aligned:
                if kind == 'paired':
                    if s_text is None or sf_text is None:
                        continue
                    if s_text['text'] == sf_text['text']:
                        append_common(s_text, sf_text)
                    else:
                        append_different(s_text, sf_text)
                elif kind == 'left_only':
                    if s_text is not None:
                        result['script_only'].append(s_text)
                elif kind == 'right_only':
                    if sf_text is not None:
                        result['scriptf_only'].append(sf_text)
        return result

    script_by_offset = {match_key(t): t for t in script_texts}
    scriptf_by_offset = {match_key(t): t for t in scriptf_texts}

    all_offsets = set(script_by_offset.keys()) | set(scriptf_by_offset.keys())

    for offset in sorted(all_offsets):
        s_text = script_by_offset.get(offset)
        sf_text = scriptf_by_offset.get(offset)

        if s_text and sf_text:
            if s_text['text'] == sf_text['text']:
                append_common(s_text, sf_text)
            else:
                append_different(s_text, sf_text)
        elif s_text:
            result['script_only'].append(s_text)
        else:
            result['scriptf_only'].append(sf_text)

    return result


def extract_all_scripts(base_dir: Path, tbl: Dict[int, str], output_dir: Path, merge: bool = True):
    """
    提取所有脚本文件

    参数:
        merge: 是否合并相邻文本块为完整对话
    """
    script_dir = base_dir / 'Media' / 'Script'
    scriptf_dir = base_dir / 'Media' / 'Scriptf'

    output_dir.mkdir(parents=True, exist_ok=True)

    # 获取所有脚本文件
    script_files = list(script_dir.glob('*.mg*'))

    all_results = {}

    for script_file in sorted(script_files):
        name = script_file.name
        scriptf_file = scriptf_dir / name

        print(f"处理: {name}")

        # 解析两个版本
        script_texts = parse_script_file(script_file, tbl, merge=merge)

        if scriptf_file.exists():
            scriptf_texts = parse_script_file(scriptf_file, tbl, merge=merge)
            comparison = compare_scripts(
                script_texts,
                scriptf_texts,
                force_sequence_alignment=(script_file.suffix.lower() == '.mgp' and script_file.name.upper() != 'RL.MGP'),
            )
        else:
            comparison = {
                'common': script_texts,
                'script_only': [],
                'scriptf_only': [],
                'different': []
            }

        all_results[name] = comparison

        # 保存单个文件的结果
        output_file = output_dir / f"{name}.json"
        with open(output_file, 'w', encoding='utf-8') as f:
            json.dump(comparison, f, ensure_ascii=False, indent=2)

        # 统计
        print(f"  相同: {len(comparison['common'])}")
        print(f"  不同: {len(comparison['different'])}")
        print(f"  仅Script: {len(comparison['script_only'])}")
        print(f"  仅Scriptf: {len(comparison['scriptf_only'])}")

    # 保存汇总
    summary_file = output_dir / '_summary.json'
    summary = {
        'total_files': len(all_results),
        'files': {}
    }
    for name, comp in all_results.items():
        # 计算该文件的最大行长度（用于翻译时换行）
        max_line_len = 0
        for item in comp['common'] + comp['script_only'] + comp['scriptf_only']:
            text = item.get('text', '')
            for line in text.split('\n'):
                # 移除控制码后计算实际显示长度
                clean_line = re.sub(r'<[0-9A-F]{2}>', '', line)
                clean_line = re.sub(r'\[[0-9A-F]{4}\]', '', clean_line)
                if len(clean_line) > max_line_len:
                    max_line_len = len(clean_line)
        for item in comp['different']:
            for key in ['script_text', 'scriptf_text']:
                text = item.get(key, '')
                for line in text.split('\n'):
                    clean_line = re.sub(r'<[0-9A-F]{2}>', '', line)
                    clean_line = re.sub(r'\[[0-9A-F]{4}\]', '', clean_line)
                    if len(clean_line) > max_line_len:
                        max_line_len = len(clean_line)

        summary['files'][name] = {
            'common': len(comp['common']),
            'different': len(comp['different']),
            'script_only': len(comp['script_only']),
            'scriptf_only': len(comp['scriptf_only']),
            'max_line_length': max_line_len  # 最长行的字符数
        }

    with open(summary_file, 'w', encoding='utf-8') as f:
        json.dump(summary, f, ensure_ascii=False, indent=2)

    return all_results


def generate_translation_template(results: Dict, output_dir: Path):
    """
    生成翻译模板文件
    格式:
    - common.txt: 共同文本（只需翻译一次）
    - different.txt: 不同文本（需要分别翻译）
    """
    common_file = output_dir / 'translation_common.txt'
    different_file = output_dir / 'translation_different.txt'

    # 收集所有共同文本（去重）
    common_texts = set()
    different_texts = []

    for name, comp in results.items():
        for item in comp['common']:
            common_texts.add(item['text'])

        for item in comp['different']:
            different_texts.append({
                'file': name,
                'offset': item['offset'],
                'script': item['script_text'],
                'scriptf': item['scriptf_text']
            })

    # 写入共同文本模板
    with open(common_file, 'w', encoding='utf-8') as f:
        f.write("# SMT NINE 共同文本翻译模板\n")
        f.write("# 格式: 日文|||中文\n")
        f.write("# 请在|||后面填写中文翻译\n\n")

        for text in sorted(common_texts):
            # 保留换行符
            text_escaped = text.replace('\n', '\\n')
            f.write(f"{text_escaped}|||\n")

    # 写入不同文本模板
    with open(different_file, 'w', encoding='utf-8') as f:
        f.write("# SMT NINE 差异文本翻译模板\n")
        f.write("# Script=男主角(パトリック), Scriptf=女主角(スミレ)\n")
        f.write("# 格式: \n")
        f.write("# [FILE:文件名]\n")
        f.write("# @OFFSET:偏移\n")
        f.write("# SCRIPT:日文|||中文\n")
        f.write("# SCRIPTF:日文|||中文\n\n")

        current_file = None
        for item in different_texts:
            if item['file'] != current_file:
                current_file = item['file']
                f.write(f"\n[FILE:{current_file}]\n")

            f.write(f"@OFFSET:{item['offset']:08X}\n")
            script_escaped = item['script'].replace('\n', '\\n')
            scriptf_escaped = item['scriptf'].replace('\n', '\\n')
            f.write(f"SCRIPT:{script_escaped}|||\n")
            f.write(f"SCRIPTF:{scriptf_escaped}|||\n")

    print(f"\n翻译模板已生成:")
    print(f"  共同文本: {common_file} ({len(common_texts)} 条)")
    print(f"  差异文本: {different_file} ({len(different_texts)} 条)")


def main():
    import argparse

    parser = argparse.ArgumentParser(description='SMT NINE 脚本提取工具')
    parser.add_argument('--base-dir', type=Path,
                        default=Path(__file__).parent.parent / 'backup',
                        help='游戏根目录（默认backup）')
    parser.add_argument('--tbl', type=Path,
                        default=None,
                        help='TBL码表文件路径')
    parser.add_argument('--output', type=Path,
                        default=Path(__file__).parent / 'extracted',
                        help='输出目录')
    parser.add_argument('--no-merge', action='store_true',
                        help='不合并相邻文本块（按换行拆分）')

    args = parser.parse_args()

    # 查找TBL文件
    if args.tbl is None:
        # 尝试默认位置
        possible_tbls = [
            args.base_dir / 'Media' / 'SysFont' / 'sysfont.tbl',
            args.base_dir / 'sysfont.tbl',
            Path(__file__).parent.parent / 'font_pipeline' / 'sysfont.tbl',
        ]
        for p in possible_tbls:
            if p.exists():
                args.tbl = p
                break

    if args.tbl is None or not args.tbl.exists():
        print("错误: 找不到TBL码表文件")
        print("请使用 --tbl 参数指定路径")
        return

    print(f"使用码表: {args.tbl}")
    tbl = load_tbl(args.tbl)
    print(f"  加载了 {len(tbl)} 个字符映射")

    # 提取脚本
    merge = not args.no_merge
    print(f"  合并相邻文本: {'是' if merge else '否'}")
    results = extract_all_scripts(args.base_dir, tbl, args.output, merge=merge)

    # 生成翻译模板
    generate_translation_template(results, args.output)

    print("\n完成!")


if __name__ == '__main__':
    main()
