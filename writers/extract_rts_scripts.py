from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from rts_scripts_common import decode_inf_blob, load_pack_entries, parse_inf_messages, parse_menu_inf_strings


SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
DEFAULT_INPUT = ROOT_DIR / "backup" / "Media" / "RTS" / "Scripts" / "RTSScript.bin"
DEFAULT_EXTRACTED_DIR = SCRIPT_DIR / "extracted_rts"
DEFAULT_TRANSLATED_DIR = SCRIPT_DIR / "translated_rts"
DEFAULT_REPORT = ROOT_DIR / "rts_script_investigation.md"


def build_index(pack_path: Path) -> dict[str, Any]:
    _, entries = load_pack_entries(pack_path)
    files: list[dict[str, Any]] = []
    message_total = 0
    menu_strings_data: dict[str, Any] | None = None

    for entry in entries:
        if entry["kind"] != "inf" or not str(entry["name"]).endswith(".inf"):
            continue

        decoded = decode_inf_blob(entry["blob"])

        if str(entry["name"]) == "menu.inf":
            jp_strings = parse_menu_inf_strings(decoded)
            menu_strings_data = {
                "file_name": "menu.inf",
                "strings": [{"text": s, "chinese": ""} for s in jp_strings],
            }
            continue

        messages = parse_inf_messages(decoded)
        if not messages:
            continue

        normalized_messages: list[dict[str, Any]] = []
        for message in messages:
            normalized_messages.append(
                {
                    "message_index": message["message_index"],
                    "label": message["label"],
                    "start_line": message["start_line"],
                    "command": message["command"],
                    "speaker_id": message["speaker_id"],
                    "window_y": message["window_y"],
                    "window_width": message["window_width"],
                    "text": message["text"],
                    "body_text": message["body_text"],
                    "chinese": "",
                }
            )

        message_total += len(normalized_messages)
        files.append(
            {
                "name": entry["name"],
                "size": entry["size"],
                "message_count": len(normalized_messages),
                "messages": normalized_messages,
            }
        )

    result: dict[str, Any] = {
        "source": str(pack_path),
        "file_count": len(files),
        "message_total": message_total,
        "files": files,
    }
    if menu_strings_data:
        result["menu_strings"] = menu_strings_data
    return result


def build_translation_map(index: dict[str, Any]) -> dict[tuple[str, int, str], str]:
    mapping: dict[tuple[str, int, str], str] = {}
    for file_info in index.get("files", []):
        file_name = str(file_info.get("name") or "")
        for message in file_info.get("messages", []):
            key = (
                file_name,
                int(message.get("message_index") or 0),
                str(message.get("text") or ""),
            )
            mapping[key] = str(message.get("chinese") or "")
    return mapping


def build_translated_index(extracted: dict[str, Any], existing: dict[str, Any] | None) -> dict[str, Any]:
    translated = json.loads(json.dumps(extracted, ensure_ascii=False))
    existing_map = build_translation_map(existing or {})

    for file_info in translated.get("files", []):
        file_name = str(file_info.get("name") or "")
        for message in file_info.get("messages", []):
            key = (
                file_name,
                int(message.get("message_index") or 0),
                str(message.get("text") or ""),
            )
            message["chinese"] = existing_map.get(key, "")

    if "menu_strings" in translated and existing:
        existing_menu = existing.get("menu_strings", {})
        old_map: dict[str, str] = {}
        for entry in existing_menu.get("strings", []):
            text = str(entry.get("text") or "")
            chinese = str(entry.get("chinese") or "")
            if text and chinese:
                old_map[text] = chinese
        for entry in translated["menu_strings"].get("strings", []):
            text = str(entry.get("text") or "")
            if text in old_map:
                entry["chinese"] = old_map[text]

    return translated


def write_report(index: dict[str, Any], report_path: Path) -> None:
    report_path.parent.mkdir(parents=True, exist_ok=True)
    lines: list[str] = []
    lines.append("# RTS Script Investigation")
    lines.append("")
    lines.append("- `backup/Media/RTS/Scripts/RTSScript.bin` is the extraction baseline.")
    lines.append("- `RTSScript.bin` is a `PACK` container.")
    lines.append("- `.inf` payloads decode after bytewise `^ 0xFF`, then parse as text scripts.")
    lines.append("- Current formal flow only extracts visible `Message(...)` bodies from `.inf` files.")
    lines.append("- In editable JSON, `text` is the normalized visible message body.")
    lines.append("- `body_text` keeps the raw script syntax view: trailing `\\` are in-page line joins and `;;` is a page separator.")
    lines.append("- `chinese` must be filled with explicit line breaks only; the writer does not auto-wrap.")
    lines.append(f"- Current extraction found `{index['message_total']}` `Message(...)` entries across `{index['file_count']}` `.inf` files.")
    lines.append("")
    lines.append("## Key Sample")
    lines.append("")

    sample_text = ""
    sample_file = ""
    sample_label = ""
    for file_info in index["files"]:
        for message in file_info["messages"]:
            text = str(message["text"])
            if "オマエ、ハッキングは初めてだったな？" in text:
                sample_file = str(file_info["name"])
                sample_label = str(message["label"])
                sample_text = text
                break
        if sample_text:
            break

    if sample_text:
        lines.append(f"- File: `{sample_file}`")
        lines.append(f"- Label: `{sample_label}`")
        lines.append("- Visible text:")
        lines.append("```text")
        lines.append(sample_text)
        lines.append("```")
        lines.append("")

    lines.append("## Files With Message Blocks")
    lines.append("")
    for file_info in index["files"]:
        lines.append(f"- `{file_info['name']}`: {file_info['message_count']} messages")

    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="Extract visible RTS Message(...) text from RTSScript.bin")
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT, help="Path to baseline RTSScript.bin")
    parser.add_argument("--extracted-dir", type=Path, default=DEFAULT_EXTRACTED_DIR, help="Directory for extracted JSON")
    parser.add_argument("--translated-dir", type=Path, default=DEFAULT_TRANSLATED_DIR, help="Directory for editable translated JSON")
    parser.add_argument("--report", type=Path, default=DEFAULT_REPORT, help="Markdown report path")
    args = parser.parse_args()

    extracted = build_index(args.input)

    args.extracted_dir.mkdir(parents=True, exist_ok=True)
    args.translated_dir.mkdir(parents=True, exist_ok=True)

    extracted_path = args.extracted_dir / "rts_messages.json"
    translated_path = args.translated_dir / "rts_messages.json"

    existing_translated: dict[str, Any] | None = None
    if translated_path.exists():
        existing_translated = json.loads(translated_path.read_text(encoding="utf-8"))
    translated = build_translated_index(extracted, existing_translated)

    extracted_path.write_text(json.dumps(extracted, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    translated_path.write_text(json.dumps(translated, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    write_report(extracted, args.report)

    print(f"source={args.input}")
    print(f"files={extracted['file_count']}")
    print(f"messages={extracted['message_total']}")
    print(f"extracted={extracted_path}")
    print(f"translated={translated_path}")
    print(f"report={args.report}")


if __name__ == "__main__":
    main()
