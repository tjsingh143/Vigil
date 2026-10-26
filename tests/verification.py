"""Unconditional verification checks and an early interpreter-mode boundary."""
import sys


def require_unoptimized():
    if sys.flags.optimize:
        raise RuntimeError("verification refuses Python optimization (-O/PYTHONOPTIMIZE); "
                           "assertions must remain enabled")


def require(condition, detail=None):
    """Raise independently of -O; evaluate failure details only on failure.

    A lazy detail preserves the old checks' behavior when a diagnostic reads
    a subprocess pipe or computes an expensive database/state dump.
    """
    if not condition:
        raise AssertionError(detail() if detail is not None else "verification check failed")


# Import this before fixture imports, argument parsing or any environment work.
require_unoptimized()
