# Copyright cocotb contributors
# Licensed under the Revised BSD License, see LICENSE for details.
# SPDX-License-Identifier: BSD-3-Clause


TOPLEVEL_LANG ?= verilog

ifneq ($(TOPLEVEL_LANG),verilog)

all:
	@echo "Skipping test due to TOPLEVEL_LANG=$(TOPLEVEL_LANG) not being verilog"
clean::

else

include ../../designs/sample_module/Makefile

# This suite waits on a trigger 100_000 times per test. Tracing those
# iterations costs ~2.5x its runtime and adds no coverage information: the
# same lines run in the first iteration, and neither line nor branch coverage
# records how often a line ran. On the slower CI runners that overhead is the
# difference between fitting the job's 40 minute budget and being killed:
# test_trigger_memory_leak alone took 19 of 30 minutes on Ubuntu and timed a
# macOS job out on its own. The other suites cover the same trigger paths, so
# skip the instrumentation for this one.
export COCOTB_LIBRARY_COVERAGE := 0

COCOTB_TEST_MODULES:=test_trigger_memory_leak

endif
