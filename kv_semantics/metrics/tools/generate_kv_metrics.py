#!/usr/bin/env python3
"""Generate KV descriptors and check the UCM registration contract."""
from __future__ import annotations

import argparse
import json
import re
from decimal import Decimal
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "kv_semantics/metrics/config/kv_metrics.yaml"
DEFAULT_HEADER = (
    ROOT / "kv_semantics/metrics/include/kv_metrics/default_metric_descriptors.h"
)
SECTIONS = ("counter", "gauge", "histogram")
UCM_YAML = ROOT / "examples/metrics/metrics_configs.yaml"
UCM_DEFAULTS = ROOT / "ucm/default_metrics_config.py"


def render_ucm_block(
    definitions: dict[str, list[dict]], section: str, python: bool
) -> str:
    lines = [
        (
            f"    # BEGIN GENERATED KV {section}"
            if python
            else f"  # BEGIN GENERATED KV {section}"
        )
    ]
    for metric in definitions[section]:
        name = json.dumps(metric["name"])
        documentation = json.dumps(metric["documentation"])
        # YAML 1.1 treats bare 1e-05 as a string; use decimal literals on both sides.
        buckets = (
            "["
            + ", ".join(
                format(Decimal(str(value)), "f") for value in metric.get("buckets", [])
            )
            + "]"
        )
        if python:
            extra = (
                f", {buckets}"
                if section == "histogram"
                else ", {}" if section == "gauge" else ""
            )
            lines.append(f"    ({name}, {documentation}{extra}),")
        else:
            lines.extend([f"  - name: {name}", f"    documentation: {documentation}"])
            if section == "histogram":
                lines.append(f"    buckets: {buckets}")
    lines.append(
        f"    # END GENERATED KV {section}"
        if python
        else f"  # END GENERATED KV {section}"
    )
    return "\n".join(lines)


def render_ucm_config(
    content: str, definitions: dict[str, list[dict]], python: bool
) -> str:
    for section in SECTIONS:
        if not definitions[section]:
            continue
        block = render_ucm_block(definitions, section, python)
        pattern = rf"(?m)^ *# BEGIN GENERATED KV {section}\n.*?^ *# END GENERATED KV {section}"
        if re.search(pattern, content, re.DOTALL):
            content = re.sub(pattern, lambda _: block, content, flags=re.DOTALL)
        else:
            anchor = f"_{section.upper()}_METRICS = [\n" if python else f"{section}:\n"
            if anchor not in content:
                raise ValueError(f"missing UCM section: {section}")
            content = content.replace(anchor, anchor + block + "\n", 1)
    return content


def check_ucm_definitions(
    definitions: dict[str, list[dict]], config: dict
) -> list[str]:
    """Report missing or incompatible definitions without changing custom enable lists."""
    actual: dict[str, list[tuple[str, dict]]] = {}
    for section in SECTIONS:
        for metric in config.get(section, []) or []:
            actual.setdefault(metric["name"], []).append((section, metric))
    errors = []
    for section in SECTIONS:
        for metric in definitions[section]:
            name = metric["name"]
            entries = actual.get(name, [])
            if not entries:
                errors.append(f"missing KV metric: {name}")
                continue
            if len(entries) != 1:
                errors.append(f"duplicate KV metric: {name}")
                continue
            actual_type, entry = entries[0]
            if actual_type != section:
                errors.append(
                    f"type mismatch for {name}: expected {section}, got {actual_type}"
                )
            if entry.get("documentation") != metric["documentation"]:
                errors.append(f"documentation mismatch for {name}")
            if entry.get("buckets", []) != metric.get("buckets", []):
                message = f"bucket mismatch for {name}"
                buckets = entry.get("buckets", [])
                if isinstance(buckets, list):
                    strings = [
                        f"buckets[{index}]={value!r} (str)"
                        for index, value in enumerate(buckets)
                        if isinstance(value, str)
                    ]
                    if strings:
                        message += (
                            f": {', '.join(strings)}; expected numeric values. "
                            "YAML 1.1 may parse 1e-5 as a string; use unquoted "
                            "decimal literals such as 0.00001."
                        )
                errors.append(message)
    return errors


