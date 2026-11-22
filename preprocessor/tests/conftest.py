"""Shared test configuration."""

import os

from hypothesis import settings

# CI runs more examples, in a fixed order, so failures reproduce exactly.
settings.register_profile("ci", max_examples=500, derandomize=True)
settings.register_profile("dev", max_examples=100)
settings.load_profile(os.environ.get("HYPOTHESIS_PROFILE", "dev"))
