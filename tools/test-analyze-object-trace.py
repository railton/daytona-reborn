#!/usr/bin/env python3
'''Synthetic checks for analyze-object-trace.py (stdlib only).'''
from __future__ import annotations

import csv
import importlib.util
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('analyzer', HERE / 'analyze-object-trace.py')
analyzer = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(analyzer)

FIELDS = ['frame', 'pool', 'oba', 'obc', 'mode'] + [f'matrix{i}' for i in range(12)] + ['vertices', 'source']


def write_csv(path: Path, rows):
    with path.open('w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)


def row_from_raw(frame, oba, vertices, source, values):
    r = {'frame': frame, 'pool': 0, 'oba': oba, 'obc': 1, 'mode': 0,
         'vertices': vertices, 'source': source}
    for i, value in enumerate(values):
        r[f'matrix{i}'] = value
    return r


def row(frame, oba, vertices, source, tx=0.0):
    return row_from_raw(
        frame, oba, vertices, source,
        [1, 0, 0, 0, 1, 0, 0, 0, 1, tx, 0, 0],
    )


def test_rigid(tmp: Path):
    rows = []
    for f in range(4):
        for oba, v in ((0x00800001, 10), (0x00800002, 20), (0x00800003, 30)):
            rows.append(row(f, oba, v, 'ROM'))
    p = tmp / 'rigid.csv'
    write_csv(p, rows)
    out = analyzer.analyze(p)
    assert out['rom_share'] == 1.0 and out['clique_cover'] == 1.0, out
    assert out['verdict'] == 'alternative_valide', out


def test_rigid_fixed_offsets(tmp: Path):
    rows = []
    for f in range(4):
        shift = f * 5.0
        for oba, off in ((0x00800001, 0.0), (0x00800002, 10.0), (0x00800003, 20.0)):
            rows.append(row(f, oba, 10, 'ROM', tx=shift + off))
    p = tmp / 'rigid_offsets.csv'
    write_csv(p, rows)
    out = analyzer.analyze(p)
    assert out['clique_cover'] == 1.0, out
    assert out['verdict'] == 'alternative_valide', out


def test_independent(tmp: Path):
    rows = []
    for f in range(4):
        for i, oba in enumerate((0x00800001, 0x00800002, 0x00800003)):
            rows.append(row(f, oba, 10, 'ROM', tx=float(f * (i + 1))))
    p = tmp / 'independent.csv'
    write_csv(p, rows)
    out = analyzer.analyze(p)
    assert out['clique_size'] <= 1 or out['clique_cover'] < 0.5, out
    assert out['verdict'] == 'alternative_refutee', out


def test_ram_majority(tmp: Path):
    rows = []
    for f in range(4):
        for oba in (0x00800001, 0x00800002):
            rows.append(row(f, oba, 10, 'ROM'))
        for k in range(10):
            rows.append(row(f, 0x10000 + k, 10, 'RAM0'))
    p = tmp / 'ram.csv'
    write_csv(p, rows)
    out = analyzer.analyze(p)
    assert out['rom_share'] < 0.5, out
    assert out['verdict'] == 'alternative_refutee', out


def test_missing_key(tmp: Path):
    rows = []
    for f in range(4):
        rows.append(row(f, 0x00800001, 10, 'ROM'))
        if f < 2:
            rows.append(row(f, 0x00800002, 10, 'ROM'))
    p = tmp / 'missing.csv'
    write_csv(p, rows)
    out = analyzer.analyze(p)
    assert out['common_rom_keys'] == 1, out
    assert out['verdict'] == 'alternative_refutee', out


def test_duplicate_moving_instances(tmp: Path):
    rows = []
    for f in range(4):
        rows.append(row(f, 0x00800001, 10, 'ROM', tx=f * 7.0))
        rows.append(row(f, 0x00800001, 10, 'ROM', tx=0.0))
        rows.append(row(f, 0x00800002, 10, 'ROM', tx=10.0))
    p = tmp / 'duplicate_moving.csv'
    write_csv(p, rows)
    out = analyzer.analyze(p)
    assert out['verdict'] == 'alternative_indeterminee', out
    assert out['rows'] == 12 and out['frames'] == 4, out
    assert out['source_vertices'] == {'ROM': 120}, out
    assert out['rom_share'] == 1.0, out
    assert out['ambiguous_frames'] == 4, out
    assert out['duplicate_rom_key_groups'] == 4, out
    assert out['duplicate_rom_rows'] == 8, out
    assert 'repeated ROM geometry key group' in out['reason'], out


def test_duplicate_identical_poses(tmp: Path):
    rows = []
    for f in range(4):
        rows.append(row(f, 0x00800001, 10, 'ROM', tx=0.0))
        rows.append(row(f, 0x00800001, 10, 'ROM', tx=0.0))
        rows.append(row(f, 0x00800002, 20, 'ROM', tx=10.0))
    p = tmp / 'duplicate_identical.csv'
    write_csv(p, rows)
    out = analyzer.analyze(p)
    assert out['verdict'] == 'alternative_indeterminee', out
    assert out['source_vertices'] == {'ROM': 160}, out
    assert out['ambiguous_frames'] == 4, out
    assert out['duplicate_rom_key_groups'] == 4, out
    assert out['duplicate_rom_rows'] == 8, out


def test_duplicate_reordered_rows(tmp: Path):
    rows = []
    for f in range(4):
        rows.append(row(f, 0x00800001, 10, 'ROM', tx=f * 7.0))
        rows.append(row(f, 0x00800001, 10, 'ROM', tx=0.0))
        rows.append(row(f, 0x00800002, 10, 'ROM', tx=10.0))
    original = tmp / 'duplicate_order_a.csv'
    reordered = tmp / 'duplicate_order_b.csv'
    write_csv(original, rows)
    write_csv(reordered, list(reversed(rows)))
    first = analyzer.analyze(original)
    second = analyzer.analyze(reordered)
    assert first['verdict'] == 'alternative_indeterminee', first
    assert second == first, (first, second)


def test_same_geometry_once_per_frame(tmp: Path):
    rows = []
    for f in range(4):
        shift = f * 5.0
        rows.append(row(f, 0x00800001, 10, 'ROM', tx=shift))
        rows.append(row(f, 0x00800002, 10, 'ROM', tx=shift + 4.0))
    p = tmp / 'once_per_frame.csv'
    write_csv(p, rows)
    out = analyzer.analyze(p)
    assert 'duplicate_rom_key_groups' not in out, out
    assert out['common_rom_keys'] == 2, out
    assert out['clique_cover'] == 1.0, out
    assert out['verdict'] == 'alternative_valide', out


def test_native_matrix_layout(tmp: Path):
    # Literal raw array and result from geo.cpp transform_point's indexing;
    # independent of the fixture writer and analyzer's conversion code.
    raw = {f'matrix{i}': i + 1 for i in range(12)}
    matrix = analyzer.parse_matrix(raw)
    assert matrix == [[1, 4, 7, 10], [2, 5, 8, 11], [3, 6, 9, 12]], matrix
    transformed = [sum(m[c] * v for c, v in enumerate((2, 3, 5, 1))) for m in matrix]
    assert transformed == [59, 70, 81], transformed


def test_rotating_camera_native_layout(tmp: Path):
    # Three stationary world objects, camera rotates/translates every frame.
    # CSV ordering also reverses: neither row order nor camera motion is identity.
    rows = []
    for frame, (c, s) in enumerate(((1, 0), (0, 1), (-1, 0), (0, -1))):
        current = []
        for i, offset in enumerate((0, 10, 20)):
            item = row(frame, 0x00800001 + i, 10, 'ROM')
            raw = [c, s, 0, -s, c, 0, 0, 0, 1, c * offset + frame * 3, s * offset - frame * 2, 80]
            item.update({f'matrix{k}': value for k, value in enumerate(raw)})
            current.append(item)
        rows.extend(reversed(current) if frame % 2 else current)
    path = tmp / 'rotating_camera.csv'
    write_csv(path, rows)
    out = analyzer.analyze(path)
    assert out['rigid_pairs'] == 3 and out['clique_cover'] == 1.0, out
    assert out['verdict'] == 'alternative_valide', out


def test_singular_reference(tmp: Path):
    rows = []
    for frame in range(2):
        for i in range(3):
            item = row(frame, 0x00800001 + i, 10, 'ROM', tx=i * 10)
            if frame == 0:
                item.update({f'matrix{k}': 0 for k in range(12)})
            rows.append(item)
    path = tmp / 'singular_reference.csv'
    write_csv(path, rows)
    out = analyzer.analyze(path)
    assert out['rigid_pairs'] == 0, out
    assert out['unsupported_pairs'] == 3, out
    assert out['verdict'] == 'alternative_indeterminee', out


def test_independent_equal_size_two(tmp: Path):
    rows = []
    for f in range(4):
        rows.append(row(f, 0x00800001, 10, 'ROM', tx=f * 5.0))
        rows.append(row(f, 0x00800002, 10, 'ROM', tx=-f * 7.0))
    path = tmp / 'independent_equal_two.csv'
    write_csv(path, rows)
    out = analyzer.analyze(path)
    assert out['frames'] == 4, out
    assert out['rigid_pairs'] == 0, out
    assert out['clique_size'] == 1, out
    assert out['clique_cover'] == 0.5, out
    assert out['verdict'] == 'alternative_refutee', out


def test_single_frame_cannot_validate(tmp: Path):
    rows = [row(0, 0x00800001 + i, 10, 'ROM', tx=i * 10.0) for i in range(3)]
    path = tmp / 'single_frame.csv'
    write_csv(path, rows)
    out = analyzer.analyze(path)
    assert out['frames'] == 1, out
    assert out['rigid_pairs'] == 0, out
    assert out['verdict'] == 'alternative_indeterminee', out


def _invalid_basis_trace(tmp: Path, filename: str, basis):
    rows = []
    for f in range(4):
        a = list(basis)
        b = list(basis)
        a[9] = f * 5.0
        b[9] = f * 5.0 + 10.0
        rows.append(row_from_raw(f, 0x00800001, 10, 'ROM', a))
        rows.append(row_from_raw(f, 0x00800002, 10, 'ROM', b))
    path = tmp / filename
    write_csv(path, rows)
    return analyzer.analyze(path)


def _assert_invalid(out):
    assert out['frames'] == 4, out
    assert out['common_rom_keys'] == 2, out
    assert out['rigid_pairs'] == 0, out
    assert out['clique_size'] < 2, out
    assert out['clique_cover'] == 0.5, out
    assert out['unsupported_pairs'] == 1, out
    assert out['verdict'] == 'alternative_indeterminee', out


def test_mixed_singular_preserves_supported_evidence(tmp: Path):
    for bad_weight, verdict in ((90, 'alternative_indeterminee'),
                                (1, 'alternative_valide')):
        rows = []
        for frame in range(3):
            rows += [row(frame, 0x800001, 10, 'ROM', tx=frame),
                     row(frame, 0x800002, 10, 'ROM', tx=frame + 10),
                     row_from_raw(frame, 0x800003, bad_weight, 'ROM', [0] * 12)]
        path = tmp / f'mixed_singular_{bad_weight}.csv'
        write_csv(path, rows)
        out = analyzer.analyze(path)
        assert out['rigid_pairs'] == 1, out
        assert out['unsupported_pairs'] == 2, out
        assert out['verdict'] == verdict, out


def test_scaled_matrices_cannot_validate(tmp: Path):
    basis = [2, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0]
    _assert_invalid(_invalid_basis_trace(tmp, 'scaled.csv', basis))


def test_sheared_matrices_cannot_validate(tmp: Path):
    basis = [1, 0, 0, 0.25, 1, 0, 0, 0, 1, 0, 0, 0]
    _assert_invalid(_invalid_basis_trace(tmp, 'sheared.csv', basis))


def test_reflected_matrices_cannot_validate(tmp: Path):
    basis = [-1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]
    _assert_invalid(_invalid_basis_trace(tmp, 'reflected.csv', basis))


def test_nan_translation_cannot_validate(tmp: Path):
    rows = []
    for f in range(4):
        rows.append(row(f, 0x00800001, 10, 'ROM', tx=float('nan')))
        rows.append(row(f, 0x00800002, 10, 'ROM', tx=float('nan')))
    path = tmp / 'nan_translation.csv'
    write_csv(path, rows)
    _assert_invalid(analyzer.analyze(path))


def test_equal_cardinality_heavy_clique_wins(tmp: Path):
    def build(filename, reverse):
        rows = []
        for f in range(4):
            current = [
                row(f, 0x00800001, 10, 'ROM', tx=100.0 - f * 3.0),
                row(f, 0x00800002, 10, 'ROM', tx=110.0 - f * 3.0),
                row(f, 0x00800003, 100, 'ROM', tx=f * 7.0),
                row(f, 0x00800004, 100, 'ROM', tx=f * 7.0 + 10.0),
            ]
            if reverse:
                current.reverse()
            rows.extend(current)
        path = tmp / filename
        write_csv(path, rows)
        return analyzer.analyze(path)

    first = build('heavy_equal_a.csv', False)
    second = build('heavy_equal_b.csv', True)
    expected = round(200 / 220, 4)
    for out in (first, second):
        assert out['common_rom_keys'] == 4, out
        assert out['rigid_pairs'] == 2, out
        assert out['clique_size'] == 2, out
        assert out['clique_cover'] == expected, out
        assert out['verdict'] == 'alternative_valide', out
    assert first == second, (first, second)


def test_larger_light_clique_does_not_hide_heavy(tmp: Path):
    def build(filename, reverse):
        rows = []
        for f in range(4):
            current = [
                row(f, 0x00800001, 1, 'ROM', tx=-4.0 * f),
                row(f, 0x00800002, 1, 'ROM', tx=-4.0 * f + 5.0),
                row(f, 0x00800003, 1, 'ROM', tx=-4.0 * f + 10.0),
                row(f, 0x00800004, 100, 'ROM', tx=9.0 * f),
                row(f, 0x00800005, 100, 'ROM', tx=9.0 * f + 20.0),
            ]
            if reverse:
                current.reverse()
            rows.extend(current)
        path = tmp / filename
        write_csv(path, rows)
        return analyzer.analyze(path)

    first = build('weighted_a.csv', False)
    second = build('weighted_b.csv', True)
    expected = round(200 / 203, 4)
    for out in (first, second):
        assert out['common_rom_keys'] == 5, out
        assert out['rigid_pairs'] == 4, out
        assert out['clique_size'] == 2, out
        assert out['clique_cover'] == expected, out
        assert out['verdict'] == 'alternative_valide', out
    assert first == second, (first, second)


def test_all_frames_coverage(tmp):
    rows = []
    for f, weights in enumerate(((50,50,1), (1,1,100), (1,1,100))):
        rows += [row(f, 1, weights[0], 'ROM', tx=f),
                 row(f, 2, weights[1], 'ROM', tx=f+10),
                 row(f, 3, weights[2], 'ROM', tx=100+20*f)]
    path = tmp / 'changing_coverage.csv'; write_csv(path, rows)
    out = analyzer.analyze(path)
    assert out['verdict'] == 'alternative_refutee', out
    assert out['clique_cover_by_frame'] == [round(100/101,4), round(2/102,4), round(2/102,4)], out
    # Frame order is not a source of weights or a privileged pose reference.
    for r in rows: r['frame'] = 2-r['frame']
    write_csv(path, list(reversed(rows))); reversed_out = analyzer.analyze(path)
    assert reversed_out['verdict'] == out['verdict'], reversed_out
    assert reversed_out['clique_cover'] == out['clique_cover'], reversed_out


def test_unsupported_cannot_rescue_impossible_majority(tmp):
    rows = []
    for f in range(3):
        rows += [row(f, k, 10, 'ROM', tx=f*k*10) for k in range(1,5)]
        rows.append(row_from_raw(f, 5, 1, 'ROM', [0]*12))
    path = tmp / 'light_unknown.csv'; write_csv(path, rows)
    out = analyzer.analyze(path)
    assert out['unsupported_pairs'] == 4, out
    assert out['optimistic_clique_cover'] == round(11/41,4), out
    assert out['verdict'] == 'alternative_refutee', out


def test_rom_minority_is_decisive_with_unknown(tmp):
    rows = []
    for f in range(3):
        rows += [row_from_raw(f, 1, 10, 'ROM', [0]*12),
                 row_from_raw(f, 2, 10, 'ROM', [0]*12), row(f, 3, 100, 'RAM')]
    path = tmp / 'rom_minority_unknown.csv'; write_csv(path, rows)
    assert analyzer.analyze(path)['verdict'] == 'alternative_refutee'


def test_supported_mismatch_survives_unknown_sample(tmp):
    rows = [row_from_raw(0, k, 10, 'ROM', [0]*12) for k in (1,2)]
    for f in (1,2):
        rows += [row(f, 1, 10, 'ROM', tx=f), row(f, 2, 10, 'ROM', tx=10*f)]
    path = tmp / 'mismatch_unknown.csv'; write_csv(path, rows)
    out = analyzer.analyze(path)
    assert out['verdict'] == 'alternative_refutee', out
    assert out['optimistic_clique_size'] == 1, out


def test_search_limit_is_indeterminate(tmp):
    rows = [row(f, k, 10, 'ROM', tx=f+k) for f in range(2) for k in range(23)]
    path = tmp / 'bounded_search.csv'; write_csv(path, rows)
    out = analyzer.analyze(path)
    assert out['verdict'] == 'alternative_indeterminee' and out['search_limit'], out


def test_same_oba_different_obc_ambiguous(tmp):
    # obc is a link-prefix, not an instance id: two rows with same
    # pool/oba/mode in one frame are ambiguous even when obc differs.
    rows = []
    for f in range(4):
        for obc, tx in ((4, f * 5.0), (5, f * 5.0 + 10.0)):
            item = row(f, 0x00800001, 10, 'ROM', tx=tx)
            item['obc'] = obc
            rows.append(item)
    path = tmp / 'same_oba_different_obc.csv'; write_csv(path, rows)
    out = analyzer.analyze(path)
    assert out['verdict'] == 'alternative_indeterminee', out
    assert out['ambiguous_frames'] == 4, out
    assert out['duplicate_rom_key_groups'] == 4, out
    assert out['duplicate_rom_rows'] == 8, out


def main():
    tests = [
        test_all_frames_coverage,
        test_unsupported_cannot_rescue_impossible_majority,
        test_rom_minority_is_decisive_with_unknown,
        test_supported_mismatch_survives_unknown_sample,
        test_search_limit_is_indeterminate,
        test_native_matrix_layout,
        test_rotating_camera_native_layout,
        test_singular_reference,
        test_mixed_singular_preserves_supported_evidence,
        test_rigid,
        test_rigid_fixed_offsets,
        test_independent,
        test_ram_majority,
        test_missing_key,
        test_duplicate_moving_instances,
        test_duplicate_identical_poses,
        test_duplicate_reordered_rows,
        test_same_geometry_once_per_frame,
        test_same_oba_different_obc_ambiguous,
        test_independent_equal_size_two,
        test_single_frame_cannot_validate,
        test_scaled_matrices_cannot_validate,
        test_sheared_matrices_cannot_validate,
        test_reflected_matrices_cannot_validate,
        test_nan_translation_cannot_validate,
        test_equal_cardinality_heavy_clique_wins,
        test_larger_light_clique_does_not_hide_heavy,
    ]
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        for test in tests:
            test(tmp)
    print(f'{len(tests)} synthetic tests PASS')


if __name__ == '__main__':
    main()
