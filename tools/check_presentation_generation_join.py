"""Reference join between sampled presentation generations and digests."""

UINT64_MAX = (1 << 64) - 1


def _uint64(value, name):
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{name} must be a uint64 integer")
    if value < 0 or value > UINT64_MAX:
        raise ValueError(f"{name} must be in uint64 range")
    return value


def join_presentations(samples, digests):
    """Join ordered (submit_ns, generation) samples to generation digests.

    Returns ordered (submit_ns, digest) rows. The digest for a repeated
    generation is reused for every present in that generation. If a sampled
    generation is absent from *digests*, raises ValueError naming the first
    such generation in sample order.
    """
    rows = []
    previous_submit = None
    previous_generation = None
    have_previous = False

    for index, sample in enumerate(samples):
        try:
            submit_ns, generation = sample
        except (TypeError, ValueError):
            raise ValueError(
                f"sample {index} must be a (submit_ns, generation) pair"
            ) from None

        _uint64(submit_ns, "submit_ns")
        _uint64(generation, "generation")

        if have_previous:
            if submit_ns <= previous_submit:
                raise ValueError(
                    "sample timestamps must be strictly increasing: "
                    f"sample {index} has submit_ns {submit_ns} after {previous_submit}"
                )
            if generation < previous_generation:
                raise ValueError(
                    "sample generations must be nondecreasing: "
                    f"sample {index} has generation {generation} after {previous_generation}"
                )

        if generation not in digests:
            raise ValueError(f"missing digest for generation {generation}")

        digest = digests[generation]
        if not isinstance(digest, str) or not digest.strip():
            raise ValueError(
                f"digest for generation {generation} must be a nonempty string"
            )

        rows.append((submit_ns, digest))
        previous_submit = submit_ns
        previous_generation = generation
        have_previous = True

    return rows


def _expect_value_error(samples, digests, needle):
    try:
        join_presentations(samples, digests)
    except ValueError as error:
        text = str(error)
        if needle not in text:
            raise AssertionError(
                f"expected {needle!r} in ValueError message, got {text!r}"
            ) from None
        return
    raise AssertionError("expected ValueError")


def _self_test():
    # Repeated presents plus a digest map inserted out of generation order.
    rows = join_presentations(
        [(100, 4), (200, 4), (300, 5)],
        {5: "five", 4: "four"},
    )
    if rows != [(100, "four"), (200, "four"), (300, "five")]:
        raise AssertionError(f"repeat/order join mismatch: {rows!r}")

    if join_presentations([], {}) != []:
        raise AssertionError("empty input should join to an empty list")

    # Missing middle generation.
    _expect_value_error(
        [(10, 0), (20, 1), (30, 2)],
        {2: "two", 0: "zero"},
        "generation 1",
    )

    # A numeric gap must not be filled or skipped over silently.
    _expect_value_error(
        [(10, 0), (20, 2)],
        {0: "zero"},
        "generation 2",
    )

    # Strict timestamp order is required.
    _expect_value_error(
        [(10, 0), (20, 1), (20, 2)],
        {0: "zero", 1: "one", 2: "two"},
        "timestamp",
    )


def main(argv):
    if argv == ["--self-test"]:
        _self_test()
        return 0
    return 2


if __name__ == "__main__":
    import sys

    raise SystemExit(main(sys.argv[1:]))
