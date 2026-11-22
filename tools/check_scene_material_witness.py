# Compare SceneCache::PrimitiveState uint32_t texheader[4] values.

_UINT32_MAX = (1 << 32) - 1


def _is_mesh_id(value):
    return value is None or (type(value) is int and value >= 0)


def _canonical_headers(headers):
    if not isinstance(headers, (list, tuple)) or not headers:
        return None
    canonical = []
    for header in headers:
        if not isinstance(header, (list, tuple)) or len(header) != 4:
            return None
        words = []
        for word in header:
            if type(word) is not int or not 0 <= word <= _UINT32_MAX:
                return None
            words.append(word)
        canonical.append(tuple(words))
    return tuple(canonical)


def compare_material_witness(mesh_a, headers_a, mesh_b, headers_b):
    if not _is_mesh_id(mesh_a) or not _is_mesh_id(mesh_b):
        return 'unavailable'
    if mesh_a is None or mesh_b is None:
        return 'unavailable'

    if mesh_a != mesh_b:
        return 'out_of_scope'

    canon_a = _canonical_headers(headers_a)
    canon_b = _canonical_headers(headers_b)
    if canon_a is None or canon_b is None:
        return 'unavailable'
    if canon_a == canon_b:
        return 'same_signature'
    return 'conflicting_signature'


def _self_test():
    zero = [(0, 0, 0, 0)]
    edge = [(_UINT32_MAX, 0, 1, 2)]
    headers = ((1, 2, 3, 4), (5, 6, 7, 8))
    reordered = ((5, 6, 7, 8), (1, 2, 3, 4))

    assert compare_material_witness(7, zero, 7, [[0, 0, 0, 0]]) == 'same_signature'
    assert compare_material_witness(7, edge, 7, edge) == 'same_signature'
    assert compare_material_witness(7, [(0, 0, 0, 1)], 7, zero) == 'conflicting_signature'
    assert compare_material_witness(7, headers, 7, reordered) == 'conflicting_signature'
    assert compare_material_witness(8, headers, 9, headers) == 'out_of_scope'
    assert compare_material_witness(8, None, 9, None) == 'out_of_scope'

    assert compare_material_witness(True, zero, 7, zero) == 'unavailable'
    assert compare_material_witness(-1, zero, 7, zero) == 'unavailable'
    assert compare_material_witness(7, [(True, 0, 0, 0)], 7, zero) == 'unavailable'
    assert compare_material_witness(7, [(-1, 0, 0, 0)], 7, zero) == 'unavailable'
    assert compare_material_witness(7, [(_UINT32_MAX + 1, 0, 0, 0)], 7, zero) == 'unavailable'
    assert compare_material_witness(7, [(0, 0, 0)], 7, zero) == 'unavailable'
    assert compare_material_witness(7, None, 7, zero) == 'unavailable'
    assert compare_material_witness(7, [], 7, zero) == 'unavailable'
    assert compare_material_witness(None, zero, 7, zero) == 'unavailable'
    assert compare_material_witness(7, zero, None, zero) == 'unavailable'


if __name__ == '__main__':
    _self_test()
