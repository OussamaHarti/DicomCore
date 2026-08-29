"""Pydicom benchmark baseline with tags declared directly in the script and multi-processing support.

Example:
    python benchmark_pydicom_hardcoded.py --source ".../My files py" \
        --output ".../Output py hardcoded" --threads 4 --in-place

Use a non-anonymized copy of the dataset: with --in-place, source files
are modified, just like in the C++ program.
"""

from __future__ import annotations

import argparse
import csv
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

try:
    import pydicom
except ImportError:
    sys.exit("pydicom is not installed. Run: python -m pip install pydicom")


DICOM_EXTENSIONS = {".dicom", ".dcm", ".dic"}

# Researchers typically declare the fields necessary for their dataset here.
CSV_FIELDS = [
    ("File Meta Information Version", "FileMetaInformationVersion", True),
    ("TransferSyntax", "TransferSyntaxUID", True),
    ("Modality", "Modality", False),
    ("SamplesPerPixel", "SamplesPerPixel", False),
    ("PhotometricInterpretation", "PhotometricInterpretation", False),
    ("PlanarConfiguration", "PlanarConfiguration", False),
    ("NumberOfFrames", "NumberOfFrames", False),
    ("Rows", "Rows", False),
    ("Columns", "Columns", False),
    ("BitsAllocated", "BitsAllocated", False),
    ("BitsStored", "BitsStored", False),
    ("PixelRepresentation", "PixelRepresentation", False),
]

# The same sensitive fields handled by the C++ program.
SENSITIVE_ATTRIBUTES = [
    "StudyDate", "AccessionNumber", "InstitutionName", "InstitutionAddress",
    "ReferringPhysicianName", "StationName", "PerformingPhysicianName",
    "OperatorsName", "PatientName", "PatientID", "OtherPatientIDs",
    "OtherPatientNames", "PatientAddress", "PatientTelephoneNumbers",
    "PatientComments", "DeviceSerialNumber", "ImageComments",
]


def csv_value(value: object) -> str:
    if isinstance(value, bytes):
        return value.decode("latin-1", errors="replace").rstrip(" \0")
    return str(value).rstrip(" \0") if value is not None else ""


def anonymize_in_place(dataset: pydicom.dataset.FileDataset, file_path: Path) -> None:
    """Removes top-level sensitive tags and rewrites the DICOM Part 10 file."""
    for attribute in SENSITIVE_ATTRIBUTES:
        if hasattr(dataset, attribute):
            setattr(dataset, attribute, "")

    dataset.save_as(file_path, write_like_original=False)


def raw_destination(source: Path, raw_root: Path, file_path: Path) -> Path:
    relative_parent = file_path.parent.relative_to(source)
    return raw_root / relative_parent / f"{file_path.name}.raw"


def csv_row(dataset: pydicom.dataset.FileDataset, raw_path: Path) -> list[str]:
    row: list[str] = []
    for _, attribute, is_meta in CSV_FIELDS:
        owner = dataset.file_meta if is_meta else dataset
        row.append(csv_value(getattr(owner, attribute, None)))
    row.append(str(raw_path))
    return row


def process_file(file_path: Path, source: Path, raw_root: Path) -> tuple[bool, list[str] | None, str | None]:
    """Processes a single DICOM file (executed by parallel processes)."""
    try:
        dataset = pydicom.dcmread(file_path, force=False)
        pixel_data = dataset[0x7FE00010].value

        anonymize_in_place(dataset, file_path)

        destination = raw_destination(source, raw_root, file_path)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(bytes(pixel_data))

        row = csv_row(dataset, destination)
        return True, row, None
    except Exception as error:
        return False, None, f"[PROCESSING ERROR] {file_path} - {error}\n"


def run(source: Path, output: Path, num_processes: int) -> int:
    raw_root = output / "raw"
    output.mkdir(parents=True, exist_ok=True)
    raw_root.mkdir(parents=True, exist_ok=True)

    files = sorted(path for path in source.rglob("*") if path.is_file() and path.suffix.lower() in DICOM_EXTENSIONS)
    if not files:
        print(f"No DICOM files found in {source}", file=sys.stderr)
        return 1

    valid_count = 0
    skipped_count = 0
    started_at = time.perf_counter()
    last_progress = started_at

    csv_file_path = output / "dataset.csv"
    log_file_path = output / "errors.log"

    with csv_file_path.open("w", newline="", encoding="utf-8") as csv_file, \
         log_file_path.open("w", encoding="utf-8") as log_file:
        
        writer = csv.writer(csv_file)
        writer.writerow([name for name, _, _ in CSV_FIELDS] + ["RawPath"])

        print(f"Starting processing of {len(files)} files using {num_processes} processes...")

        with ProcessPoolExecutor(max_workers=num_processes) as executor:
            futures = {
                executor.submit(process_file, file_path, source, raw_root): file_path 
                for file_path in files
            }

            for index, future in enumerate(as_completed(futures), start=1):
                success, row, error_msg = future.result()

                if success:
                    writer.writerow(row)
                    valid_count += 1
                else:
                    log_file.write(error_msg)
                    skipped_count += 1

                now = time.perf_counter()
                if now - last_progress >= 0.25 or index == len(files):
                    print(f"\r[PROCESSING] {index} / {len(files)} ({index * 100 // len(files)}%)", end="", flush=True)
                    last_progress = now

        elapsed = time.perf_counter() - started_at
        log_file.write(f"\nCompleted: {valid_count} valid files, {skipped_count} skipped.\n")
        log_file.write(f"Execution time: {elapsed:.4f} seconds.\n")

    print(f"\nCompleted in {elapsed:.4f} seconds: {valid_count} valid, {skipped_count} skipped.")
    print(f"Details in {output / 'errors.log'}")
    return 0


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Pydicom benchmark with hardcoded tags and multi-processing.")
    parser.add_argument("--source", required=True, type=Path, help="Copy of the DICOM dataset.")
    parser.add_argument("--output", required=True, type=Path, help="New output directory.")
    parser.add_argument("--threads", type=int, default=4, help="Number of parallel processes (default: 4).")
    parser.add_argument("--in-place", action="store_true", help="Confirms modification of the source DICOM files.")
    arguments = parser.parse_args()
    if not arguments.in_place:
        parser.error("For safety reasons, you must specify --in-place; use a copy of the dataset.")
    return arguments


if __name__ == "__main__":
    args = parse_arguments()
    source_path = args.source.resolve()
    output_path = args.output.resolve()
    if not source_path.is_dir():
        sys.exit("Source directory does not exist.")
    sys.exit(run(source_path, output_path, args.threads))