*********************
GPI Library Reference
*********************

cocotb contains a native library called :term:`GPI` (Generic Procedural Interface)
that is an abstraction layer for the VPI, VHPI, and FLI simulator interfaces.

.. image:: diagrams/svg/cocotb_overview.svg

Cocotb's Python testbench does not run inside the simulator process.
Instead, at simulation start the GPI starts the embedded cocotb IPC server,
which spawns a separate Python process for the testbench.
All simulator interactions—handles, callbacks, logging, and simulation control—travel between the two processes over a local IPC connection,
which keeps the testbench debuggable and profileable like any ordinary Python process.

Each interface library (e.g. ``libcocotbvpi_questa.so``) contains the GPI,
the IPC server, and all interfaces that the simulator's flows can use:
the interface the library is named after is linked statically
and is the primary interface of the run,
while any remaining interfaces are compiled in but resolved from the
simulator's exports at runtime, so only one library file is loaded per
simulation run.

Environment Variables
=====================

.. envvar:: GPI_EXTRA

    A comma-separated list of secondary GPI interface names to activate,
    chosen from ``vpi``, ``vhpi``, and ``fli``.
    Secondary interfaces are activated before elaboration,
    so they can register system functions and callbacks.
    Note that :term:`HDL` objects cannot be accessed at this time.

    This is only needed for mixed-language simulation
    and is normally set automatically by the :ref:`building` system
    and :ref:`api-runner` for the language combination in use.

    For example:

    * ``GPI_EXTRA=vhpi`` activates the VHPI interface alongside the primary VPI interface for a mixed Verilog+VHDL simulation.

    .. versionchanged:: 2.2
        ``GPI_EXTRA`` now takes a list of GPI interface names instead of a
        list of ``path/to/library.so:entry_point`` pairs,
        which are no longer supported.
        Loading additional libraries through the former ``GPI_USERS``
        variable is no longer supported either;
        the cocotb IPC server is embedded in the interface library and
        always started automatically.

C API
=====

.. doxygenfile:: gpi.h
   :sections: brief detaileddescription

User Handles
------------
These types and functions are about handles the GPI provides to users
for interacting with GPI-managed objects.

.. doxygentypedef:: gpi_sim_hdl
.. doxygentypedef:: gpi_iterator_hdl
.. doxygentypedef:: gpi_cb_hdl

GPI Functionality
-----------------

Simulator Control and Interrogation
+++++++++++++++++++++++++++++++++++
.. doxygengroup:: SimIntf

Simulation Object Query
+++++++++++++++++++++++
.. doxygengroup:: ObjQuery

General Object Properties
+++++++++++++++++++++++++
.. doxygengroup:: ObjProps

Signal Object Properties
++++++++++++++++++++++++
.. doxygengroup:: SigProps

Simulation Object Iteration
+++++++++++++++++++++++++++
.. doxygengroup:: HandleIteration

Simulation Callbacks
++++++++++++++++++++
.. doxygengroup:: SimCallbacks

Logging Dependency Injection
++++++++++++++++++++++++++++
.. doxygengroup:: Logging