def load_definitions() -> dict[str, list[dict]]:
    definitions: dict[str, list[dict]] = {section: [] for section in SECTIONS}
    section = ""
    field_pattern = re.compile(r"([a-z_]+):\s*(?:\"((?:\\.|[^\"])*)\"|\[([^\]]*)\])")
    for line_number, raw_line in enumerate(
        SOURCE.read_text(encoding="utf-8").splitlines(), start=1
    ):
        stripped = raw_line.strip()
        if stripped in {f"{name}:" for name in SECTIONS}:
            section = stripped[:-1]
            continue
        if not section or not stripped.startswith("- {"):
            continue
        metric: dict = {}
        for match in field_pattern.finditer(stripped):
            key, quoted, sequence = match.groups()
            if sequence is not None:
                metric[key] = [
                    float(value.strip())
                    for value in sequence.split(",")
                    if value.strip()
                ]
            else:
                metric[key] = json.loads(f'"{quoted}"')
        if not metric:
            raise ValueError(f"cannot parse metric at {SOURCE}:{line_number}")
        definitions[section].append(metric)

    seen_names: set[str] = set()
    for section in SECTIONS:
        for metric in definitions[section]:
            name = metric.get("name", "")
            if not re.fullmatch(r"[a-zA-Z_:][a-zA-Z0-9_:]*", name):
                raise ValueError(f"invalid metric name: {name!r}")
            if name in seen_names:
                raise ValueError(f"duplicate metric name: {name}")
            if not metric.get("documentation"):
                raise ValueError(f"missing documentation for {name}")
            if section == "histogram" and not metric.get("buckets"):
                raise ValueError(f"missing histogram buckets for {name}")
            seen_names.add(name)
    return definitions


def cpp_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def cpp_number(value: int | float) -> str:
    return repr(value)


def render_default_header(definitions: dict[str, list[dict]]) -> str:
    entries: list[str] = []
    for section in SECTIONS:
        for metric in definitions[section]:
            buckets = metric.get("buckets", [])
            bucket_values = (
                "{" + ", ".join(cpp_number(value) for value in buckets) + "}"
            )
            entries.append(
                "        {"
                f"{cpp_string(metric['name'])}, MetricType::{section.upper()}, "
                f"{cpp_string(metric['documentation'])}, {bucket_values}"
                "},"
            )
    body = "\n".join(entries)
    return f"""#pragma once

// Generated by generate_kv_metrics.py from kv_metrics.yaml.

#include <vector>
#include "kv_metrics/standalone_metrics_backend.h"

namespace kv::metrics {{

inline std::vector<MetricDescriptor> DefaultKvMetricDescriptors()
{{
    // Keep generated descriptors compact and deterministic across clang-format versions.
    // clang-format off
    return {{
{body}
    }};
    // clang-format on
}}

}}  // namespace kv::metrics
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="check standalone and UCM generated definitions",
    )
    parser.add_argument(
        "--sync-ucm",
        action="store_true",
        help="also update UCM YAML and Python defaults",
    )
    parser.add_argument(
        "--ucm-config", type=Path, help="validate a deployment YAML (requires PyYAML)"
    )
    args = parser.parse_args()
    if args.ucm_config and (args.check or args.sync_ucm):
        parser.error("--ucm-config cannot be combined with --check or --sync-ucm")
    definitions = load_definitions()
    if args.ucm_config:
        import yaml

        config = yaml.safe_load(args.ucm_config.read_text(encoding="utf-8")) or {}
        errors = check_ucm_definitions(definitions, config)
        for error in errors:
            print(error)
        return int(bool(errors))
    outputs = {DEFAULT_HEADER: render_default_header(definitions)}
    if args.check or args.sync_ucm:
        for path, python in ((UCM_YAML, False), (UCM_DEFAULTS, True)):
            outputs[path] = render_ucm_config(
                path.read_text(encoding="utf-8"), definitions, python
            )
    stale = False
    for path, content in outputs.items():
        if path.exists() and path.read_text(encoding="utf-8") == content:
            continue
        stale = True
        if args.check:
            print(f"out of date: {path.relative_to(ROOT)}")
        else:
            path.write_text(content, encoding="utf-8", newline="\n")
            print(f"generated {path.relative_to(ROOT)}")
    return int(args.check and stale)


if __name__ == "__main__":
    raise SystemExit(main())
