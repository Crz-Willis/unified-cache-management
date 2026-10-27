import importlib.util
import runpy
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "kv_semantics/metrics/tools/generate_kv_metrics.py"
spec = importlib.util.spec_from_file_location("kv_metric_codegen", SCRIPT)
codegen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(codegen)


def test_native_and_ucm_catalogs_match():
    import yaml

    definitions = codegen.load_definitions()
    default = runpy.run_path(str(ROOT / "ucm/default_metrics_config.py"))
    example = yaml.safe_load(
        (ROOT / "examples/metrics/metrics_configs.yaml").read_text()
    )
    assert not codegen.check_ucm_definitions(
        definitions, default["get_default_metrics_config"]()
    )
    assert not codegen.check_ucm_definitions(definitions, example)


def test_generated_files_are_current():
    subprocess.run([sys.executable, str(SCRIPT), "--check"], check=True)


def test_all_operation_outcomes_are_registered():
    definitions = codegen.load_definitions()
    names = {item["name"] for items in definitions.values() for item in items}
    ops = ["query", "load", "store", "batch_load", "batch_store", "delete"]
    for op in ops:
        for result in ("succeeded", "failed", "canceled"):
            assert f"kv_client_{op}_tasks_{result}_total" in names
        assert f"kv_client_{op}_task_e2e_duration_seconds" in names
    for result in ("failed", "canceled"):
        assert f"kv_transport_tasks_{result}_total" in names
    for op in [*ops, "keep_alive"]:
        for result in ("succeeded", "failed", "canceled"):
            assert f"kv_transport_{op}_tasks_{result}_total" not in names
        assert f"kv_transport_{op}_task_e2e_duration_seconds" not in names
    for op in ops:
        for result in ("succeeded", "failed"):
            assert (f"kv_client_{op}_entries_{result}_total" in names) == (
                op in ("load", "store", "batch_load", "batch_store")
            )
