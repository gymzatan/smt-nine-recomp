from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path
from typing import Any

from rts_scripts_common import (
    decode_inf_blob,
    encode_inf_blob,
    encode_visible_text,
    load_pack_entries,
    load_text_tbl,
    parse_inf_messages,
    rebuild_pack,
    text_to_body_bytes,
)


SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
BACKUP_INPUT = ROOT_DIR / "backup" / "Media" / "RTS" / "Scripts" / "RTSScript.bin"
TRANSLATED_INPUT = SCRIPT_DIR / "translated_rts" / "rts_messages.json"
OUTPUT_PATH = SCRIPT_DIR / "output" / "RTS" / "Scripts" / "RTSScript.bin"
LIVE_PATH = ROOT_DIR / "Media" / "RTS" / "Scripts" / "RTSScript.bin"
TBL_CANDIDATES = [
    ROOT_DIR / "font_pipeline" / "final_game_f24_full.tbl",
    ROOT_DIR / "font_pipeline" / "final_game_f24.tbl",
    ROOT_DIR / "font_pipeline" / "sysfont_cn.tbl",
]


def resolve_tbl_file() -> Path:
    for candidate in TBL_CANDIDATES:
        if candidate.exists():
            return candidate
    raise FileNotFoundError("no RTS writeback TBL file found")


def load_translation_index(path: Path) -> dict[str, dict[int, dict[str, Any]]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    index: dict[str, dict[int, dict[str, Any]]] = {}
    for file_info in data.get("files", []):
        file_name = str(file_info.get("name") or "")
        index[file_name] = {}
        for message in file_info.get("messages", []):
            message_index = int(message.get("message_index") or 0)
            index[file_name][message_index] = message
    return index


def load_menu_strings(path: Path) -> list[dict[str, str]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    menu = data.get("menu_strings", {})
    return menu.get("strings", [])


def apply_menu_translations(
    decoded: bytes,
    menu_strings: list[dict[str, str]],
    char_map: dict[str, bytes],
) -> tuple[bytes, int]:
    pairs: list[tuple[bytes, bytes]] = []
    for entry in menu_strings:
        jp_text = str(entry.get("text") or "")
        cn_text = str(entry.get("chinese") or "")
        if not jp_text or not cn_text:
            continue
        jp_bytes = b'"' + jp_text.encode("cp932") + b'"'
        cn_bytes = b'"' + encode_visible_text(cn_text, char_map) + b'"'
        pairs.append((jp_bytes, cn_bytes))

    pairs.sort(key=lambda p: -len(p[0]))

    result = decoded
    changed = 0
    for jp_bytes, cn_bytes in pairs:
        if jp_bytes in result:
            result = result.replace(jp_bytes, cn_bytes)
            changed += 1

    return result, changed


def apply_translations_to_inf(
    file_name: str,
    decoded: bytes,
    translations: dict[int, dict[str, Any]],
    char_map: dict[str, bytes],
) -> tuple[bytes, int]:
    messages = parse_inf_messages(decoded)
    if not messages:
        return decoded, 0

    out = bytearray()
    cursor = 0
    changed = 0

    for source in messages:
        out.extend(decoded[cursor:source["body_start"]])
        cursor = source["body_end"]

        translated = translations.get(int(source["message_index"]))
        if translated is None:
            out.extend(source["body_bytes"])
            continue

        source_text = str(source["text"])
        translated_text = str(translated.get("text") or "")
        if translated_text != source_text:
            raise ValueError(
                f"RTS source mismatch: {file_name} message_index={source['message_index']} "
                f"text changed between extraction and writeback"
            )

        chinese = str(translated.get("chinese") or "")
        if not chinese:
            out.extend(source["body_bytes"])
            continue

        replacement = text_to_body_bytes(chinese, char_map)
        out.extend(replacement)
        changed += 1

    out.extend(decoded[cursor:])
    return bytes(out), changed


def build_output(
    source_path: Path,
    translated_path: Path,
    output_path: Path,
    *,
    dry_run: bool,
) -> tuple[int, int, int]:
    prefix, entries = load_pack_entries(source_path)
    translation_index = load_translation_index(translated_path)
    menu_strings = load_menu_strings(translated_path)
    char_map = load_text_tbl(resolve_tbl_file())

    changed_files = 0
    changed_messages = 0
    menu_changed = 0

    rebuilt_entries: list[dict[str, Any]] = []
    for entry in entries:
        rebuilt = dict(entry)
        if entry["kind"] == "inf" and str(entry["name"]).endswith(".inf"):
            if str(entry["name"]) == "menu.inf" and menu_strings:
                decoded = decode_inf_blob(entry["blob"])
                rewritten, mc = apply_menu_translations(decoded, menu_strings, char_map)
                rebuilt["blob"] = encode_inf_blob(rewritten)
                menu_changed = mc
            elif str(entry["name"]) in translation_index:
                decoded = decode_inf_blob(entry["blob"])
                rewritten, changed = apply_translations_to_inf(
                    str(entry["name"]),
                    decoded,
                    translation_index[str(entry["name"])],
                    char_map,
                )
                rebuilt["blob"] = encode_inf_blob(rewritten)
                if changed:
                    changed_files += 1
                    changed_messages += changed
        rebuilt_entries.append(rebuilt)

    rebuilt_pack = rebuild_pack(prefix, rebuilt_entries)
    if not dry_run:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(rebuilt_pack)
    return changed_files, changed_messages, menu_changed


def main() -> None:
    parser = argparse.ArgumentParser(description="Apply translated RTS Message bodies back into RTSScript.bin")
    parser.add_argument("--source", type=Path, default=BACKUP_INPUT, help="Baseline RTSScript.bin")
    parser.add_argument("--translated", type=Path, default=TRANSLATED_INPUT, help="Editable RTS translation JSON")
    parser.add_argument("--output", type=Path, default=OUTPUT_PATH, help="Output RTSScript.bin path")
    parser.add_argument("--dry-run", action="store_true", help="Parse and validate only; do not write output")
    parser.add_argument("--deploy-live", action="store_true", help="Also copy output RTSScript.bin into Media/RTS/Scripts")
    args = parser.parse_args()

    changed_files, changed_messages, menu_changed = build_output(
        args.source,
        args.translated,
        args.output,
        dry_run=args.dry_run,
    )

    print(f"source={args.source}")
    print(f"translated={args.translated}")
    print(f"changed_files={changed_files}")
    print(f"changed_messages={changed_messages}")
    print(f"menu_strings_applied={menu_changed}")
    print(f"output={args.output}")

    if args.deploy_live and not args.dry_run:
        LIVE_PATH.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(args.output, LIVE_PATH)
        print(f"deployed={LIVE_PATH}")


if __name__ == "__main__":
    main()
