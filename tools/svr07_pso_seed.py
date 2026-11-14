"""SvR07 copy of RumbleRosesRecomp/tools/nr_pso_seed.py (same record format: native/src/native/ufc_pipeline_store.cpp).
Build pipelines_seed.bin (NR-9h) from the rec= fields of native test.log files.

usage: python -I SVR07-Recomp-native/tools/svr07_pso_seed.py <out file> <run dir, test.log or earlier seed .bin> [more ...]

Each miss line carries the pipeline's store record in hex. Runs with different fighters and venues
merge: put the file beside shader_cache.bin in the frozen build. Record layout = PipelineState,
header = FileHeader in src/native/ufc_pipeline_store.cpp.
"""
import os
import re
import struct
import sys

MAGIC, VERSION, RECORD = 0x50525653, 2, 156
REC = re.compile(r'render-thread compile \(miss.* rec=([0-9A-F]+)')


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    records = {}
    for path in argv[1:]:
        if path.endswith('.bin'):
            # An earlier seed: keep its records, so a new run's misses add to it.
            before = len(records)
            with open(path, 'rb') as f:
                if struct.unpack('<IIII', f.read(16)) != (MAGIC, VERSION, RECORD, 0):
                    print('%s: wrong header' % path)
                    return 1
                while True:
                    raw = f.read(RECORD)
                    if len(raw) < RECORD:
                        break
                    records.setdefault(raw, True)
            print('%s: %d seed records' % (path, len(records) - before))
            continue
        if os.path.isdir(path):
            path = os.path.join(path, 'test.log')
        before = len(records)
        lines = 0
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                m = REC.search(line)
                if m:
                    lines += 1
                    raw = bytes.fromhex(m.group(1))
                    if len(raw) != RECORD:
                        print('wrong record size', len(raw), 'in', path)
                        return 1
                    records.setdefault(raw, True)
        print('%s: %d miss lines, %d new records' % (path, lines, len(records) - before))
    if not records:
        print('no records: no rec= field in these logs')
        return 1
    with open(argv[0], 'wb') as f:
        f.write(struct.pack('<IIII', MAGIC, VERSION, RECORD, 0))
        for raw in records:
            f.write(raw)
    print('%s: %d records' % (argv[0], len(records)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
